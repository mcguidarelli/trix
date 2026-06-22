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

# Trix Operator Cheat Sheet

The everyday subset.  ~312 of the 838 user-facing operators in Trix,
picked for "things you reach for first".  Every op listed has been verified to
exist in `systemdict`.  For exact stack effects, error conditions,
edge cases, and worked examples, see the corresponding `### 3.x`
subsection in [`trix-reference.md`](trix-reference.md).

**Stack-effect notation:** `before -- after` (top of stack is rightmost).

**Type shorthand:**
`num` = any numeric ·
`int` = signed integer (Integer / Long / Int128) ·
`uint` = unsigned integer (Byte / UInteger / ULong / UInt128) ·
`byte` = Byte ·
`any` = any type ·
`arr` = Array ·
`pk` = Packed ·
`str` = String ·
`proc` = executable Array or Packed ·
`dict` = Dict ·
`set` = Set ·
`bool` = Boolean ·
`name` = Name ·
`stream` = Stream ·
`tagged` = Tagged ·
`record` = Record ·
`cell` = Cell ·
`coroutine` = Coroutine ·
`pipe` = PipeBuffer ·
`coll` = any iterable collection (arr/pk/str/dict/set/lazy) ·
`xf` = transducer ·
`pred` = proc with signature `any -- bool` ·
`tag-name` = type-name Name.

---

## 3.1 Stack manipulation

| Op | Effect | Note |
| --- | --- | --- |
| `pop` | `any --` | Drop top |
| `dup` | `a -- a a` | Duplicate top |
| `exch` | `a b -- b a` | Swap (no separate `swap` alias) |
| `dup-n` | `a1..aN N -- a1..aN a1..aN` | Duplicate top N stack items |
| `roll` | `aN..a1 n j -- a(j)..` | Cyclic rotate |
| `index` | `aN..a0 i -- aN..a0 a(i)` | Copy by depth |
| `count` | `-- n` | Items on the whole operand stack (use `count-to-mark` for items above a mark) |
| `clear` | `... --` | Drop all |
| `mark` | `-- mark` | Push mark sentinel |
| `stack` `print-stack` | `--` | Print operand stack (debug; no consume) |

## 3.2 Arithmetic — strict-typed (no auto-promotion; see types-cheatsheet.md)

| Op                    | Effect                                          |
| --------------------- | ----------------------------------------------- |
| `add sub mul div mod` | Standard binary                                 |
| `neg abs`             | Unary                                           |
| `min max`             | Binary                                          |
| `clamp`               | `n lo hi -- n'` -- bounded clamp                |
| `promote`             | `n1 n2 -- n1' n2'` -- widen both to common type |

`div` is type-preserving: `7 2 div` returns `3` (Integer truncating);
`7.0 2.0 div` returns `3.5` (Real).  For mixed-type division use
`$[ a / b ]` infix.

## 3.3 Comparison

| Op                                | Effect                                                        |
| --------------------------------- | ------------------------------------------------------------- |
| `eq ne`                           | `a b -- bool` -- strict on type+value (`1 1.0 eq` is `false`) |
| `lt le gt ge`                     | Same shape; type-check error on mixed types                   |
| `deep-eq deep-ne`                 | Deep equality (recursive into containers)                     |
| `between?`                        | `n lo hi -- bool`                                             |
| `total-order?` `total-order-mag?` | IEEE-754 total-ordering predicates                            |
| `ulp-equal?`                      | Float comparison within N ULPs                                |

## 3.4 Logical and bitwise

| Op                                  | Effect                                             |
| ----------------------------------- | -------------------------------------------------- |
| `and or xor not`                    | Boolean / bitwise (overloaded)                     |
| `and? or?`                          | Short-circuit boolean variants                     |
| `shift-left shift-right`            | Arithmetic shift                                   |
| `bit-set bit-clear bit-toggle bit?` | Per-bit operations (`bit?` queries)                |
| `bit-width bit-ceil bit-floor`      | Bit-position utilities                             |
| `single-bit?`                       | `uint -- bool` -- power-of-two test (unsigned int) |

## 3.5 Math

| Op                                        | Effect                         |
| ----------------------------------------- | ------------------------------ |
| `sqrt sin cos tan` `asin acos atan atan2` | Real → Real                    |
| `exp log log2 log10`                      | Real → Real                    |
| `floor ceil trunc round round-even`       | Real → Real (still float type) |
| `gcd lcm`                                 | Integer pair → Integer         |
| `fe-get-round fe-set-round`               | IEEE-754 rounding mode         |

