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
-->

# Trix Error Cheat Sheet

Every named error Trix can raise, grouped by domain.  Each row gives:

  * **Error name** — the `/...` Name you compare against in `try` / `try-catch`
  * **When** — the runtime situation that triggers it
  * **Typical raisers** — representative ops that throw this
  * **Recovery pattern** — the idiomatic way to handle it (if any)

Trix's `Error` enum has **59** entries (`/no-error` + 58 distinct
error names).  The enum-value doubles as the **process exit code**
when an error escapes the top level — `+error` byte.  See § "Process
exit codes" at the bottom.

> **Catching the right way:** `{ ... } try` returns `/no-error` on
> success or the error-name Name on failure.  Compare with `eq`:
>
> ```trix
> { /vm-full throw } try
>   dup /vm-full eq { pop (out of memory -- fallback) print }
>                  { pop (other error) print } if-else
> ```
>
> Or use `try-catch` for a dict of `{ /error-name: handler-proc }`
> entries.  `try-result` wraps success/failure as a single `/ok` /
> `/err` tagged value.  User code raises errors with `throw`
> (a name on the stack) — there is no separate `error` op.

---

## Stack errors

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/opstack-underflow` | Op popped fewer items than required from the operand stack | Every op that consumes args (`add`, `pop`, `def`, …) | Audit the calling sequence; missing arg. Usually a bug, not catchable in production |
| `/opstack-overflow` | Operand stack exceeded its 1024-slot capacity | Recursive pushes without consume | Bounded; restructure to consume earlier |
| `/execstack-overflow` | Execution stack exceeded its 2048-slot capacity | Deep recursion, unbounded `loop` body push | Convert to iteration or trampoline |
| `/errstack-overflow` | Error stack exceeded its 64-slot capacity | Pathological try/error nesting | Almost always a bug — implies handler leaks |
| `/dictstack-overflow` | Dict-stack exceeded its cap (default 64; 32 per coroutine) | Excessive `begin` without matching `end` | `end` more aggressively; for actors, see [coroutines.md](coroutines.md) |
| `/dictstack-underflow` | `end` with no matching `begin`, or below the `systemdict` floor | Stray `end` | Match `begin`/`end` pairs |
| `/unmatched-mark` | Searched for a `mark` on the operand stack and didn't find one | `]`, `>>`, `}}`, `clear-to-mark`, `count-to-mark` | Push `mark` before the body that expects to find one |

## Type / value errors

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/type-check` | Operand has wrong type for the op | Every op with `verify_operands(...)` | Convert before calling (`to-number`, `cast`, etc.) |
| `/undefined` | Executable Name not bound in any dict on the dict stack | Bare `\foo` lookup with `foo` undefined | Provide a definition; or check `where /foo` first |
| `/invalid-name` | Tried to bind a Name beginning with `@` (reserved for internal operators) | `def`, `def-persist`, `local-def`, `\|...\|` binding | Don't prefix user-defined names with `@` |
| `/undefined-case` | `case` fell through with no match and no default arm; or a protocol method called on a type with no implementation | `case`, `type-case`, protocol dispatch | Add a default arm / `def-default-method`; or pre-filter inputs |
| `/undefined-result` | An operation produced a result with no meaningful representation | `fmod` / `remainder` with an infinite operand; forcing a circular thunk; `cell-set` on a computed cell | Ensure finite operands; avoid self-referential thunks/cells |
| `/range-check` | Numeric value out of acceptable range for the op | `range`, integer indices in collection ops, byte ranges | Clamp / validate before call |
| `/index-check` | Container index out of bounds | `arr i get`, `str i get`, `dict-arr i get` | Check `length` first; or use `known?` for dicts |
| `/limit-check` | Hit a fixed implementation limit (not VM heap) | `MaxStringLength` (65535), `=proc` capacity, generation counter exhaustion | Different op (e.g. streaming) or reduce input size |

## Numeric errors

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/div-by-zero` | Integer `div` / `mod` with zero divisor | `div`, `mod` | Guard with `dup 0 eq { ...zero-path... } { div } if-else` |
| `/numerical-nan` | NaN propagated through a math op that rejected it | `sqrt` of negative Real (some build modes), comparison vs NaN under strict policy | Use `nan?` predicate; or accept NaN as the path |
| `/numerical-inf` | Infinity propagated where rejected | Overflow in `mul` / `exp` (mode-dependent) | Pre-clamp magnitudes |
| `/numerical-overflow` | Integer overflow on typed arithmetic | `add`, `sub`, `mul` on `Integer` / `Long` / etc. | Promote to wider type (`Long` / `Int128`) before op |

## Memory / VM

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/vm-full` | Allocator (local or global) could not satisfy a request | Any allocating op: `dict`, `string`, `array`, container literals, `concat` | Often unrecoverable in-place; `vm-global-gc` may free space if the global VM is the culprit |
| `/dict-full` | Dict capacity reached (non-dynamic) | `put` on a fixed-capacity dict at full | Build with `N dict` of sufficient capacity, or use `N dynamic-dict` |
| `/read-only` | Mutation attempted on a `ReadOnly` container | `put`, `poke`, etc. on `#r` or `#ar` containers | Build with `#w` (or default), or copy before mutation |
| `/invalid-access` | Capability check failed (handle access denied) | Stream / OpaqueHandle ops on closed or wrong-kind handles | Reopen / re-handshake the handle |

