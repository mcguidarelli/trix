<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Terminal I/O in Trix

Trix supports terminal user interfaces (TUIs) — single-key input without
Enter, ANSI/CSI output for cursor positioning, color, and screen control.
This document is a how-to for building a TUI program in Trix; see
[`docs/trix-reference.md` § 3.17](trix-reference.md#317-io-and-streams)
for the operator reference.

The committed showcases that depend on this surface are
`tetrix.trx`, `raycaster.trx`, and `chip8.trx`.

---

## 1. The surface

| Op                      | Phase | Stack effect               | Purpose                      |
| ----------------------- | ----- | -------------------------- | ---------------------------- |
| `raw-mode`              | 1     | `--`                       | Switch stdin tty to raw mode |
| `cooked-mode`           | 1     | `--`                       | Restore canonical mode       |
| `raw-mode?`             | 1     | `-- bool`                  | Current mode predicate       |
| `read-key-byte`         | 2     | `-- byte true \| false`    | Blocking single-byte read    |
| `read-key-byte-timeout` | 2     | `ms -- byte true \| false` | Same with deadline           |
| `key-ready?`            | 2     | `-- bool`                  | Non-blocking poll            |
| `terminal-size`         | 3     | `-- cols rows`             | Query controlling tty size   |

**Sandbox:** all six host ops raise `/unsupported` in `--sandbox` mode.
`raw-mode?` is allowed (read-only state, no host effect).

**Output** is just bytes written to stdout — Trix has no special "ANSI op."
Standard CSI sequences are written via `print`, `write-string`, or
`print-fmt`.  See [§ 4](#4-output-ansi-cookbook).

---

## 2. The shape of a TUI program

The minimum well-behaved TUI program looks like this:

```trix
(lib/keys.trx) run

{
    % program body: alt-screen-buffer, cursor-hide, main loop
    (\e[?1049h) print          % enter alt-screen-buffer
    (\e[?25l) print            % hide cursor

    {
        % main loop: read a key, dispatch
        { read-key-byte } { 50 read-key-byte-timeout } decode-key
        dup /q eq { exit } if
        % ... handle the key, redraw frame ...
        pop
    } loop

    (\e[?25h) print            % show cursor
    (\e[?1049l) print          % leave alt-screen-buffer
} with-raw-mode
```

`with-raw-mode` (in `lib/keys.trx`) wraps the body in `raw-mode` /
`cooked-mode` with a try/finally pattern, so even an uncaught error
restores the terminal before propagating.  The signal handler installed
by `raw-mode` itself covers Ctrl-C / SIGTERM / SIGHUP / SIGQUIT and an
`atexit(3)` hook covers `assert()` / `abort()` / normal `exit()`.

What you do **not** need to manage manually: termios save/restore,
signal handler installation, atexit registration, O_NONBLOCK toggling on
stdin.  The Phase 1/2 ops handle all of that.

What you **do** need to manage manually: alt-screen-buffer enter/exit,
cursor hide/show, color reset on exit (per the cookbook below) — unless
you build on the virtual screen, in which case
`with-fullscreen-screen` ([§ 4.6](#46-the-library-layer-libansitrx-and-libscreentrx))
automates exactly these around a terminal-sized cell buffer.

---

## 3. Decoding keystrokes — `lib/keys.trx`

`read-key-byte` returns one byte at a time.  Single keystrokes are often
multi-byte sequences: arrow keys are `ESC [ A`, function keys are
`ESC [ 1 5 ~`, etc.  `lib/keys.trx` provides:

```
decode-key      block-src poll-src -- key-name
with-raw-mode   proc --
```

### `decode-key`

`block-src` and `poll-src` are procs with stack effect
`( -- byte true | false )`.  The decoder uses `block-src` for the first
byte of a keystroke (so the program blocks until input is available) and
`poll-src` for any continuation bytes (so a bare `ESC` keypress times
out and resolves to `/escape` rather than hanging on the user's release).

Real use:
```trix
{ read-key-byte } { 50 read-key-byte-timeout } decode-key
```

Tests use a synthetic byte source (a proc that yields from a
pre-recorded array) — see `tests/test_keys_trx.trx`.

### Returned key names

| Input bytes                 | Key name                                                         |
| --------------------------- | ---------------------------------------------------------------- |
| 0x20..0x7E printable        | `/<char>` (the byte literally — `/a`, `/Q`, `/!`, `/~`)          |
| 0x01..0x1A control letters  | `/ctrl-a` .. `/ctrl-z` (except 0x09, 0x0A, 0x0D below)           |
| 0x09 (TAB)                  | `/tab`                                                           |
| 0x0A or 0x0D                | `/enter`                                                         |
| 0x7F                        | `/backspace`                                                     |
| 0x1B (no continuation)      | `/escape`                                                        |
| `ESC [ A` / `B` / `C` / `D` | `/up` / `/down` / `/right` / `/left`                             |
| `ESC [ H` / `F`             | `/home` / `/end`                                                 |
| `ESC [ 1 ~` / `4 ~`         | `/home` / `/end` (alt encoding)                                  |
| `ESC [ 2 ~` / `3 ~`         | `/insert` / `/delete`                                            |
| `ESC [ 5 ~` / `6 ~`         | `/page-up` / `/page-down`                                        |
| `ESC O P` / `Q` / `R` / `S` | `/f1` / `/f2` / `/f3` / `/f4`                                    |
| `ESC [ 15 ~` / `17 ~` / ... | `/f5` / `/f6` / `/f7` / `/f8` / `/f9` / `/f10` / `/f11` / `/f12` |
| Block-src returns false     | `/eof`                                                           |
| Anything else               | `/unknown`                                                       |

The decoder is intentionally minimal — covers the keys that 90 % of
TUI programs need.  Extending the table for terminal-specific quirks
(rxvt, mosh) is a one-line edit at the call site since the decoder lives
in Trix, not C++.

---

## 4. Output: ANSI cookbook

Trix has no ANSI ops — terminal control is just bytes written to stdout.
The sequences in this section are standard CSI / SGR / DEC-private
escapes that work on every modern terminal (xterm, iTerm2, Windows
Terminal, alacritty, kitty, tmux, screen).

### Alt-screen-buffer (recommended for full-screen TUIs)

```trix
(\e[?1049h) print          % enter; the user's scrollback is preserved
... your program ...
(\e[?1049l) print          % leave; their scrollback comes back
```

### Cursor hide/show

```trix
(\e[?25l) print            % hide
(\e[?25h) print            % show -- ALWAYS pair with hide on exit
```

### Cursor positioning

<!-- doctest: skip (placeholder row/col cursor-position illustration) -->
```trix
% Move cursor to row R, column C (1-based, both):
(\e[) print row print (;) print col print (H) print
% or with aprint-fmt (fmt first, then the args array; it returns a
% count and an ok flag -- pop them):
(\e[{0};{1}H) [ row col ] aprint-fmt pop pop
```

### Clear screen / clear line

```trix
(\e[2J) print              % clear entire screen
(\e[H) print               % home cursor (row 1 col 1)
(\e[K) print               % clear from cursor to end of line
```

### Color (16-color SGR)

```trix
(\e[31m) print             % red foreground
(\e[42m) print             % green background
(\e[1m) print              % bold
(\e[0m) print              % RESET -- always emit on exit / between regions
```

### Color (256-color)

<!-- doctest: skip (placeholder color-index N illustration) -->
```trix
% Foreground color N (0..255):
(\e[38;5;{0}m) [ N ] aprint-fmt pop pop
% Background:
(\e[48;5;{0}m) [ N ] aprint-fmt pop pop
```

### Color (24-bit truecolor)

<!-- doctest: skip (placeholder R/G/B truecolor illustration) -->
```trix
(\e[38;2;{0};{1};{2}m) [ R G B ] aprint-fmt pop pop   % foreground
(\e[48;2;{0};{1};{2}m) [ R G B ] aprint-fmt pop pop   % background
```

### Box-drawing

Use ASCII glyphs (`+`, `-`, `|`) per project memory `feedback_ascii_only.md`.
Unicode box-drawing (`─│┌┐`) prints fine on modern terminals but is
prohibited by Trix project convention.

---

## 4.5 Output: virtual screen with diff render (`make-screen`)

Raw ANSI works fine for simple full-screen redraws, but a TUI that updates
a few cells per frame (a scrolling map, a live HUD, a Tetris board)
should not retransmit the entire screen each tick.  Trix ships a curses-
equivalent virtual cell buffer with a built-in diff renderer.

The full `screen-*` surface (14 ops, OpaqueHandle-backed):

| Op | Stack effect | Purpose |
| --- | --- | --- |
| `make-screen` | `cols rows -- screen` | Allocate a virtual cell buffer |
| `screen-cols` | `screen -- cols` | Current width |
| `screen-rows` | `screen -- rows` | Current height |
| `screen-clear` | `screen -- screen` | Reset every cell to the default |
| `screen-resize` | `screen new-cols new-rows -- screen` | Reallocate (intersection preserved) |
| `screen-put-cell` | `screen col row codepoint fg bg attrs -- screen` | Write one cell |
| `screen-put-string` | `screen col row str fg bg attrs -- screen` | Write a Latin-1 run (truncates at edge) |
| `screen-put-utf8-string` | `screen col row str fg bg attrs -- screen` | Write a UTF-8 run (one cell/codepoint) |
| `screen-fill-rect` | `screen x y w h codepoint fg bg attrs -- screen` | Paint a clipped rectangle |
| `screen-get-cell` | `screen col row -- ch fg bg attrs` | Read one cell back |
| `screen-blit` | `src sx sy w h dst dx dy -- dst` | Rect copy (clipped; same-screen safe) |
| `screen-render` | `screen -- screen` | Diff-render to stdout (sandbox-gated) |
| `screen-render-to` | `screen stream -- screen` | Diff-render to a writable stream |
| `screen-park-cursor` | `col row --` | Park the terminal cursor (sandbox-gated) |

Use `screen-cols`/`screen-rows` to query the buffer's size instead of the
per-frame `terminal-size` poll in [§ 5](#5-resizing), and `screen-resize`
to react to a terminal-size change.  A minimal session:

```trix
80 24 make-screen /scr exch def      % allocate 80x24 cell buffer

% Mutate the buffer: every put-cell / put-string / fill-rect targets
% the in-memory cells, NOT the terminal.
scr 10 5 (Hello, world!) 7 0 0 screen-put-string pop
scr 0 0 80 1 95 7 0 0 screen-fill-rect pop  % '_' across top row

% Render: walk cells vs the previous-render snapshot, emit only the
% changed cells (with cursor moves + minimal SGR sequences).  First
% render of a freshly-made screen paints every cell; subsequent renders
% emit only the diff.
scr screen-render pop
```

Each cell is 8 bytes: `uint32 ch, uint8 fg, uint8 bg, uint8 attrs, pad`.
Color is the 256-color SGR palette (truecolor deferred).  `attrs` is a
bitfield: bit 0 bold, 1 dim, 2 italic, 3 underline, 4 blink, 5 reverse,
6 strike.  Cells default to space + light-gray fg + black bg + no attrs;
`screen-clear` resets to that default.

`screen-render` is sandbox-gated (touches stdout via `::write()`).  For
testing rendered output without a terminal, use `screen-render-to stream`
with a `make-string-stream` capture buffer to assert exact byte output.

The render output is UTF-8: ASCII codepoints emit one byte, multi-byte
codepoints (e.g. the Unicode box-drawing range U+2500..U+257F) emit 2-4
bytes.  This is the **carve-out** to `feedback_ascii_only.md` for
terminal-output `.trx` files: string literals destined for the screen
may contain UTF-8 byte sequences.  C++ source remains strictly ASCII.

`make-screen` allocates ScreenState + cells + prev on the VM heap with
no pool or SID; save/restore reclaims any allocations made above the
save mark normally.

---

## 4.6 The library layer: `lib/ansi.trx` and `lib/screen.trx`

Sections 4 and 4.5 document the raw material — escape strings and the
`screen-*` C ops.  Two library files wrap that material into the procs
a TUI program actually calls.  Load them with `require` (idempotent:
one execution per canonical path, so libraries can `require` each
other freely):

```trix
(lib/screen.trx) require       % pulls in lib/ansi.trx via its own require
(lib/keys.trx) require         % input decoding -- section 3
```

One convention difference to keep straight when mixing the layers:

| Layer                                   | Coordinates       | Matches           |
| --------------------------------------- | ----------------- | ----------------- |
| `lib/ansi.trx`                          | 1-based `col row` | the ANSI/CSI spec |
| `lib/screen.trx` and the `screen-*` ops | 0-based `col row` | the cell buffer   |

### `lib/ansi.trx` — CSI/SGR emitters

Every cookbook sequence from [§ 4](#4-output-ansi-cookbook) as a named
proc.  All helpers write through the rebindable stream variable
`ansi-out` (default `stdout`): tests redefine `ansi-out` to a
string-backed write stream and assert on the captured bytes; production
code leaves it alone.  Every proc consumes its arguments and pushes
nothing.

| Group | Procs |
| --- | --- |
| Cursor | `col row move-to`, `n move-up` / `move-down` / `move-left` / `move-right`, `cursor-save` / `cursor-restore` (DEC ESC 7/8), `cursor-hide` / `cursor-show` |
| Erase | `clear-screen` (erase + home), `clear-line`, `clear-to-eol`, `clear-to-bol` |
| Alt screen | `alt-screen-enter` / `alt-screen-leave` |
| SGR attrs | `bold`, `dim`, `italic`, `underline`, `blink`, `reverse`, `strike`, `reset-attrs` |
| Color (16) | `n fg-16` / `n bg-16` (0..7 normal, 8..15 bright) |
| Color (256) | `n fg-256` / `n bg-256` |
| Truecolor | `r g b fg-rgb` / `r g b bg-rgb`, `default-fg` / `default-bg` |
| Composites | `col row str print-at`, `attr-list proc with-attrs`, `n proc with-fg`, `n proc with-bg` |

The `with-*` composites scope their effect: apply, run proc, then reset
(`reset-attrs` / `default-fg` / `default-bg`):

```trix
(lib/ansi.trx) require

10 5 (status:) print-at
[ /bold /reverse ] { 18 5 (ALERT) print-at } with-attrs
196 { 25 5 (disk full) print-at } with-fg       % 256-color red
```

Color and attr arguments are not validated — feed palette indices in
range (`0..15` for `fg-16`/`bg-16`, `0..255` elsewhere).

### `lib/screen.trx` — TUI lifecycle wrappers

| Proc | Stack effect | Purpose |
| --- | --- | --- |
| `with-fullscreen-screen` | `proc --` | Full TUI setup/teardown; terminal-sized screen on the stack |
| `with-screen` | `cols rows proc --` | Allocate a screen, run proc with it — no terminal-mode side effects |
| `screen-fill` | `screen ch fg bg attrs -- screen` | Fill every cell (whole-screen `screen-fill-rect`) |
| `screen-from-terminal` | `-- screen` | `terminal-size`-sized `make-screen` |

`with-fullscreen-screen` is the entry point the showcases use
(`tetrix.trx`, `chip8.trx`, `raycaster.trx`, `screen-demo.trx`): it
runs `raw-mode` → `alt-screen-enter` → `cursor-hide`, allocates a
screen sized to the terminal, pushes it, and runs proc.  Teardown runs
on **every** exit path — normal completion, caught error, uncaught
error (rethrown after the terminal is restored) — in reverse order,
so the terminal returns through clean intermediate states.

Two wire-ordering details the wrapper absorbs so you don't have to:

- `ansi-out` writes are stream-buffered while `screen-render` writes
  straight to the fd; the wrapper flushes between its ANSI setup and
  your first `screen-render` so setup bytes can't land after render
  bytes.
- Teardown restores `cooked-mode` *before* the final flush: `raw-mode`
  puts stdin on `O_NONBLOCK`, which also affects stdout (one shared
  open file description), and flushing a still-draining tty in that
  state can surface `/io-write-error`; cooked mode first makes the
  final flush blocking.

`with-screen` is the headless variant — combine it with
`screen-render-to` and a string stream to assert exact rendered bytes
([§ 4.5](#45-output-virtual-screen-with-diff-render-make-screen)).

A complete interactive skeleton, all three libraries together:

```trix
(lib/screen.trx) require
(lib/keys.trx) require

{                                       % screen arrives on the stack
    /scr exch def
    scr 32 7 0 0 screen-fill pop        % space, light-gray on black
    scr 2 1 (Hello -- press q) 7 0 1 screen-put-string pop
    {
        scr screen-render pop
        { read-key-byte } { 50 read-key-byte-timeout } decode-key
        /q eq { exit } if
    } loop
} with-fullscreen-screen
```

---

## 5. Resizing

Phase 1 has no `SIGWINCH` listener.  If your program needs to react to
terminal resizes, call `terminal-size` once per frame and re-layout when
the result changes:

```trix
/last-cols 0 def
/last-rows 0 def

% in main loop:
terminal-size                    % cols rows
dup last-rows ne
{
    /last-rows exch def
    /last-cols exch def
    redraw                       % re-layout for new size
}
{ pop pop } if-else
```

This adds one syscall per frame — negligible compared to the rendering
work the frame already does.

If you drive the display through a `make-screen` buffer
([§ 4.5](#45-output-virtual-screen-with-diff-render-make-screen)), call
`screen-resize` with the new dimensions and let the next `screen-render`
repaint; `screen-cols` / `screen-rows` report the buffer's current size.

---

## 6. Common patterns

### Frame timer with selective input

<!-- doctest: skip (frame-timer pattern with placeholder handlers; read-key-byte-timeout needs a tty) -->
```trix
% Wait up to 16 ms for a key (60 fps), then advance the game frame.
16 read-key-byte-timeout
{ % byte true on stack -- handle the key
    /handle-key store
    handle-key
}
{ % false on stack -- timer fired with no input
    advance-frame
}
if-else
```

### Drain pending input

<!-- doctest: skip (interactive terminal input; needs a tty) -->
```trix
% Consume any keys queued in stdin without blocking.  Useful at the
% start of a frame to discard mid-frame keystrokes that no longer apply.
{
    key-ready?
    { 0 read-key-byte-timeout pop pop }
    { exit }
    if-else
} loop
```

### Chained-up-to-ms timeout

```trix
% Read a multi-byte sequence (e.g. a paste) by reading repeatedly
% with a short inter-byte timeout.
[
    {
        10 read-key-byte-timeout
        { } { exit } if-else
    } loop
]
```

---

## 7. Snapshot-and-resume

Per memory `plan_raw_mode_keyboard.md` decision D5, snapshots are
mode-agnostic and thaw lands in cooked mode.  The thawed program is
responsible for calling `raw-mode` again if it wants raw input back.
The `with-raw-mode` wrapper handles this automatically as long as the
snapshot was taken from inside its body and the resume path re-enters
through the same wrapper.

```trix
% Quicksave from inside a TUI:
{ snap-shot } with-raw-mode

% Thaw lands in cooked mode -- re-arm raw mode at resume:
{ ... resumed program ... } with-raw-mode
```

---

## 8. Sandbox interaction

The host-touching ops (`raw-mode`, `cooked-mode`, `read-key-byte`,
`read-key-byte-timeout`, `key-ready?`, `terminal-size`) all raise
`/unsupported` when Trix is started with `--sandbox`.  This matches the
gating on `system`, `shell`, `chmod`, `chdir`, etc.  Library / fuzzer
embedders who don't want a TTY-aware language get a clean failure mode.

`raw-mode?` is allowed in sandbox: it inspects in-memory state and has
no host effect.

---

## 9. Platform notes

Phase 1–4 ship on Linux and macOS via `termios`.  Native Windows is
deferred — Trix already excludes MSVC builds at compile time
(`src/types.inl:42-46`), so deferring Windows raw-mode inherits an
existing project posture.  Modern Windows users run Trix under WSL,
where the Linux termios path Just Works.

When `terminal-size` is called from a non-tty stdin (piped script, CI,
Docker without `-t`, etc.) it raises `/io-read-error`.  Programs that
want to fall back to a default canvas size should check `interactive?
m_stdin` (or wrap `terminal-size` in a `try-catch`).

---

## See also

- `docs/trix-reference.md` § 3.17 — operator reference for the full I/O surface.
- `lib/keys.trx` — the escape decoder + `with-raw-mode` wrapper.
- `lib/ansi.trx` — CSI/SGR emitter procs over the § 4 cookbook ([§ 4.6](#46-the-library-layer-libansitrx-and-libscreentrx)).
- `lib/screen.trx` — `with-fullscreen-screen` and friends ([§ 4.6](#46-the-library-layer-libansitrx-and-libscreentrx)).
- `examples/screen-demo.trx` — guided tour of the virtual-screen + library stack.
- `examples/keytest.trx` — manual smoke test that prints the decoded
  Name for each keystroke.
- `tests/test_keys_trx.trx` — synthetic byte-source tests of the decoder.