(There is no built-in `pi` / `e` constant op — use the literal
`3.14159265358979d` or compute via `1.0 atan 4.0 mul`.)

## 3.6 Type predicates and queries

Type predicates use the **`is-TYPE` prefix form** (not `TYPE?` suffix):

| Op                                                             | Effect                          |
| -------------------------------------------------------------- | ------------------------------- |
| `is-null is-byte is-integer is-uinteger is-long is-ulong`      | Per-numeric-type predicates     |
| `is-int128 is-uint128 is-real is-double`                       | Wide / float predicates         |
| `is-number`                                                    | Any numeric type                |
| `is-signed is-unsigned`                                        | Signed / unsigned integral type |
| `is-float`                                                     | Real or Double                  |
| `is-boolean is-name is-mark is-operator`                       | Other scalar predicates         |
| `is-string is-array is-packed is-dict is-tagged is-record`     | Container predicates            |
| `is-stream is-pipebuffer is-coroutine is-cell is-continuation` | Handle predicates               |
| `is-curry is-thunk is-address`                                 | Other                           |
| `is-actor is-supervisor`                                       | Coroutine-family predicates     |

Plus alternative dispatch via `type` + `eq` (returns the type-name
Name, e.g. `/integer-type`):

```trix
42 type /integer-type eq       % true (equivalent to `42 is-integer`)
```

See [`types-cheatsheet.md`](types-cheatsheet.md) for the full
30-type-name list and dispatch idioms.

Other predicate ops by name:

| Op                      | Effect                                     |
| ----------------------- | ------------------------------------------ |
| `executable?`           | `any -- bool` -- has executable attribute  |
| `readable?` `writable?` | Capability tests on streams / handles      |
| `screen?`               | `any -- bool` -- OpaqueHandle kind test    |
| `known?`                | `dict name -- bool` -- key present in dict |
| `set-member?`           | `set elem -- bool`                         |

## 3.7 Conversion

| Op            | Effect                                                                    |
| ------------- | ------------------------------------------------------------------------- |
| `to-number`   | `str -- num` -- parse a string into the right numeric type                |
| `to-string`   | `any rwstr -- str` -- pre-allocate buffer, write formatted form           |
| `to-name`     | `str -- name` -- intern the string as a Name                              |
| `promote`     | `n1 n2 -- n1' n2'` -- widen both to common numeric type                   |
| `reinterpret` | `num /type-name -- num'` -- bit-cast to named type                        |
| `coerce`      | `container /type-name -- container'` -- widen all elements (no narrowing) |

See [`types-cheatsheet.md`](types-cheatsheet.md) for the full
promotion lattice and conversion recipes.

## 3.8 Strings

| Op                                  | Effect                                 |
| ----------------------------------- | -------------------------------------- |
| `length`                            | `str -- int`                           |
| `get put`                           | Index access (returns / writes `byte`) |
| `concat`                            | `s1 s2 -- s3`                          |
| `take drop`                         | `str n -- prefix / suffix`             |
| `take-while drop-while`             | `str pred -- prefix / suffix`          |
| `split`                             | `s sep -- arr-of-str`                  |
| `trim trim-left trim-right`         | Whitespace trim                        |
| `uppercase lowercase`               | Case conversion                        |
| `contains? starts-with? ends-with?` | Predicates                             |
| `count-substring`                   | `s sub -- int` -- count occurrences    |

## 3.9 Binary + compression

| Op                                              | Effect                            |
| ----------------------------------------------- | --------------------------------- |
| `pack unpack`                                   | Endian-aware binary serialization |
| `crc32 adler32 fletcher32`                      | Checksums                         |
| `deflate inflate`                               | RFC 1951 raw                      |
| `deflate-stream inflate-stream`                 | Streaming variants                |
| `crc32-stream adler32-stream fletcher32-stream` | Streaming checksums               |

## 3.10 Arrays / collections

