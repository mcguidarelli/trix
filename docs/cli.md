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

# Running Trix: Command-Line Reference

Updated: 2026-06-04

This is the user-facing guide to the `trix` command line: how to launch the
interpreter, every option it accepts, where it looks for modules, what the
sandbox blocks, and how the process exit code maps to errors. It is a
companion to the per-feature guides -- for what the *language* does once it is
running, see [the documentation index](index.md). For the C++ embedding view
of the same configuration knobs (the `Config` struct), see
[host-integration.md](host-integration.md).

The authoritative source for the option surface is `trix --help`; this
document mirrors and explains it. Every flag below appears in that output.

---

## Table of Contents

1. [Synopsis](#1-synopsis)
2. [Startup modes](#2-startup-modes)
3. [Option reference](#3-option-reference)
4. [Module search path and `--module-path`](#4-module-search-path-and---module-path)
5. [The `--sandbox` flag](#5-the---sandbox-flag)
6. [Debugger flags](#6-debugger-flags)
7. [Snapshot images from the CLI](#7-snapshot-images-from-the-cli)
8. [Exit codes](#8-exit-codes)
9. [Where to go next](#9-where-to-go-next)

---

## 1. Synopsis

```
trix [options] [filename] [script-args...]
```

- **`[options]`** -- zero or more flags from [section 3](#3-option-reference).
  Options must come *before* the filename.
- **`[filename]`** -- a `.trx` script to run, or (with `-l` / `--image`) a
  snapshot image to resume. If omitted, `trix` starts an interactive REPL.
- **`[script-args...]`** -- everything after the filename is **not**
  interpreted by `trix`. Those tokens are handed to the script unchanged and
  surfaced by the `command-line-args` operator. Option parsing stops at the
  first non-option argument (the filename), so flags after the filename also
  belong to the script, not to `trix`.

Passing arguments through to a script:

```trix
command-line-args ==
```

```console
$ trix args.trx alpha beta 42
[(alpha)#lr (beta)#lr (42)#lr]
```

The three tail tokens arrive as Trix strings; the script is responsible for
parsing them (`string->integer`, etc.). See
[host-integration.md](host-integration.md) for the `command-line-args`
operator's exact stack effect.

Two flags exit immediately after printing:

```console
$ trix --version
trix 0.9.0 — the cat in concatenative
build: debug; sanitizers: address; features: heap-tracking debugger backtrace

$ trix --help
Usage: trix [options] [filename] [script-args...]
...
```

`--about` prints the same banner plus compiler, C++ standard, snapshot-format
version, and the full default-configuration table (the defaults listed in
[section 3](#3-option-reference)). All three (`--version`, `--help`,
`--about`) exit with status 0.

---

## 2. Startup modes

The combination of filename and flags selects one of five modes. `trix`
resolves them as follows:

| Invocation | Mode | Behavior |
| --- | --- | --- |
| `trix script.trx` | **Script file** | Run the file, then exit. |
| `trix` (no filename), or `trix -i` | **Interactive REPL** | Read-eval-print loop on the terminal. |
| `trix --stdin` | **Standard input** | Read a whole program from stdin (for piped one-liners). |
| `trix -l image.img` | **Image resume** | Resume a snapshot image (see [section 7](#7-snapshot-images-from-the-cli)). |
| `trix -i script.trx` | **File then REPL** | Run the file, then drop into the REPL with that state loaded. |

`--stdin` is mutually exclusive with both `-i` and a filename; combining them
is a usage error:

```console
$ echo "1 =" | trix --stdin /tmp/x.trx
trix: --stdin and filename are mutually exclusive
$ echo "1 =" | trix --stdin -i
trix: --stdin and -i/--stdedit are mutually exclusive
```

### 2.1 Script file

The everyday case -- run a program and exit:

```trix
1 2 add =
```

```console
$ trix add.trx
3
```

### 2.2 Interactive REPL

With no filename, `trix` starts an interactive REPL. Use `-i` / `--stdedit`
to force the REPL even when other arguments are present, or to run a file and
*then* stay interactive (see 2.5). The REPL prints a banner naming the
version, VM size, operator count, and snapshot-image version, then a
`Trix> ` prompt; type `quit` or press Ctrl-D to leave.

```console
$ trix -i
Trix 0.9.0 -- 1M VM -- 828 ops (986 total) +3 user, image v176
(ctrl-D or 'quit' to exit)
Trix> 2 3 mul =
6
Trix> quit
```

The `+N user` field counts host-installed operators; the reference `trix`
binary ships three example ops (`my-square`, `my-clamp`,
`my-raise-interrupt`), so it reads `+3 user`.

Line editing and history come from GNU readline.  A `TRIX_NO_READLINE` build
(`build.sh --no-readline`) keeps the REPL but drops readline: the `Trix> `
prompt then reads each line from stdin with no editing or history.

`-q` / `--quiet` suppresses the banner (and all other diagnostic output --
see [section 3](#3-option-reference)):

```console
$ trix -q -i
Trix> 10 20 add =
30
Trix>
```

`--no-banner` suppresses only the banner: error reports and backtraces
still print, which is what a script driving an interactive session
usually wants (`--quiet` would swallow the errors too):

```console
$ trix -i --no-banner
Trix> 1 0 div
Trix div-by-zero 'div': div: division by zero
Trix>
```

Each line you type is scanned and executed as it is entered, so the operand
stack persists between prompts -- the REPL is a live session, not a sequence
of independent programs.

### 2.3 Standard input (`--stdin`)

`--stdin` reads an entire program from standard input and runs it as if it
were a file. This is the mode for shell pipelines and `here-doc` one-liners.
There is no prompt and no banner:

```console
$ echo "1 2 add =" | trix --stdin
3
```

```console
$ trix --stdin <<'EOF'
/sq { dup mul } def
7 sq =
EOF
49
```

### 2.4 Image resume (`-l` / `--image`)

`-l` (or `--image`) treats the filename as a snapshot **image** rather than a
script, and resumes the VM from exactly where `snap-shot` was called. This is
covered in [section 7](#7-snapshot-images-from-the-cli).

### 2.5 File, then REPL

`-i` together with a filename runs the file first and then enters the REPL
with the resulting VM state -- definitions, stack, and dicts from the
script are all available at the prompt. This is handy for loading a library
and then poking at it interactively:

```trix
/greeting (hello) def
```

```console
$ trix -i greet.trx
Trix> greeting =
hello
Trix> quit
```

---

## 3. Option reference

Every option below appears in `trix --help`. Options come in three groups:
behavior flags, the debugger flags (only present in debugger builds -- see
[section 6](#6-debugger-flags)), and VM-tuning knobs. The defaults and ranges
listed here are the canonical values compiled into the binary; `--about`
prints the live defaults for your build.

### 3.1 Behavior flags

| Flag | Argument | Effect |
| --- | --- | --- |
| `-h`, `--help` | -- | Print usage and exit 0. |
| `-v`, `--version` | -- | Print version + build info and exit 0. |
| `--about` | -- | Print extended version/build/config info and exit 0. |
| `--error-codes` | -- | Print every `Error` name with its process exit code (one `code`-TAB-`name` line per entry) and exit 0. The codes are the runtime source of truth for the exit-code contract ([trix-reference.md "Process exit codes"](trix-reference.md#316-error-handling)). |
| `-i`, `--stdedit`, `--interactive` | -- | Force the interactive REPL (default when no filename). |
| `--stdin` | -- | Read the program from standard input. |
| `-l`, `--image` | -- | Treat the filename as a snapshot image, not a script. |
| `-q`, `--quiet` | -- | Suppress the startup banner **and** all diagnostic stderr (backtraces, error messages, warnings). |
| `--no-banner` | -- | Suppress only the startup banner; diagnostics still print (for scripted interactive sessions). |
| `--sandbox` | -- | Disable filesystem, system, and raw-memory operators (see [section 5](#5-the---sandbox-flag)). |
| `--resident` | -- | Stay resident after the script or input ends: instead of exiting, the interpreter parks on the interrupt wait and serves host-delivered work (`invoke()` / `raise_interrupt()` from another thread, [host-integration.md §12](host-integration.md#12-embedded-deployment)). A delivered `quit` or `ExitIRQ` stops it. Embedded hosts set `Config::m_resident`. |

`-q` suppresses diagnostics but does **not** change the exit code -- a script
that fails under `-q` still exits with the corresponding error code, just
without printing the message:

```console
$ trix -q undefined-name.trx ; echo "exit=$?"
exit=41
```

### 3.2 VM-tuning options

These size the VM heap and the fixed-depth stacks. Numeric values for
`--vm-size` and `--stream-buffer` accept `K`, `M`, and `G` suffixes
(`1024`, `1048576`, `1073741824`). A value below the minimum or above the
maximum is clamped (with a warning to stderr); the run still proceeds. An
unparseable value is a usage error (exit 1).

| Flag | Argument | Default | Range |
| --- | --- | --- | --- |
| `--vm-size` | BYTES | 1M | 256K .. 4G |
| `--operand-depth` | N | 1024 | 128 .. 8192 |
| `--exec-depth` | N | 2048 | 128 .. 8192 |
| `--dict-depth` | N | 64 | 16 .. 256 |
| `--error-depth` | N | 64 | 8 .. 256 |
| `--scratch-depth` | N | 128 | 16 .. 4096 |
| `--save-depth` | N | 64 | 4 .. 255 |
| `--stream-count` | N | 4 | 0 .. 255 |
| `--stream-buffer` | BYTES | 4K | 128 .. 256K |
| `--stream-io` | MODE | `all` | `none`, `all`, or a comma list |
| `--name-buckets` | N | 2053 | snaps to a prime in the bucket table |
| `--userdict-size` | N | 512 | 256 .. 50000 |
| `--eq-string` | N | 128 | 0 .. 256 |
| `--eq-array` | N | 32 | 0 .. 256 |
| `--eq-proc` | N | 32 | 0 .. 256 |
| `--eq-dict` | N | 32 | 0 .. 256 |
| `--quantum` | N | 0 (unlimited) | 0 .. 1000000000 |
| `--max-ops` | N | 0 (unlimited) | 0 .. 2^64-1 |
| `--sleep-budget` | N (ms) | 0 (unlimited) | 0 .. 2^64-1 |
| `--module-path` | PATH | (none) | colon-separated dirs (see [section 4](#4-module-search-path-and---module-path)) |

Notes on the less obvious knobs:

- **`--vm-size`** sizes the single VM heap that backs both the local bump
  arena and the global GC'd heap. The interactive banner echoes it back:

  ```console
  $ trix -i --vm-size=2M
  Trix 0.9.0 -- 2M VM -- 828 ops (986 total) +3 user, image v176
  (ctrl-D or 'quit' to exit)
  Trix>
  ```

- **`--operand-depth`**, **`--exec-depth`**, **`--dict-depth`**,
  **`--error-depth`** size the four fixed interpreter stacks (operand,
  exec, dict, error). A below-minimum value is clamped, not
  rejected:

  ```console
  $ echo "1 =" | trix --stdin --operand-depth=99
  trix: --operand-depth: 99 is below minimum 128
  1
  ```

- **`--save-depth`** caps nested `save`/`restore` checkpoints; see
  [save-restore.md](save-restore.md).
- **`--scratch-depth`** sizes the per-coroutine scratch arena used by
  `find-all` / `find-n` / `aggregate`.
- **`--stream-count`** and **`--stream-buffer`** size the open-stream table
  and each stream's buffer; see [streams-io.md](streams-io.md).
- **`--stream-io=MODE`** selects which standard streams are wired up:
  `all` (the default), `none`, or a comma-separated subset of
  `stdin,stdout,stderr,stdedit`. With `stdin` disabled, reading from standard
  input fails rather than blocking.
- **`--eq-string` / `--eq-array` / `--eq-proc` / `--eq-dict`** size the
  per-type buffers used when comparing and deduplicating equal values; the
  defaults are fine for almost all programs.
- **`--quantum`** sets the default cooperative-scheduler time slice
  (operations per slice; `0` = run to completion); see
  [coroutines.md](coroutines.md).
- **`--max-ops`** is a hard execution-limit cap. When set, the VM raises
  `/execution-limit` after that many operations -- a watchdog for untrusted
  or possibly-non-terminating scripts:

  ```console
  $ trix --max-ops=100000 spin.trx
  Trix execution-limit '@loop': execution limit reached (100000 operations)
  $ echo "exit=$?"
  exit=54
  ```

- **`--sleep-budget`** is `--max-ops`'s wall-clock companion: the op
  counter cannot tick while a coroutine is parked, so a single huge
  `coroutine-sleep` (or `actor-recv-timeout` / `read-key-byte-timeout`
  deadline) stalls a bounded run anyway. The budget caps the TOTAL
  park time granted across the whole run; once spent, every timed park
  wakes immediately (yield semantics). Event parks with no deadline
  (actor receive with no timeout, `read-key-byte`'s infinite wait) are
  not wall-clock sleeps and are unaffected. The fuzz harness sets both
  knobs.

---

## 4. Module search path and `--module-path`

`require` (and the `require-module` family) load a `.trx` file by name,
idempotently -- the first `require` of a given file runs it; later `require`s
of the same canonical path are silently skipped. `--module-path` controls
where `require` looks when the name is **relative**.

`--module-path` takes a single colon-separated list of directories, e.g.
`--module-path=/opt/trixlib:./vendor`.

For a `require` argument, `trix` resolves the file in this order:

1. **Current working directory (and any relative/absolute path in the
   argument).** The raw name is first resolved with `realpath` against the
   process CWD. If that names an existing file, it wins -- the search path is
   not consulted.
2. **Each `--module-path` entry, in order.** If step 1 fails *and* the name
   is relative, `trix` tries `<entry>/<name>` for each colon-separated entry,
   left to right, and takes the first that exists.
3. **The binary-relative `lib/` directory, last.** Finally `trix` tries
   `<dir-of-trix-binary>/lib/<name>`. This is how the shipped standard
   libraries (`lib/keys.trx`, `lib/screen.trx`, `lib/ansi.trx`, ...) are
   found without any configuration.

An **absolute** `require` argument (`/abs/path.trx`) skips the search path
entirely and is resolved as given.

Worked example. Given a module:

```trix
/double { 2 mul } def
```

placed at `/opt/lib/mymod.trx`, a script can load it from anywhere by putting
`/opt/lib` on the module path:

```console
$ echo "(mymod.trx) require  21 double =" | trix --stdin --module-path=/opt/lib
42
```

Without a matching path entry (and with no such file in the CWD), `require`
fails with `/filename-not-found`:

```console
$ echo "(mymod.trx) require" | trix --stdin
Trix filename-not-found 'require': stream open failed with errno = 2/No such file or directory
$ echo "exit=$?"
exit=10
```

Because step 1 checks the CWD first, a module sitting next to the script (or
named by a relative path like `./helpers/util.trx`) is found without any
`--module-path` at all. See [modules.md](modules.md) for the module *system*
(`module` / `use` / `import` / `require-module`); `--module-path` only governs
file lookup.

---

## 5. The `--sandbox` flag

`--sandbox` disables the operators that touch the filesystem, spawn processes,
or read/write raw VM memory. Use it when running untrusted scripts. A blocked
operator raises `/unsupported` with a message naming itself; the rest of the
language (arithmetic, collections, strings, logic, coroutines, in-memory I/O,
etc.) runs normally.

What it blocks (each raises `unsupported '<op>': <op>: disabled in sandbox
mode`):

- **Process / shell:** `system`, `shell`, `chdir`, `getcwd`, `hostname`.
- **Filesystem:** `stream` (file open), `with-stream`, `delete-file`,
  `rename-file`, `file-stat`, `chmod`, `mkdir`, `rmdir`.
- **Whole-VM image:** `snap-shot`, `thaw`, and the `run` operator (running a
  file by name).
- **Raw memory:** `alloc`, `free`, `peek`, `poke`.
- **Terminal / raw input:** `raw-mode`, `cooked-mode`, `terminal-size`,
  `key-ready?`, `read-key-byte`, `read-key-byte-timeout`, and the
  screen-rendering primitives.
- **Debugger:** `breakpoint`.

What it does **not** block: read-only environment introspection such as
`getenv`, `getpid`, and `file-exists?` remain available. (The sandbox guards
the operators that *act*; pure queries are allowed.)

Representative checks:

```console
$ echo "(echo hi) system" | trix --stdin --sandbox
Trix unsupported 'system': system: disabled in sandbox mode

$ echo "(/tmp/x.trx) (r) stream" | trix --stdin --sandbox
Trix unsupported 'stream': stream: disabled in sandbox mode

$ echo "getpid =" | trix --stdin --sandbox
191805
```

The same operators run normally without `--sandbox`. The exact list of
sandbox-gated operators is `m_sandbox`-guarded in the source; the categories
above cover all of them.

---

## 6. Debugger flags

These flags exist only in builds compiled with the debugger feature (check
`trix --version` for `debugger` in the feature list; they are absent from a
release build that compiles it out). There are two distinct facilities:

- **`-d` / `--debug`** enables the interactive *debugger substrate* -- the
  `debug-*` introspection operators and breakpoint machinery a script can
  drive itself. This is the low-level layer documented in
  [debugger.md](debugger.md).
- **`--inspect` / `--inspect-on-error` / `--inspect-at=/NAME`** launch the
  full-screen **inspector TUI** (`lib/debugger.trx`) around a script:
  `--inspect` steps from line 1 under interactive control, `--inspect-on-error`
  runs normally and drops into the inspector on the first error, and
  `--inspect-at=/NAME` runs until `/NAME` is invoked and halts there. All
  three require a script filename and are mutually exclusive with `-i` and
  `--image`. **`--no-color`** disables ANSI color in the inspector UI.

The inspector is an interactive terminal program; do not pipe it. For the
full walkthrough of both the substrate and the TUI -- panes, stepping,
watch expressions, and the operator catalog -- see
[debugger.md](debugger.md).

---

## 7. Snapshot images from the CLI

Trix can serialize its complete VM state to a single image file and resume it
later. From the CLI this is a two-step workflow: a script calls `snap-shot`
to write an image, and a later invocation passes `-l` / `--image` to resume.

`snap-shot` captures the VM as a *continuation*: resuming does not re-run the
script from the top, it picks up execution **at the point `snap-shot` was
called**. So the observable work goes *after* `snap-shot` -- it runs once on
the original execution and again on every resume. (See
[snapshot-thaw.md](snapshot-thaw.md) §4.1 for the full semantics.)

A minimal round trip. The builder script:

```trix
/greeting (Hello from the image) def
(/tmp/demo.img) snap-shot
greeting =
```

Running it writes the image and -- because the print is after `snap-shot` --
prints once on the original run:

```console
$ trix build.trx
Hello from the image
$ ls /tmp/demo.img
/tmp/demo.img
```

Resuming the image re-runs the captured continuation, so the post-`snap-shot`
code runs again. Both spellings of the flag work:

```console
$ trix --image /tmp/demo.img
Hello from the image
$ trix -l /tmp/demo.img
Hello from the image
```

Open file streams are reconnected to their files at their saved positions on
resume. `snap-shot` and `thaw` are both disabled under `--sandbox`
([section 5](#5-the---sandbox-flag)). For the in-script `thaw` operator,
images with open streams, PRNG state, and the on-disk format, see
[snapshot-thaw.md](snapshot-thaw.md).

---

## 8. Exit codes

When a program runs cleanly to completion, `trix` exits 0:

```console
$ echo "1 2 add =" | trix --stdin ; echo "exit=$?"
3
exit=0
```

When an error escapes the top level, `trix` exits with the **error's numeric
code** -- the underlying byte value of the Trix `Error` enum. This lets a
shell script branch on the specific failure:

```console
$ trix undefined-name.trx ; echo "exit=$?"
Trix undefined 'interpreter': executable name undefined-name is not associated with any Object
exit=41
```

Here `/undefined` maps to exit code 41. A few commonly-seen codes:

| Code | Error                 | Typical cause                           |
| ---- | --------------------- | --------------------------------------- |
| 0    | `/no-error`           | clean completion                        |
| 10   | `/filename-not-found` | `require` / file open of a missing file |
| 39   | `/syntax-error`       | unbalanced `{ }`, bad token             |
| 41   | `/undefined`          | executable name not bound in any dict   |
| 46   | `/vm-full`            | VM heap exhausted                       |
| 54   | `/execution-limit`    | `--max-ops` cap reached                 |

Codes 0..124 are reserved for Trix errors; 125 is an uncaught C++ exception;
126/127 are POSIX-reserved; 128+N means "killed by signal N". A
*usage* error (an unknown flag, a mutually-exclusive flag combination, or an
unparseable numeric argument) exits 1 before the VM ever starts. The
complete error-code-to-name mapping is in
[errors-cheatsheet.md](errors-cheatsheet.md#process-exit-codes).

---

## 9. Where to go next

- [Documentation index](index.md) -- the full map of language guides.
- [host-integration.md](host-integration.md) -- the same configuration from
  the C++ embedding side (the `Config` struct), plus `command-line-args`.
- [modules.md](modules.md) -- the module system that `--module-path` feeds.
- [streams-io.md](streams-io.md) -- streams, files, and `--stream-*` tuning.
- [snapshot-thaw.md](snapshot-thaw.md) -- `snap-shot` / `thaw` and image
  format, the engine behind `-l` / `--image`.
- [debugger.md](debugger.md) -- the debugger substrate and inspector TUI
  behind `-d`, `--inspect`, and friends.
- [errors-cheatsheet.md](errors-cheatsheet.md) -- the full error catalog and
  exit-code mapping.
- [save-restore.md](save-restore.md) -- the transaction mechanism sized by
  `--save-depth`.