## Save / restore

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/invalid-restore` | `restore` token is stale, malformed, or below the current barrier | `restore` with a token from a different `save` generation, zero token, or token from a discarded branch | Use only the immediate `save`-returned token; never reuse |
| `/above-barrier` | Op tried to touch state above its save-level barrier | `-persist` family, eqref ops at the wrong level | Stay within the save level the token was issued at |

## Control flow

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/invalid-exit` | `exit` outside a loop body | Bare `exit` at top level | Only call `exit` inside `loop`, `repeat`, `while`, `for`, `for-all` |
| `/invalid-stop` | `stop` outside a `stopped` extent | `stop` with no enclosing `stopped` | Only call `stop` inside a `stopped {...}` |
| `/invalid-throw` | `throw` called with `/no-error` (a non-Name arg raises `/type-check`) | `throw` | Don't throw `/no-error`; check the arg is a Name |

## I/O and streams

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/file-open-error` | OS-level open failure (permissions, ENOSYS, EMFILE, etc.) | `stream` | Check `errno`-equivalent via OS; verify path / mode |
| `/filename-exists` | Mode required absence (`/create-new`) but file present | `stream` with create-exclusive mode | Choose a unique name, or delete first |
| `/filename-not-found` | Mode required presence and file is absent | `stream` for read | Check existence with `file-exists?`; fall back to default |
| `/io-read-error` | OS-level read failure mid-stream | `read-line`, `read`, `read-all`, stream-decode ops | Reopen stream; check device state |
| `/io-write-error` | OS-level write failure | `write`, `write-string`, `print`, `flush` | Surface to user; rarely recoverable in-process |
| `/io-seek-error` | Seek on a non-seekable stream, or past EOF where disallowed | `set-stream-position` | Use a buffered or memory-stream wrapper |
| `/invalid-stream` | Operation on a closed / freed stream | Any stream op after `close-stream` | Track stream lifetime; reopen if needed |
| `/invalid-stream-access` | Read on write-only or write on read-only | `read-line` on output stream | Open with both directions or split into two streams |
| `/set-file-position-required` | Operation required explicit `set-stream-position` first | Some random-access binary ops | Issue `0 set-stream-position` before the call |

## Snapshot / image

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/invalid-image-file` | Snapshot file failed header / magic / CRC check | `thaw` | Image was hand-edited, truncated, or from a different binary build |
| `/snap-shot-error` | Snapshot internal-state failure (mid-write or unexpected layout) | `snap-shot`, `thaw` | Likely a build incompatibility (snapshot version) — see `docs/snapshot-thaw.md` |