| Op                                | Effect                                                    |
| --------------------------------- | --------------------------------------------------------- |
| `length get put`                  | Indexed access                                            |
| `for-all`                         | `coll proc --` (proc runs per element)                    |
| `map filter reduce`               | Higher-order                                              |
| `sort sort-by reverse`            | In-place / new-array                                      |
| `range`                           | `stop -- arr` -- generate [0, stop) (half-open)           |
| `range-from`                      | `start stop -- arr` -- generate [start, stop) (half-open) |
| `append`                          | Array building (`concat` is String-only; no `prepend`)    |
| `take drop take-while drop-while` | Slice forms                                               |

## 3.11 Dicts

| Op                             | Effect                      |
| ------------------------------ | --------------------------- |
| `dict dynamic-dict`            | `int -- dict` constructor   |
| `get put known?`               | Access                      |
| `undef undef-persist`          | `dict name --` remove a key |
| `begin end`                    | Push / pop dict-stack       |
| `def override store local-def` | Bind in dict-stack          |
| `where current-dict`           | Introspect dict-stack       |
| `length for-all map filter`    | Higher-order                |

## 3.12 Sets

| Op                                          | Effect                                  |
| ------------------------------------------- | --------------------------------------- |
| `set`                                       | `int -- set` constructor                |
| `set-add set-remove set-member?`            | Modification + access                   |
| `set-union set-intersection set-difference` | Binary ops on Set values                |
| `union intersect difference`                | Variants that operate on Arrays-as-sets |
| `subset? disjoint?`                         | Predicates (no `superset?`)             |

## 3.13 Tagged values

| Op                   | Effect                                            |
| -------------------- | ------------------------------------------------- |
| `tag`                | `val tag-name -- tagged`                          |
| `tag-name tag-value` | Decompose                                         |
| `tag-match`          | `tagged dict -- ...` -- dispatch via dict of arms |

## 3.14 Records

| Op                | Effect                     |
| ----------------- | -------------------------- |
| `record-schema`   | Define a Record type       |
| `record-zip`      | Build instance from values |
| `record-to-dict`  | Cross-convert              |
| `.field` (syntax) | Field access sugar         |

## 3.15 Control flow

| Op        | Effect                                    |
| --------- | ----------------------------------------- |
| `if`      | `bool proc --`                            |
| `if-else` | `bool then-proc else-proc --`             |
| `loop`    | `proc --` (infinite; use `exit` to break) |
| `repeat`  | `n proc --`                               |
| `while`   | `cond-proc body-proc --`                  |
| `for`     | `start step stop proc --`                 |
| `for-all` | `coll proc --`                            |
| `exit`    | Break innermost loop                      |

## 3.16 Error handling

| Op                                              | Effect                                                  |
| ----------------------------------------------- | ------------------------------------------------------- |
| `try`                                           | `proc -- /no-error \| /error-name`                      |
| `try-catch`                                     | `dict proc --` -- dict is `{ /err-name: handler-proc }` |
| `try-result`                                    | `proc -- /ok-or-err-tagged`                             |
| `throw`                                         | `name --` -- raise a named error                        |
| `rethrow`                                       | `--` -- re-raise the last error                         |
| `last-error last-error-message last-error-data` | Inspect the last error                                  |
| `stop stopped`                                  | PostScript-style escape                                 |

(There is no separate `error` op — `throw` is the user-side raise.
See [`errors-cheatsheet.md`](errors-cheatsheet.md) for catch patterns.)

## 3.17 I/O

| Op                     | Effect                                                                   |
| ---------------------- | ------------------------------------------------------------------------ |
| `print` `= ==`         | Write to stdout (debug / value / object form)                            |
| `read-line`            | `stream rwstr -- str bool` (fills the provided buffer)                   |
| `write` `write-string` | Stream output                                                            |
| `stream`               | `(path) (mode)#b -- stream` -- open file as stream                       |
| `close-stream`         | Stream cleanup                                                           |
| `make-memory-stream`   | `str -- stream` -- wrap string as readable stream                        |
| `flush`                | Stream flush                                                             |
| `readable? writable?`  | Capability tests (EOF surfaces as `readable?` returning false post-read) |

## 3.20 VM and memory

| Op | Effect |
| --- | --- |
| `save` | `-- save-token` |
| `restore` | `save-token --` |
| `set-global` | `bool --` (toggle force-global allocation) |
| `current-global` | `-- bool` |
| `vm-global-gc` | Trigger mark-sweep on global VM |
| `query-status` | `name -- any` -- one runtime status value by key (VM fields via the `//:status:KEY` path) |

