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

# The Trix Interactive Debugger

`./trix --inspect FILE` launches an interactive, terminal-based debugger
that halts your Trix program, lets you step through it line by line,
set breakpoints (plain, conditional, one-shot), watch expressions, peek
at every VM stack, and evaluate arbitrary Trix in a sandboxed `:p`
prompt. It is built to be both a production-grade companion tool for
day-to-day Trix development *and* a demonstration of what the language
itself can do: the entire user-facing UI is implemented in Trix
(`lib/debugger.trx`, ~1880 LOC) on top of a thin layer of C++ intrinsics.

## Table of contents

1. [Quick start](#1-quick-start)
2. [Invocation](#2-invocation)
3. [The TUI](#3-the-tui)
4. [Keys cheat sheet](#4-keys-cheat-sheet)
5. [`:` command reference](#5--command-reference)
6. [Tutorial: debug a buggy program](#6-tutorial-debug-a-buggy-program)
7. [Feature deep-dives](#7-feature-deep-dives)
8. [Conditional compilation](#8-conditional-compilation)
9. [How it works](#9-how-it-works)
10. [Limitations](#10-limitations)
11. [Troubleshooting](#11-troubleshooting)
12. [See also](#12-see-also)

---

## 1. Quick start

```text
$ ./trix --inspect myscript.trx
```

The terminal is taken over by a five-pane TUI. The cursor is parked on
line 1 of `myscript.trx`. From here:

| Key   | What it does                                              |
| ----- | --------------------------------------------------------- |
| `s`   | step in (one source-line boundary)                        |
| `n`   | step over (same call depth)                               |
| `o`   | step out (run until current frame returns)                |
| `c`   | continue (run until breakpoint, error, or end)            |
| `b`   | toggle a breakpoint at the proc currently being entered   |
| `:`   | open the command line (e.g. `:b /find-min`, `:p x 2 mul`) |
| `q`   | quit the debugger and resume normal execution             |
| `Tab` | cycle focus across panes                                  |

That's enough to do real work. The rest of this document fills in
everything else.

---

## 2. Invocation

The debugger is gated behind four mutually-cooperating CLI flags:

| Flag | Effect |
| --- | --- |
| `--inspect FILE` | Load `FILE`, arm step-in at line 1, hand the user control before any user code runs. |
| `--inspect-on-error FILE` | Load `FILE`, run normally at full speed, halt when an error is about to go *uncaught* (`try`-caught errors never halt). The session is post-mortem: the FATAL status bar replaces the resume hints, inspection commands all work, and any resume key ends the session -- the error then finishes on the real terminal with its diagnostic and `Error`-enum exit code. Errors in actors/coroutines halt too, on the dying coroutine's own stacks. |
| `--inspect-at=/NAME FILE` | Load `FILE`, pre-set a name-resolution breakpoint on `/NAME`, run until that name is dispatched. |
| `--no-color` | Switch the UI to a bold-only theme (no ANSI 16-color escapes). Useful for monochrome terminals. |

All three `--inspect*` flags imply each other's prerequisites: they
require `FILE` to be a real path on disk (`--inspect` with no filename
is rejected) and they are mutually exclusive with `-i` / `--stdedit`
(interactive REPL mode). The flags themselves are accepted *only* in
TRIX_DEBUGGER-enabled builds (see [§8](#8-conditional-compilation));
release builds reject them with a CLI error.

After `FILE`, any remaining argv tail is forwarded verbatim to the
script via `command-line-args` exactly as it would be in non-inspect
mode.

```text
# Step from line 1.
./trix --inspect examples/debugger_tour.trx

# Run at full speed; halt on the first uncaught error.
./trix --inspect-on-error examples/debugger_tour.trx

# Halt the first time /find-min is invoked.
./trix --inspect-at=/find-min examples/debugger_tour.trx

# Monochrome terminal.
./trix --inspect --no-color examples/debugger_tour.trx
```

If the debugger detects that stdin is not a TTY, it refuses to enter
raw-mode and exits with an error (it would otherwise hang trying to
read keys from a pipe).

---

## 3. The TUI

The terminal is divided into five panes plus a footer:

```text
+----------------------------------------------------------------+
|  myscript.trx                                                  |
|   1   /find-min {|arr|#+3                                      |
|   2       arr length 1 le                                      |
| > 3       { 0 }                          <- PC, highlighted    |
|   4       {                                                    |
|   5           /rest arr 1 arr length 1 sub get-interval ...    |
|   ...                                                          |
+----------------------------------------------------------------+
|  output (3 lines, top-trimmed)                                 |
|  small array:                                                  |
|  single-element:                                               |
+----------------------------------------------------------------+
|  watches                                                       |
|  [0] arr length         = 1                                    |
|  [1] arr 0 get          = 42                                   |
+----------------------------------------------------------------+
|  op[2]  exec[5]  dict[3]  err[0]                               |
|  op:    integer:1   array:[42]                                 |
|  exec:  @call:find-min  ...                                    |
|  dict:  userdict  find-min:locals                              |
|  err:   (empty)                                                |
+----------------------------------------------------------------+
| F11=step F10=over F12=out F5=cont F9=break Tab=focus Enter=expand q=quit |
+----------------------------------------------------------------+
```

### 3.1 Panes

- **Source** (top). Reads `_dbg-source-lines` and renders a viewport
  around the program counter. The PC line is marked with `>` and
  rendered in the `pc` theme color (yellow + reverse + bold by
  default). Lines that hold a breakpoint are tagged with `*` in the
  `bp` color (red + bold). Multi-file programs are handled: the
  pane re-loads source from disk per-sid via the persistent
  sid → path cache (see [§7.6](#76-multi-file-source-pane)).

- **Output** (default 3 rows, configurable). Captures `m_stdout`
  while the user script is running. Whatever the script writes via
  `print` / `=` lands here, top-trimmed at `_dbg-output-max-lines`
  (default 128 entries). On quit, the tail is flushed to the *real*
  terminal so you see what your program actually produced.

- **Watches** (auto-sized). Up to `_dbg-watch-max` entries (default
  8). Each entry is an expression you've registered via `:w EXPR`;
  results are re-evaluated at every halt in a sandbox (save/restore
  brackets, blocklist on side-effecting ops, global-VM result
  capture). Empty when no watches set; pane collapses entirely in
  that case so source absorbs the rows.

- **Stacks** (4 rows). One row each for the operand stack, exec
  stack, dict stack, and error stack. The header shows the depth
  of each (`op[N]  exec[N]  dict[N]  err[N]`). Items in the row are
  pretty-printed previews via `format-object`; per-item width adapts
  to terminal width.

- **Status / command line** (bottom). When you're driving with keys,
  shows the key hints. When you press `:`, becomes an editable
  command line.

### 3.2 Focus

`Tab` cycles focus across `/source`, `/op`, `/exec`, `/dict`, `/err`.
The focused pane has its title rendered in the `focused` theme color;
arrow keys and Page-Up/Page-Down operate on it. In source-focus,
arrows scroll the viewport one line; PgUp/PgDn move a half-page. In
stack-focus, arrows scroll the per-stack preview. Pressing Enter while
a stack pane is focused **expands** it into a full-screen modal where
each item gets a wider `format-object` render — this is how you
inspect deep composites that don't fit in the inline 30-char preview.

### 3.3 Theme

Two themes ship: `color` (ANSI 16-color) and `mono` (bold-only).
`--no-color` selects `mono`; in code you can switch programmatically
with `dbg-set-color-theme` / `dbg-set-mono-theme`. Each theme is a
Dict of `/normal /pc /bp /focused /unfocused /status /title` keys
to `[fg bg attrs]` triples; the attribute byte is a bitmask of
`0x01 bold | 0x02 dim | 0x04 italic | 0x08 underline | 0x10 blink |
0x20 reverse`.

---

## 4. Keys cheat sheet

### 4.1 Normal mode

| Key           | Action     | Effect                                                  |
| ------------- | ---------- | ------------------------------------------------------- |
| `s`, `F11`    | step       | Halt at next source-line boundary, recursing into procs |
| `n`, `F10`    | step-over  | Halt at next line at *current* call depth or shallower  |
| `o`, `F12`    | step-out   | Run until the current `@call` frame returns             |
| `c`, `F5`     | continue   | Run until breakpoint / error / end of program           |
| `b`, `F9`     | break      | Toggle a breakpoint on the proc currently being entered |
| `Tab`         | focus-next | Cycle focus through panes                               |
| `Up`/`Down`   | scroll     | Scroll focused pane by one line                         |
| `PgUp`/`PgDn` | page       | Scroll focused pane by half a screen                    |
| `Enter`       | expand     | When a stack pane is focused, open the modal expand     |
| `Esc`         | collapse   | Close the modal (no-op outside modal)                   |
| `:`           | enter-cmd  | Open the `:` command line                               |
| `q`           | quit       | Uninstall the debugger and let the script run to end    |

### 4.2 Command-line mode (`:`)

| Key                               | Action                                                          |
| --------------------------------- | --------------------------------------------------------------- |
| `Enter`                           | Submit the typed command                                        |
| `Esc`                             | Discard the buffer and return to normal mode                    |
| `Backspace`                       | Delete the last character                                       |
| `Up`/`Down`                       | Step backward / forward through command history (32-entry ring) |
| Printable bytes 0x21-0x7E + Space | Append to the buffer                                            |

Command history persists across halts within one debug session.
Pressing `Up` from a live (in-progress) buffer stashes it so a later
`Down` past the newest entry restores your in-progress edit. Adjacent
duplicates are deduped on submit.

### 4.3 Modal mode

When a stack pane has been expanded with `Enter`:

| Key           | Action                                     |
| ------------- | ------------------------------------------ |
| `Esc`         | Close the modal, return to multi-pane view |
| `Up`/`Down`   | Scroll the modal by one line               |
| `PgUp`/`PgDn` | Page by one screen                         |
| `q`           | Quit the entire debugger                   |

The status bar swaps to a modal-specific hint set so it's clear which
key map is active.

---

## 5. `:` command reference

Type `:` to open the command line. Commands are space-separated; the
first token is the verb, the rest is the argument (or empty).

### 5.1 Breakpoints

```text
:b /NAME [when {PRED}]
```

Set a name-resolution breakpoint at `/NAME`. Whenever the interpreter
dispatches the name `/NAME`, the debugger halts -- at the first
steppable dispatch *inside* the resolved body, so the call's own
frames, operands, and `|locals|` bindings are live in the panes (a
recursive proc therefore halts once per level). With an optional
`when {PRED}` clause, the predicate is sandbox-evaluated at every
candidate halt and only truthy results actually pause execution; falsy
results (false, null, no value, error, blocklisted op) cause a silent
skip and the program resumes in its most recent mode. A bare `:b /name`
*clears* any prior condition on the name and makes the bp unconditional.

```text
:b1 /NAME [when {PRED}]
```

One-shot variant: same grammar, but the breakpoint auto-clears the
first time it actually halts. Predicate-filtered silent skips do *not*
consume the one-shot; the bp keeps firing until the predicate passes.
Combines with conditional bps (the predicate is dropped alongside the
bp on clear).

```text
:clear /NAME
```

Drop the breakpoint, its condition, and its one-shot tag.

```text
:clear-all
```

Drop every breakpoint (and all conditions, all one-shot tags). Reports
how many were cleared.

```text
:bp
```

List all active breakpoints with their hit counts and decorations.
Example output:

```text
/find-min [4 hits] (once) when {arr length 1 le} /helper [0 hits]
```

Hit counts include predicate-filtered silent skips (they count
"how often the name was dispatched while the bp was registered", not
"how often the user actually saw a halt"). Re-setting a bp via
`:b /name` preserves the count; `:clear` drops it.

### 5.2 Sandboxed evaluation

```text
:p EXPR
```

Evaluate `EXPR` in the user-script's dict context, format the top of
the operand stack via `format-object`, render the result on the
command-result row. Bracketed by `save` / `restore` so side effects
roll back; a blocklist rejects ops that would compromise the rollback
(`quit`, `save`, `restore`, `snap-shot`, `coroutine-kill`, etc.). The
formatted result is allocated in global VM so it survives the restore.

```text
:p arr length         -> 6
:p arr 0 get          -> 7
:p { 1 10 add } exec  -> 11
```

If `EXPR` throws inside the sandbox, the error is caught and the
formatted error info is shown instead of the result.

### 5.3 Watches

```text
:w EXPR
```

Register `EXPR` as a watch. At every halt, the debugger re-evaluates
all watches via the same sandbox harness as `:p` and renders the
results in the watches pane (one row per watch). Cap is
`_dbg-watch-max` (default 8); attempting to exceed it is rejected with
a one-line message.

```text
:watches
```

List all watches with their indices and current values.

```text
:clear-watch N
```

Drop the watch at index `N` (0-based). Indices shift after a drop.

```text
:clear-watches
```

Drop every watch.

### 5.4 Frame navigation

```text
:frames
```

List every `@call` frame on the exec stack with its source location
and an `*` marker on the currently selected frame (the walk starts at
the halt point, so the debugger's own frames are excluded). Output
example:

```text
*[0] /find-min:29 |  [1] /find-min:29 |  [2] /find-min:29 |  [3] /find-min:29
```

Per-frame line numbers report the interpreter's *stream position* at
the moment each call was pushed -- for a recursion driven from a
top-level driver that is the driver's line for every level. The frame
*names* are the navigation signal.

```text
:up
```

Move to an older (caller) frame. The source pane re-renders at the
selected frame's source location.

```text
:down
```

Move toward the deepest (callee) frame.

```text
:frame N
```

Jump directly to frame `N`. Reports `no frame at index N` if out of
range.

### 5.5 Help

```text
:help
```

Prints the verb summary inline on the command-result row.

---

## 6. Tutorial: debug a buggy program

The repo ships an intentionally-buggy companion script:
`examples/debugger_tour.trx`. It contains a recursive `/find-min`
that returns 0 for every input — find the bug and fix it without
reading the source.

```text
$ ./trix --inspect examples/debugger_tour.trx
```

The TUI opens with the cursor parked on line 1. Before stepping in,
take stock: hit `c` to let it run to completion and see the
buggy output in the output pane:

```text
small array:        0
single-element:     0
four elements:      0
```

Three zeros, all wrong. Press `q` to exit, restart the inspect
session, and let's hunt.

### Step 1: pin a breakpoint on the suspect

```text
:b /find-min
```

The command result row reports `bp set @ /find-min`. Now `c` runs the
program until the first dispatch of `/find-min` and halts inside it:
the operand stack pane shows `[7i 3i 9i 2i 8i 5i]` and the dict pane
carries the call's `|locals|` frame with `arr` bound. (The source
pane's `>` marker stays on line 29 -- the driver -- because proc-body
elements carry no per-op source position; the panes, not the marker,
tell you where you are.)

### Step 2: watch the recursion drill toward the base case

```text
:w arr length
:w arr 0 get
```

The watches pane now has two rows that auto-refresh at every halt.
The bp halts once per recursion level, so press `c` repeatedly and
watch `arr length` tick down: 6, 5, 4, 3, 2, 1.

When `arr length` reaches `1`, the `{ 0 }` base case is what runs
next and it returns the literal `0`. That's the bug — but let's
confirm it the rigorous way.

### Step 3: a conditional breakpoint

You don't want to step through six recursions every time. Clear the
plain bp and replace it with a conditional one that only halts on the
buggy branch:

```text
:clear /find-min
:b /find-min when {arr length 1 le}
```

`c` to continue. The debugger silently skips the five intermediate
recursions and halts only when `arr length 1 le` is true. The `>`
marker stays on line 29 — the driver — because proc-body elements
carry no per-op PC source position (the same caveat as Step 1 and
§10). Don't read the source marker to confirm the base case; read the
panes: the operand stack shows the single-element array and the dict
pane carries the call's `arr` local. Use `:p arr` (next step) or the
stack panes to confirm you've landed in the buggy branch.

### Step 4: inspect from the halt

```text
:p arr
```

Result: `[5i]` (or `[42i]` for the second driver, or `[25i]` for the
third). Single-element array. The `{ 0 }` branch will return 0;
that's the wrong answer.

`:p arr 0 get` confirms the element is `5i`/`42i`/`25i` — which is
what the proc should be returning.

### Step 5: navigate the recursion stack

While halted in the recursive case, run:

```text
:frames
```

You'll see the recursion stack with one `@call:find-min` frame per
level:

```text
*[0] /find-min:29 |  [1] /find-min:29 |  [2] /find-min:29 |  ...
```

`:up` walks to caller frames; the source pane re-renders at each
selected frame's source location. (Caveat: per-frame line numbers
report the interpreter's stream position when each call was pushed --
line 29, the driver, for every level of this recursion; the frame
*list depth* is what tells you how deep you are.)

### Step 6: fix the bug

The fix is on line 18 of `examples/debugger_tour.trx`. Replace:

```trix
{ 0 }                                  % <- bug lives here
```

with:

```trix
{ arr 0 get }
```

`q` out of the debugger, re-run, and confirm:

```text
$ ./trix examples/debugger_tour.trx
small array:        2
single-element:     42
four elements:      25
```

---

## 7. Feature deep-dives

### 7.1 Conditional breakpoints

`:b /name when {PRED}` stores `PRED` as a *string* in a global-VM Dict
keyed by `/name`. On every dispatch of `/name`:

1. The debugger captures the user-frame state.
2. The predicate string is fed to `_dbg-cmd-eval-bool`, a variant of
   the `:p` sandbox harness that coerces the result to a Boolean.
3. If false (or null, or no value, or threw, or hit a blocklisted op),
   the debugger silent-skips: it consumes no halt budget, drops back
   into the user's most recent mode (step / step-over / step-out /
   continue), and returns control to the interpreter.
4. If true, a normal halt fires.

Predicates can read user-frame locals because the debugger hook fires
*inline* during the user proc's body — the user frame is still on the
dict stack. This is why `arr length 1 le` works inside a `/find-min`
breakpoint without any explicit frame-restoration: the conditional bp
shares the user-script's dict context, sandboxed for side effects.

### 7.2 One-shot breakpoints

`:b1 /name` registers the bp in two places: the main breakpoint Dict
(same as `:b`) *and* the one-shot Dict (`_dbg-bp-oneshot`). On halt,
after the predicate filter and before the halt is rendered, the
debugger checks the one-shot Dict — if the current PC name is in it,
both entries are dropped. Predicate-filtered silent skips happen
*before* this check, so a one-shot conditional bp keeps firing until
the predicate passes for the first time.

`:bp` annotates one-shots with `(once)`. Combined with a `when` clause
the listing becomes `/foo [3 hits] (once) when {i 10 gt}`.

### 7.3 Watches

Each watch is a string. At every cb fire, `_dbg-eval-watches` walks
the watch array and re-evaluates each one through the sandbox. Results
are stashed in `_dbg-watch-results` (parallel array of formatted
strings). The pane renders one row per watch:

```text
[0] arr length      = 1
[1] arr 0 get       = 42
[2] m m null eq     = error: undefined 'm'
```

Errors in a watch render inline as `error: <message>` so a broken
watch doesn't blow up the UI; the others keep evaluating.

### 7.4 Modal pane expand

When a stack pane is focused and you press `Enter`, the debugger
snapshots the stack contents *first* (before any helper literal lands
on the operand stack — `op-stack-snapshot` captures the *entire*
coroutine operand stack, so snapshot ordering matters) and switches
to a full-screen layout that renders one item per line via
`format-object`. Item width matches `screen-cols`
(so it fills whatever the terminal is wide; the format string is
clipped at render time if needed). Up/Down scroll one line; PgUp/PgDn
page. `Esc` closes the modal.

Modal state resets at every halt event so each cb fire starts in the
normal multi-pane layout, regardless of how the previous halt was
left.

### 7.5 Sandboxed `:p` eval

`:p EXPR` evaluation flow:

1. Capture the current save level.
2. Allocate a fresh save scope (`save`).
3. Scan `EXPR` (a string) and execute it.
4. Format the operand-stack top via `format-object` at a generous
   width.
5. Copy the formatted string into global VM (so it survives the
   coming `restore`).
6. `restore` — rolls back any defs, stores, allocations, dict
   mutations, etc. that `EXPR` performed in local VM.
7. Render the captured global-VM result string.

A blocklist rejects ops that would compromise the rollback before they
execute:

- `quit` — would skip the restore.
- `save`, `restore`, `snap-shot` — recursion / state confusion. (The
  four read-only `*-stack-snapshot` ops are *not* blocklisted — they
  cannot compromise the rollback — and `thaw` is not blocklisted
  either.)
- `coroutine-spawn` / `coroutine-die` / `coroutine-kill`,
  `set-global` / `set-stdout`, the `*-persist` family,
  `uninstall-debugger`, the terminal-mode ops (`raw-mode` /
  `cooked-mode`, `alt-screen-*`, `cursor-*`), the filesystem mutators
  (`mkdir` / `rmdir` / `delete-file` / `rename-file` / `chmod`),
  `breakpoint`, and `clear-string-stream` — break the harness model.
- Anything else the sandbox layer flags.

If `EXPR` throws or hits a blocklisted op, the error info is rendered
in place of the result; the restore still happens.

### 7.6 Multi-file source pane

When the program is composed of `require`'d files, the debugger needs
to render source from whichever file the PC currently points at. The
naive approach (walk live streams) breaks because `require`'d files
close their streams after loading — by the time the user halts inside
a proc *defined* in one of those files, the stream is gone and
`stream-name sid` returns null.

Solution: two distinct caches cooperate.

- The **C-side sid → path cache** is a global-VM Dict (the `DebugState`
  `m_sid_path_cache`, created via `create_global_dict` and reachable as
  `debug_sid_path_dict`). It maps each Integer sid to the file's String
  path and is populated at file-open time by `debug_cache_sid_path`.
  Living in global VM, it survives stream close and any save/restore.
  The `stream-name` op queries it, so `stream-name sid` keeps returning
  the path even after a `require`'d file has closed its stream. This
  cache has no Trix-side name.

- The **Trix-side `_dbg-source-cache`** is a plain userdict `64 dict`
  keyed by sid → array-of-source-*lines* (the file split on `\n`). It
  holds the rendered source, not paths, and is filled *lazily on halt*
  by `_dbg-load-source-for-sid`: that helper calls `stream-name` (which
  consults the C-side path cache) to resolve the path, then reads and
  line-splits the file.

On halt, the source pane looks up the current PC's sid in
`_dbg-source-cache`; on a miss it resolves the path via `stream-name`,
reads the file from disk, line-splits it, caches the lines, and renders
the right pane content.

### 7.7 Output capture

While the user script is running, `m_stdout` is redirected to a 32 KiB
writable string-stream (`_dbg-capture-stream`). At every halt, the
captured bytes are drained, split on `\n`, appended to a ring of
completed lines (`_dbg-output-lines`, capped at
`_dbg-output-max-lines = 128`), and rendered in the output pane.
Partial in-progress lines (no trailing newline yet) are also surfaced
so you see "small array:        " while find-min is computing.

On teardown (`q`, `c` to completion, or a fatal halt), `uninstall-debugger`
re-emits the *complete* captured output -- every completed line, the
in-progress partial, and the un-drained tail -- to the real terminal *after*
`alt-screen-leave`, starting on a fresh column-0 line. The alt-screen's own
copy is wiped on exit, so this leaves a normal-run-style record below the
command; the leading fresh line decouples it from the restored cursor, so
terminals that clip the first line at the buffer-switch boundary (VTE / GNOME
Terminal) render it intact. If the output overran the 128-line ring, the
re-emit is prefixed with a truncation marker.

### 7.8 Frame navigation

`frame-source-locs` (C op) walks the exec stack from top to bottom,
emits `[sid line col name]` records for every `@call` marker, and
returns them in top-frame-first order. The debugger calls this once
per halt and stashes the result in `_dbg-frame-locs`. `:up` / `:down`
shift `_dbg-frame-idx`; the source pane re-renders at the selected
frame's source location.

Limitation: per-frame line numbers reflect the call-time
`m_last_scan_location`, which doesn't update during proc-body
dispatch. Frames called from inside an already-scanned proc body all
report the same line as the outermost dispatch. Frame *names* are
always distinct.

### 7.9 Command history

Up/Down arrows in `:`-mode walk a 32-entry ring of past commands.
First Up from a live buffer stashes it; Down past the newest entry
restores it. Adjacent duplicates dedup on push; empty submissions are
skipped. Any non-nav key resets the navigation cursor. History
persists across halts within one session but does not cross sessions
(no on-disk persistence today).

---

## 8. Conditional compilation

The debugger is gated by the `TRIX_DEBUGGER` macro. The build matrix:

| Build invocation         | TRIX_DEBUGGER | Notes                              |
| ------------------------ | ------------- | ---------------------------------- |
| `./build.sh`             | ON            | Default debug build, full debugger |
| `./build.sh --optimized` | OFF           | `./trix.opt`, no debugger          |
| `./build.sh --release`   | OFF           | Release build, no debugger         |

When `TRIX_DEBUGGER` is **off**:

- The `--inspect*` / `--no-color` CLI flags are not recognized at all
  (the option-table entries are compiled out); `getopt` rejects them as
  `unrecognized option`.
- Every C++ op listed in [§9.2](#92-c-intrinsics) is compiled out.
  Their SystemName enum entries are absent, their dispatch cases fold
  away, and `lib/debugger.trx` would fail to load if `require`'d —
  the file references ops that don't exist in the binary.
- The `m_debug` DebugState struct and `m_debug_active` flag are not
  members of the `Trix` struct (zero per-instance overhead).
- The dispatch-loop hook invocation is unreachable code, eliminated
  by the optimizer.

This is enforced by gating every debugger-only entity behind
`#ifdef TRIX_DEBUGGER`, so the whole surface compiles out when
`TRIX_DEBUGGER` is undefined. Files with `TRIX_DEBUGGER` gating:

- `src/api.inl` — the `--inspect*` / `--no-color` longopt entries.
- `src/build_config.inl` — the `TRIX_DEBUGGER` feature flag.
- `src/ops_debugger.inl` — every op body, hook function, DebugState
  struct, intrinsics.
- `src/dispatch.inl` — dispatch-table rows.
- `src/enums.inl` — SystemName enum entries.
- `src/gc.inl` — the `vm-gc-stress` / `vm-gc-poison` GC
  test-harness ops.
- `src/init.inl` — inspect-mode config fields.
- `src/interpreter.inl` — debug_hook invocation in the main dispatch
  loop.
- `src/member_vars.inl` — `m_debug` + `m_debug_active` + the one
  permitted STL container (see [§9.3](#93-stl-carve-out)).
- `src/ops_snapshot.inl` — resets debugger state (`m_debug`,
  `m_debug_active`) when a VM image is thawed.
- `src/save.inl` — prunes the per-op source-line annotation map
  (`m_debug_proc_lines`) on restore so reused VM offsets don't inherit
  stale line arrays.
- `src/scanner.inl` — per-op source-line annotation.
- `src/stream.inl` — `push_inspect_boot` for `--inspect` mode and
  the sid → path cache hook on file open.

In a release build, this entire surface is gone. A `breakpoint` op left
inline in user code is **not** defined there: the scanner doesn't gate on
`TRIX_DEBUGGER`, so the name still scans, but with no enum entry and no
dispatch row it resolves to nothing and raises `/undefined`, aborting the
program. The "halt, or no-op-and-continue when no UI is attached" behavior
exists only in a `TRIX_DEBUGGER`-enabled (debug) build — remove
`breakpoint` from any code shipped to a release build.

---

## 9. How it works

The debugger is a showcase for what Trix can do as a language. Most
of the surface a user touches — the entire UI, every command, the
breakpoint dispatcher, the conditional-bp predicate sandbox, watches,
frame navigation, command history — is written in **Trix itself**
(`lib/debugger.trx`, ~1880 LOC). The C++ side provides only the hook
mechanism, a handful of introspection intrinsics, and the alt-screen
plumbing already needed for `lib/screen.trx`.

### 9.1 Architecture

```text
+----------------------------------------------+
|  user script (myscript.trx)                  |
|     dispatched op-by-op by the interpreter   |
+----------------------------------------------+
       |  every name dispatch + line boundary:
       v
+----------------------------------------------+
|  debug_hook (C++, src/ops_debugger.inl)      |
|    DebugState mode filter:                   |
|      StepIn  -> halt on line change          |
|      StepOver-> halt at <= call depth        |
|      StepOut -> halt when frame returns      |
|      Continue-> only halt on bp / error      |
|    bp dict lookup; on hit, build payload     |
|    [/tag [sid line col pc-name]] and call    |
|    user-installed cb (debug-on-event)        |
+----------------------------------------------+
       |  the cb is _dbg-on-event in Trix:
       v
+----------------------------------------------+
|  lib/debugger.trx                            |
|    output-capture drain                      |
|    conditional-bp predicate filter           |
|    one-shot cleanup                          |
|    watch re-eval                             |
|    frame-locs refresh                        |
|    _dbg-redraw                               |
|    blocking read-key loop:                   |
|      decode-key -> dbg-key-to-action         |
|      action dispatch (step/over/out/cont/    |
|                       break/quit/focus/cmd)  |
|      :-cmd mode: _dbg-cmd-line-edit loop +   |
|                  _dbg-cmd-dispatch verb table |
|    when the user picks step/over/out/cont/   |
|      quit, the cb returns control to the C   |
|      hook which sets DebugState mode and     |
|      lets the interpreter resume             |
+----------------------------------------------+
```

The hook fires **inline** during the user proc's body — typically at
the first name dispatch after `@call` is built. This is why the user
frame's `|locals|` are still in scope at cb time, which is in turn
why conditional bp predicates and `:p` expressions can read user
locals directly without any frame-restoration glue.

### 9.2 C intrinsics

The C++ side exposes ~25 ops, all gated behind `#ifdef TRIX_DEBUGGER`
(plus the `@debug-error-resume` control op):

| Op                     | Stack effect       | Role                                                        |
| ---------------------- | ------------------ | ----------------------------------------------------------- |
| `debug-step`           | `--`               | Set mode StepIn                                             |
| `debug-step-over`      | `--`               | Set mode StepOver                                           |
| `debug-step-out`       | `--`               | Set mode StepOut                                            |
| `debug-continue`       | `--`               | Set mode Continue                                           |
| `debug-break`          | `/name --`         | Register a breakpoint                                       |
| `debug-unbreak`        | `/name --`         | Drop a breakpoint                                           |
| `debug-break-on-error` | `bool --`          | Toggle break-on-error mode                                  |
| `debug-call-depth`     | `-- int`           | Current exec-stack `@call` depth                            |
| `debug-pc`             | `-- name\|null`    | Current PC's dispatched name                                |
| `debug-pc-source`      | `-- int int int`   | Current `[sid line col]`                                    |
| `debug-on-event`       | `proc --`          | Install the cb                                              |
| `debug-breakpoints`    | `-- array`         | Snapshot bp Dict's keys                                     |
| `debug-bp-hits`        | `/name -- int`     | Hit count for a bp                                          |
| `op-stack-snapshot`    | `-- array`         | Copy operand stack                                          |
| `exec-stack-snapshot`  | `-- array`         | Copy exec stack                                             |
| `dict-stack-snapshot`  | `-- array`         | Copy dict stack                                             |
| `err-stack-snapshot`   | `-- array`         | Copy error stack                                            |
| `proc-disasm`          | `proc -- array`    | Disassemble to `[sid line col name preview]` rows           |
| `format-object`        | `obj n -- str`     | Pretty-print at most `n` chars                              |
| `stream-name`          | `sid -- str\|null` | sid → filename (via persistent cache)                       |
| `frame-source-locs`    | `-- array`         | `@call` frames + source locs                                |
| `breakpoint`           | `--`               | Inline halt marker (no-op when debugger inactive)           |
| `vm-gc-stress`         | `bool --`          | GC test harness: force a collection per allocation          |
| `vm-gc-poison`         | `bool --`          | GC test harness: poison freed slots to catch use-after-free |

Everything else — the rendering, the key handling, the command
dispatch, the conditional-bp predicate eval, the sandbox, the watches,
the frame navigation, the modal expand — is pure Trix.

### 9.3 STL carve-out

One bit of debugger state cannot reasonably live in VM space: the
per-op source-line annotation map (`m_debug_proc_lines`,
`std::unordered_map<vm_offset_t, std::vector<int32_t>>`). It is keyed
by VM-resident offset but otherwise has no VM-Object analog (a Dict
of `Object → Array` would force VM-Object packing for every integer
line number).

This is the *only* `std::` container in the active codebase that
allocates on the C heap. It is gated by `#ifdef TRIX_DEBUGGER` and the
carve-out rule in [STYLE.md](../STYLE.md): heap-allocating
STL is permitted in debugger-only code conditionally-compiled out of
release builds, when the data has no natural VM-Object analog or the
VM-resident equivalent would be materially more complex with no
debugger-UX benefit.

### 9.4 Showcase callouts

A few facts about the implementation worth surfacing:

- **The bp registry is a global-VM Dict.** `Dict::create_global_dict`
  produces a Dict in the journaled-out global VM. Breakpoints set
  during a save scope survive `restore`; the registry never rolls
  back. Same goes for the conditional-bp predicate strings (kept in
  global VM for the same reason) and the sid → path cache.

- **Conditional bp predicates are real Trix.** They're sandboxed
  through `_dbg-cmd-eval-bool`, which is the same `save` / `restore`
  + global-VM result + blocklist harness as `:p`. Predicates can
  reference user-frame locals, dispatch user procs, do arithmetic —
  whatever Trix supports, subject to the blocklist on irrecoverable
  ops.

- **The sandbox uses the language's own primitives.** `save` /
  `restore` and the blocklist were not built for the debugger; they
  exist as user-facing VM features (see `docs/save-restore.md`). The
  debugger composes them — its sandbox is ~20 LOC of Trix, not a new
  C++ subsystem.

- **The UI is reload-friendly.** Because `lib/debugger.trx` is
  loaded at install time and lives in userdict, you can edit it
  during development and re-`require` without recompiling C++. The
  C-side hook contract (event payload format) is stable; everything
  else is malleable Trix.

- **`format-object` is the same one user code uses.** No separate
  "debug printer." The pretty-print the debugger shows in the stack
  panes and the modal expand is identical to what `format-object`
  produces in normal user code. If the printer ever drifts, the
  debugger drifts with it — there's no shadow implementation to
  maintain.

---

## 10. Limitations

Known wrinkles, kept as open follow-ups rather than blocked work:

- **Step-out from the topmost user proc.** Stepping out when the
  current `@call` is the outermost user frame transitions back to the
  interpreter loop in a way the line-change filter doesn't notice.
  Workaround: use Continue.

- **Per-frame line numbers.** `frame-source-locs` reports each frame's
  *call-site* line — set when the `@call` marker was constructed —
  not its current PC inside the proc body. Frame names are always
  distinct, so this affects only the visual hint in the source pane,
  not which frame you've selected.

- **Multi-line tokens report END line, not START line.** A string
  literal that spans lines 4–6 will be annotated with line 6 in
  `proc-disasm`. The annotation is still a *valid* source line for
  that op, just not the one a reader would jump to.

- **Global Dict growth is GC-clean.** Long-running debug sessions
  that expand the breakpoint Dict or the sid → path cache enough to
  trigger a Dict grow do *not* leak global-VM pool blocks. Since the
  2026-05-14 region-aware `expand_dict` / `expand_set` fix, the
  default GC walker back-walks each global Dict's bucket-chain and
  free-list entries to the owning expansion block
  (`gvm_find_owning_payload`) and marks it, so global Dicts grow with
  no dangling or garbage auxiliary pool blocks. No manual
  `vm-global-gc` is required.

---

## 11. Troubleshooting

**Terminal looks corrupted after the program crashes inside the
debugger.** The crash path skipped `uninstall-debugger`, leaving the
terminal in raw-mode + alt-screen. Run `reset` or close and reopen the
terminal.

**`./trix --inspect FILE` errors with `unrecognized option`.** You're on a
release build where TRIX_DEBUGGER is off. Build with `./build.sh`
(no `--release` / no `--optimized`) to get a debug build with the
debugger enabled.

**The script runs out of VM during inspect.** The debugger lives in
local VM and adds ~150-300 KB of state (source-line cache, watch
results, output ring, frame locs, command history). For scripts that
were already near the 1 MB default, add `--vm-size=2M`.

**A conditional bp predicate is being silently rejected.** Check that
the predicate doesn't reference an op on the blocklist (`save`,
`restore`, `quit`, `snap-shot`, `coroutine-kill`, etc.) and
that it actually leaves a Boolean on the stack. The `:p` command
shows error details inline; `:p` your predicate body directly to debug
it before bp-installing it.

**`stream-name` returns null for a file I loaded.** The file may not
have been `require`'d before the halt — the sid → path cache is
populated on file *open*, not at scan time. Cache survives close, but
files never opened during the run won't be in it.

**`op-stack-snapshot` is undefined.** You're on a release build, or
running `./trix.opt`. The snapshot ops are TRIX_DEBUGGER-gated.

**Output disappears between halts.** That's expected — the output
pane shows up to `_dbg-output-max-lines = 128` of the most recent
captured lines (top-trimmed). The full tail flushes to the real
terminal on `q`.

---

## 12. See also

- `lib/debugger.trx` — full UI source (~1880 LOC, pure Trix).
- `src/ops_debugger.inl` — C intrinsics + hook plumbing.
- `examples/debugger_tour.trx` — the buggy companion script for the
  tutorial in [§6](#6-tutorial-debug-a-buggy-program).
- `docs/save-restore.md` — the save/restore primitives the sandbox
  composes.
- `docs/gvm-heap-gc.md` — the global VM region the breakpoint
  registry, conditional-bp predicate strings, and sid → path cache
  live in.
- `docs/scanner.md` — `proc-disasm` row layout and per-op source-line
  annotation provenance.