## Scanning / format strings

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/syntax-error` | Source-level malformed syntax | The scanner during file load or `exec` of an executable string | Fix the source; `try` catches when scanning a stream/string at runtime |
| `/invalid-format-string` | Format string mis-specified (`{:z}`, unmatched `{`, etc.) | `print-fmt`, `sprint-fmt`, `sscan-fmt`, `fscan-fmt` | Hand-fix the format string; see `docs/format-cheatsheet.md` |
| `/scan-duplicate-arg-id` | Same `{N}` positional id used twice in a format string | `sscan-fmt` / `fscan-fmt` | Unique-numbered placeholders |
| `/scan-input-fail` | `sscan-fmt` consumed fewer args than the format string requires | `sscan-fmt` | Provide more input; or use a shorter format |
| `/scan-match-fail` | Literal text in `sscan-fmt` did not match input | `sscan-fmt` | Loosen the literal or pre-process input |
| `/scan-type-fail` | Parsed value out of range for target type | `sscan-fmt` with `{:d}` on `1e20` | Use a wider target type or guard input |
| `/scan-type-mismatch` | Target type doesn't accept the conversion at all | `sscan-fmt` writing a non-numeric to an integer slot | Fix the format spec |

## Logic, pattern matching, effects

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/fail` | Logic-programming op explicitly failed (Prolog cut-style) | `fail`, body of a `choice` arm | Caught by `choice` / `find-all` and triggers backtracking automatically |
| `/match` | A fatal pattern-match op found no matching arm | `match`, `cond` | Use `when` (leaves the value unchanged on miss) for non-fatal matching, or add a catch-all arm |
| `/protocol` | A protocol-definition op failed: duplicate/unknown protocol, or a method name already claimed or not part of the protocol | `def-protocol`, `extend-protocol`, `def-method`, `protocol-methods`, `protocol-satisfies?` | Fix the protocol/method name; define the protocol before extending it |
| `/effect-not-handled` | `perform` reached the top level with no enclosing `handle-effect` | Algebraic-effects `perform` | Provide an enclosing `handle-effect` (`handler-dict body -- result`) |
| `/unhandled-capture` | `capture` invoked with no enclosing `delimit` | `capture` outside `delimit` | Wrap the capture in `delimit { ... }` |

## Contracts

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/require` | Function precondition failed | `precondition` | Caller bug — fix the call site; or relax the precondition |
| `/ensure` | Function postcondition failed | `postcondition` | Callee bug — fix the implementation |
| `/assert-failed` | A bare `assert` evaluated false | `assert` | Same shape as `/require` but invariant-style |

## Limits / capability

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/execution-limit` | Per-process or per-actor instruction budget exhausted (when enabled) | Long-running tight loops in budgeted contexts | Re-architect to yield (`coroutine`) or raise the budget |
| `/unsupported` | Op recognised but not supported in this combination | `{ ... }#pw` (packed proc + writable), some HandleKind-specific gaps | Choose the supported variant (`{ ... }#aw` for a writable proc body) |

## Internal / catch-all

| Error | When | Typical raisers | Recovery |
| --- | --- | --- | --- |
| `/internal-error` | Invariant violation inside the VM | Defensive guards in the interpreter | Always a Trix bug — report with reproducer |
| `/user-error` | Internal enum slot for user-thrown errors.  Not normally observed via `try` -- the thrown Name passes through unchanged.  Exit-code 58 maps here when a user-thrown name escapes the top level | `throw` with any Name not in this table | Compare against your own enumerated names |
| `/no-error` | The success sentinel `try` pushes when the body completed cleanly | Never raised; only synthesised by `try` / `stopped` success paths | N/A — comparing against this is how you detect success |

---

## Common patterns

### Catch by name (`try` + `eq` cascade)

```trix
% try returns /no-error on success, the error name otherwise.
{ undefined-name } try
  dup /undefined eq
  { pop (recovered from undefined) print }
  {
    dup /vm-full eq
    { pop (recovered from oom) print }
    { (unexpected: ) print = }        % keep the error name on top for context
    if-else
  }
  if-else
```

### Catch with a handler dict (`try-catch`)

```trix
% try-catch: dict proc :- --
% dict has /error-name -> handler-proc entries.  The handler receives the
% error name on the operand stack.  A /default key catches anything not
% otherwise matched.
<<
  /file-open-error    { (file gone: ) print = }
  /filename-not-found { (not there: ) print = }
  /default            { (other error: ) print = rethrow }
>>
{ (/no/such/file) (r)#b stream } try-catch
```

### `try-result` (single-value `/ok` or `/err` tagged form)

```trix
% try-result wraps success/failure as a single tagged Object.
% Dispatch via tag-match with a dict of arms.

{ 6 7 mul } try-result
<<
  /ok  { (success: ) print = }
  /err { (failed:  ) print = }
>> tag-match
% prints:  success: 42

{ /file-open-error throw } try-result
<<
  /ok  { (success: ) print = }
  /err { (failed:  ) print = }
>> tag-match
% prints:  failed:  file-open-error
```

### Group-catch with falsy fall-through

```trix
% Treat any I/O / filesystem failure as one category; anything else
% propagates up via rethrow.  `dup` keeps the error name on the stack
% for each `eq` test; `or` folds them; the if-arm pops the name on
% match, throws otherwise.
{ (/no/such) (r)#b stream } try
  dup /filename-not-found eq
  1 index /file-open-error eq or
  1 index /io-read-error   eq or
  1 index /io-write-error  eq or
  { pop (I/O failed -- using default) print }
  { throw }
  if-else
% prints:  I/O failed -- using default
```

### Rethrow inside a `try-catch` handler

```trix
<<
  /filename-not-found { (caught -- adding context, rethrowing) print rethrow }
>>
{ (/no/such) (r)#b stream } try-catch
% Handler runs, prints, then rethrows /filename-not-found to the next
% outer scope.  Top-level uncaught -> process exits with code 10.
```

---

## Process exit codes

When an error escapes the top level of the Trix process, the binary
exits with status `+error` (the Error enum's underlying byte value).

This means you can branch a shell script on the specific Trix
error:

```bash
./trix script.trx
case $? in
    0)  echo "ok"                ;;   # /no-error
    18) echo "format string bad" ;;   # /invalid-format-string
    39) echo "syntax error"      ;;   # /syntax-error
    40) echo "type mismatch"     ;;   # /type-check
    41) echo "undefined name"    ;;   # /undefined
    46) echo "VM exhausted"      ;;   # /vm-full
    *)  echo "exit $?"           ;;
esac
```

The exact mapping is the enum order in `src/types.inl::Error`.  Codes
0..124 are reserved for Trix errors (NoError = 0 through UserError =
58); 125 is reserved for an uncaught C++ exception; 126/127 are
POSIX-reserved shell codes; 128+N is "killed by signal N".

---

## Where to go next

  * **Op-by-op error conditions:** [`trix-reference.md`](trix-reference.md)
    § 3.x for each operator family.
  * **Format-string syntax:** [`format-cheatsheet.md`](format-cheatsheet.md).
  * **Save/restore semantics in depth:** [`save-restore.md`](save-restore.md).
  * **Error handling design (handler dicts, unwind, try-catch
    composition):** [`error-handling.md`](error-handling.md).