(For explicit global-VM construction, use `true set-global N dict`
or wrap in `${ ... }`.  For status fields, read via dict path:
`//:status:vm-used`, `//:status:vm-global-used`,
`//:status:vm-global-num-alloc` (O(1) live-block count, gc-scratch
excluded), `//:status:vm-global-num-free` (O(1) free-list count), etc.)

## 3.22 System

| Op | Effect |
| --- | --- |
| `now` | `-- ulong` monotonic ms since a steady (unspecified) epoch -- differences only |
| `clock` | `-- ulong` (monotonic process time in microseconds) |
| `instant-to-date instant-day instant-hour ...` | Field extractors (UTC + `-local` variants) |
| `interactive?` `raw-mode?` | Terminal capability tests |
| `status` | `stream -- bool` -- true if the stream is open |

## 3.28 Pipeline

| Op                                                        | Effect                                      |
| --------------------------------------------------------- | ------------------------------------------- |
| `pipe-buffer`                                             | `int -- pipe` -- bounded buffer constructor |
| `pipe-put pipe-get`                                       | Producer / consumer ops                     |
| `pipe-close pipe-error-close`                             | Tear down                                   |
| `pipe-map pipe-filter pipe-take pipe-drop pipe-distinct`  | Stage transforms                            |
| `pipe-reduce pipe-scan pipe-zip pipe-merge pipe-flat-map` | Composers                                   |
| `pipe-run`                                                | Drive a pipeline to completion              |
| `pipe-status`                                             | Introspect                                  |

## 3.29 Actors (Erlang-style)

| Op | Effect |
| --- | --- |
| `actor-spawn` | `mark obj* proc -- coroutine` |
| `actor-spawn-capacity` | Spawn with mailbox capacity |
| `actor-send` | `msg pid --` |
| `actor-broadcast` | Send to multiple actors |
| `actor-recv` | `-- msg` (blocking) |
| `actor-recv-timeout` `actor-recv-match` `actor-recv-match-timeout` | Variants |
| `actor-self actor-name` | Identity |
| `actor-status` | `coroutine -- dict` (the `/status` key holds `/running`/`/suspended`/`/dead`/...) |
| `actor-mailbox-count actor-mailbox-capacity actor-mailbox-empty?` | Mailbox introspection |
| `monitor demonitor` | Lifecycle notification |

## 3.30 Supervision (OTP-style)

| Op | Effect |
| --- | --- |
| `supervisor-spec` | Introspect a running supervisor config (`coroutine -- dict`, read-only); `supervisor` (`dict -- coroutine`) boots one |
| `supervisor-start-child` | Add a child to a running supervisor |
| `supervisor-which-children` | `sup -- arr-of-pid` |
| `supervisor-count-children` | `sup -- int` |
| `supervisor-get-child` | `sup name -- coroutine bool` (child handle + found-flag) |
| `supervisor-restart-child` `supervisor-terminate-child` | Per-child control |
| `supervisor-stop` | Tear down whole supervisor |

## 3.31 Logic / backtracking (Prolog-style)

| Op            | Effect                               |
| ------------- | ------------------------------------ |
| `unify`       | `t1 t2 -- bool`                      |
| `unify-match` | Combined unification + binding       |
| `choice`      | `proc-arr --` (try alternatives)     |
| `find-all`    | `[alts] -- arr-of-results`           |
| `aggregate`   | `init reducer [alts] -- result`      |
| `once`        | `proc --` (first solution only; cut) |
| `naf`         | `proc --` (negation as failure)      |
| `fail`        | `--` -- backtrack                    |

## 3.33 Coroutines

| Op | Effect |
| --- | --- |
| `coroutine-launch` | `mark obj* proc -- coroutine` -- spawn new coroutine |
| `coroutine-suspend` | `coroutine --` -- remove the target coroutine from scheduling |
| `coroutine-resume` | `coroutine --` -- re-enter a suspended coroutine into scheduling |
| `coroutine-await` | `coroutine -- val` -- block until coroutine yields/dies |
| `coroutine-join` | `coroutine --` -- wait for completion |
| `coroutine-status` | `coroutine -- /running\|/suspended\|/dead` |
| `coroutine-self coroutine-id coroutine-by-id` | Identity |
| `coroutine-kill coroutine-kill-all coroutine-die` | Termination |
| `coroutine-priority coroutine-quantum` | Scheduler control |

