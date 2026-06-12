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

# Trix Cookbook: Recipes and Idioms

Task-oriented recipes for getting things done in Trix.  Each recipe is a short,
self-contained program you can paste into the REPL or pipe through `./trix
--stdin`, plus a pointer to the reference doc that covers the topic in depth.

If you are new to the language, read the [Getting Started](getting-started.md)
tour first -- it explains the stack, `def`, procedures, and the `assert` /
`=` conventions used throughout.  Every block below asserts its own result with
`(message) condition assert`, which is how these recipes stay correct.

> Reminder of the conventions: `=` **prints** the top of the stack (and removes
> it); `(why) condition assert` raises an error if the condition is false; and
> reals print without a trailing `.0` (so `10.0` shows as `10`).

**Jump to:** [Text](#1-text) | [Collections](#2-collections) |
[Naming the stack](#3-naming-the-stack) | [Records and variants](#4-records-and-variants) |
[Errors and validation](#5-errors-and-validation) | [Files and I/O](#6-files-and-io) |
[Laziness and memoization](#7-laziness-and-memoization) |
[Concurrency](#8-concurrency) | [Modules](#9-modules) | [Dates](#10-dates) |
[A small CLI](#11-a-small-cli)

---

## 1. Text

### Parse delimited text

Split a string on a delimiter, then clean up each field with `trim`.  `split`
returns an array of substrings; `map` applies a procedure to each.

```trix
(parse the first field of a CSV-ish line)
(  alice , 30  ) (,) split { trim } map 0 get (alice) eq assert
```

Convert a numeric field with `to-number` (`(30) to-number` yields `30`).  See
[String Processing](string-processing.md).

### Format output

`asprint-fmt` fills a Python-style template from an array of arguments, writing
into a destination string; `aprint-fmt` does the same but prints to stdout.
`{0}` is the first argument, `{1}` the second, and so on.

```trix
(format a value into a string with a positional template)
32 string (hi {0}) [ (bob) ] asprint-fmt pop (hi bob) eq assert
```

For width, precision, alignment, and the type letters, see the
[Format String Cheat Sheet](format-cheatsheet.md).

---

## 2. Collections

### Build and query a dict

A dictionary literal is `<< /key value ... >>`; `get` looks a value up by key.

```trix
/person << /name (alice) /age 30 >> def
(look a field up by key) person /age get 30 eq assert
```

`put` stores, `known?` tests for a key, and `length` counts entries.  See
[Collections](collections.md).

### Transform a collection

`map`, `filter`, and `reduce` (a left fold, `arr init proc -- any`) compose into
a pipeline.  Here: square each number, keep the ones over ten, then sum them.

```trix
(square, keep the big ones, then sum)
[ 1 2 3 4 5 ] { dup mul } map { 10 gt } filter 0 { add } reduce 41 eq assert
```

`for-all` is the lower-level visitor (`elem --` for arrays, `key value --` for
dicts) when you want to thread your own accumulator.  See
[Functional Programming](functional-programming.md).

---

## 3. Naming the stack

### Name your arguments instead of juggling them

`exch`, `roll`, and `index` get unreadable fast.  `let` binds the top stack
values to names for the duration of a `let ... end` block.

```trix
/hypot-squared { [/a /b] let a a mul b b mul add end } def
(let names the operands so the body reads like math) 3 4 hypot-squared 25 eq assert
```

`destructure` pulls fields out of arrays and records the same way.  See
[Pattern Matching](pattern-matching.md).

---

## 4. Records and variants

### Define and use a record

A record is a typed, immutable composite with named fields -- the grown-up
version of "a dict used as an object."  `record-type` defines a constructor.

```trix
/point [ /x /y ] record-type def
(a record has named, typed fields) 3 4 point /x get 3 eq assert
```

See [Records](record.md).

### Model a state machine with tagged values

A tagged value pairs a payload with a tag name; `tag-match` dispatches on the
tag, handing the payload to the matching handler.  That makes a clean state
transition.

```trix
/step {
    << /idle    { pop /running }
       /running { pop /done }
       /done    { pop /done }
    >> tag-match
} def
(idle advances to running) /unit /idle tag step /running eq assert
```

Omit `/default` and `tag-match` enforces exhaustive case analysis -- an
unhandled tag raises an error.  See [Tagged Values](tagged-values.md).

---

## 5. Errors and validation

### Recover with a fallback value

`try-catch` runs a body under a dictionary of handlers keyed by error name.
The handler receives the error name **on top of** whatever the failing body
left on the stack, so it must clear that residue before pushing a result --
here `{ 1 0 div }` leaves `1 0` under the error name, so the handler pops three.

```trix
/safe-div {
    << /div-by-zero { pop pop pop 0 } >>
    { div }
    try-catch
} def
(the fallback replaces the failure) 1 0 safe-div 0 eq assert
(normal division is unaffected)      10 2 safe-div 5 eq assert
```

When you only need to know *whether* something failed, the simpler `try`
returns the error name (or `/no-error` on success) and leaves the recovery to
you.  See [Error Handling](error-handling.md).

### Validate inputs with guards

`precondition` consumes a boolean and raises `/require` when it is false,
passing the checked value through untouched -- chain several to validate a
domain.

```trix
/check-age { dup 0 ge precondition dup 150 lt precondition } def
(a valid age passes both guards) 30 check-age 30 eq assert
```

See [Contracts](contracts.md).

---

## 6. Files and I/O

### Read and write a file

`with-stream` opens a stream, runs your procedure with it on the stack, and
closes it for you -- even if the body raises.  A single `write-string` consumes
the stream; for several writes, `dup` the stream before each but the last.

```trix
(cookbook-demo.txt) (w)#b { (Hello, Trix!) write-string } with-stream
(round-trip a string through a file)
(cookbook-demo.txt) (r)#b { read-all } with-stream (Hello, Trix!) eq assert
```

`read-all` slurps the rest of the stream; `read-line` reads one line at a time.
See [Streams and I/O](streams-io.md).

---

## 7. Laziness and memoization

### An infinite sequence, taken finitely

`lazy-from` is an unbounded counter; `lazy-take` limits it; `lazy-to-array`
forces the result.  Nothing past the fifth element is ever computed.

```trix
(take 5 from an infinite sequence, then sum them)
0  1 lazy-from 5 lazy-take lazy-to-array  { add } for-all  15 eq assert
```

See [Lazy Sequences](lazy-sequences.md).

### Memoize an expensive computation

`thunk` wraps a procedure so it runs at most once; `force` evaluates it the
first time and returns the cached value on every call after.

```trix
/cached { 21 2 mul } thunk def
(the first force runs the proc)            cached force 42 eq assert
(the second force returns the cached value) cached force 42 eq assert
```

See [Curry / Thunk](curry-thunk.md).

---

## 8. Concurrency

### A coroutine that returns a value

`coroutine-launch` takes a `mark`, the coroutine's initial operands, and a
procedure, and starts a cooperative task; `coroutine-join` waits for it and
yields its result and a success flag.

```trix
(launch a coroutine, then join its result)
mark 6 7 { mul } coroutine-launch coroutine-join pop 42 eq assert
```

For yielding, scheduling, pipelines, and actors, see
[Coroutines](coroutines.md) and the rest of the [concurrency
docs](index.md#concurrency).

---

## 9. Modules

### Define a namespaced module

`module` defines a named, read-only namespace from a procedure body; `use ...
end` brings its entries into scope for a block, exactly like `begin` / `end`.

```trix
/math-utils {
    /square { dup mul } def
    /cube   { dup dup mul mul } def
} module
(a module's entries resolve inside use ... end)
/math-utils use 5 square end 25 eq assert
```

`require` loads a module file once (idempotently).  See [Modules](modules.md).

---

## 10. Dates

### Read fields out of an instant

A point in time is a `ULong` of milliseconds since the 1970 UTC epoch.  The
`instant-*` accessors pull calendar fields out of one.  (Use a fixed instant
for reproducible results; `epoch-time` gives the current wall clock.)

```trix
(read the calendar year out of a fixed instant)
1700000000000ul instant-year 2023 eq assert
(and the day of the month)
1700000000000ul instant-day 14 eq assert
```

See [Dates and Times](chrono.md).

---

## 11. A small CLI

### Read command-line arguments

`command-line-args` returns an array of the tokens that followed the script
name on the command line -- everything after option parsing stops.

```trix
(the script argv tail is always an array) command-line-args type /array-type eq assert
```

Given a script `greet.trx`:

```trix
% greet.trx -- print a greeting for each name argument
command-line-args { (Hello, ) exch concat = } for-all
```

it runs as a small command-line tool:

```console
$ ./trix greet.trx alice bob
Hello, alice
Hello, bob
```

Pair this with `--stdin` to read piped input instead.  The full command-line
surface -- flags, startup modes, exit codes -- is in [Running Trix](cli.md).

---

## Where to go next

- [Getting Started](getting-started.md) -- the language tour, if you skipped it.
- [Trix Reference](trix-reference.md) -- every operator, with stack effects.
- [Operator Cheatsheet](operator-cheatsheet.md) and
  [Types Cheatsheet](types-cheatsheet.md) -- quick lookups.
- The feature guides linked from each recipe, all indexed in the
  [documentation index](index.md).
- [examples/](../examples/README.md) -- complete programs to read and run.