## 3.34 Protocols (open dispatch)

| Op                    | Effect                                           |
| --------------------- | ------------------------------------------------ |
| `def-protocol`        | `method-names proto --` -- declare the interface |
| `def-method`          | `proc method type-name --` -- per-type impl      |
| `def-default-method`  | `proc method --` -- fallback impl                |
| `extend-protocol`     | `impl-dict type-name proto --` -- batch register |
| `protocol-satisfies?` | `obj proto -- bool`                              |
| `protocol-methods`    | `proto -- name-array` -- list the method set     |

(Define a protocol with `def-protocol`, then attach implementations
with `def-method` / `def-default-method`, or batch-register a whole
type with `extend-protocol`.  There is no `define-protocol` op.)

## 3.35 Pattern matching

| Op            | Effect                                                                        |
| ------------- | ----------------------------------------------------------------------------- |
| `match`       | `value pairs-array -- result` -- multi-arm dispatch over `[{test}{body} ...]` |
| `match-all`   | `val pairs -- arr` collect every matching arm's results                       |
| `unify-match` | Combined unification + binding                                                |

## 3.36 Transducers (Clojure-style)

| Op                                             | Effect                                                    |
| ---------------------------------------------- | --------------------------------------------------------- |
| `xf-map xf-filter xf-take xf-drop xf-distinct` | Build transducer steps                                    |
| `xf-scan xf-flatten`                           | More step shapes                                          |
| `into`                                         | `coll xf -- result` -- drive transducer over a collection |

(No standalone `transduce` op; `into` is the driver.)

## 3.37 GenServer (OTP-style sync requests)

| Op | Effect |
| --- | --- |
| `gen-server` | Boot a gen-server actor.  Handlers are looked up in a dict by `/handle-call`, `/handle-cast`, `/handle-info`, `/init`, `/terminate` keys |

(The GenServer surface is a single op + a handler dict; call/cast
go through `actor-send` to the resulting actor.)

## 3.40 Continuations and effects

| Op              | Effect                                                          |
| --------------- | --------------------------------------------------------------- |
| `capture`       | `proc --` -- capture the current continuation, run proc with it |
| `abort-exec`    | Unwind exec stack to nearest barrier                            |
| `handle-effect` | `handler-dict body -- result` -- install effect handlers        |
| `perform`       | `...args effect-name -- result` -- raise an effect              |

(No `reset` / `shift` separate ops; `capture` + `handle-effect` are
the foundations.  Algebraic-effects-style handling lives at
`handle-effect` + `perform`.)

## 3.42 Chrono

| Op | Effect |
| --- | --- |
| `now` | `-- ulong` -- monotonic ms (steady epoch; differences only); use `epoch-time` for wall-clock |
| `clock` | `-- ulong` -- monotonic microseconds |
| `instant-to-date` `instant-to-date-local` | `ulong -- udate` -- pack date into a UInteger (decode with date-year/date-month/date-day) |
| `instant-day instant-hour instant-minute instant-second instant-millisecond instant-month instant-weekday` | UTC field extractors |
| `instant-day-local ...` | Local-zone variants |

Date/time **parsing and formatting** use the chrono format directive
`{:I<strftime>}` in `print-fmt` / `scan-fmt`; there are no separate
`parse-instant` / `format-instant` ops.  See
[`format-cheatsheet.md`](format-cheatsheet.md) § 3 for the directive.

---

## Where to go next

- **Full operator catalog with stack effects, error conditions, examples:**
  [`trix-reference.md`](trix-reference.md) § 3.
- **Scanner sigils (`$/`, `$\`, `${...}`, `#$`, `#$$`, etc.):**
  [`scanner-syntax.md`](scanner-syntax.md) § 0 or
  [`trix-reference.md`](trix-reference.md) § 2.
- **Category-by-category index of all 42 operator groups:**
  [`trix-reference.md`](trix-reference.md) § 3 introduction.
- **Type system, promotion lattice, conversion ops:**
  [`types-cheatsheet.md`](types-cheatsheet.md).
- **Error names + recovery patterns + exit codes:**
  [`errors-cheatsheet.md`](errors-cheatsheet.md).
- **Format strings:**
  [`format-cheatsheet.md`](format-cheatsheet.md).
