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

# Trix for PostScript Programmers

If you have written PostScript, you already know how to read Trix.  The
operand stack, the dictionary stack, literal vs. executable names, procedures
in braces, `save`/`restore`, `mark` -- they are all here, with the same names
and the same behaviour.  This guide maps what you know onto Trix, points out
where the spelling or the semantics differ, and shows what Trix adds that
PostScript never had.

**One thing up front: Trix is PostScript-*inspired*, not PostScript-*compatible*.**
It borrows PostScript's execution model -- a concatenative stack language with a
dictionary stack and named procedures -- because that model is an excellent
foundation for a small, embeddable interpreter.  It does **not** implement the
imaging model: there is no `showpage`, no graphics state, no paths or paint
operators, no fonts.  Trix is a general-purpose computation engine, not a
page-description language.  Think of it as PostScript's computational core, kept
and modernised, with the printer removed.

---

## 1. The parts you already know

The execution model is the PostScript model.

- **One operand stack.**  Values are pushed and popped; operators consume their
  arguments from the top.  `count` reports the depth, exactly as in PostScript.
- **A dictionary stack.**  `begin` pushes a dictionary, `end` pops it, and a
  bare name is resolved by walking the stack top-down, with `systemdict` at the
  bottom and `userdict` above it.  (Trix also provides dedicated `errordict`
  and `protocoldict` system dictionaries.)
- **Literal, executable, and immediate names.**  `/x` pushes the name as data,
  `x` looks it up and executes the result, and `//x` resolves *at scan time* --
  PostScript's immediately-evaluated name.
- **Procedures are data.**  `{ ... }` builds a procedure without running it;
  `exec`, `if`, `for`, and friends run it later.
- **`save` / `restore` snapshot the VM.**  `save` returns a token; `restore`
  rolls every change made since back out.  (In Trix this is transaction
  journaling over the VM heap -- see [Save/Restore](save-restore.md).)
- **`mark` and friends.**  `mark` drops a marker; `count-to-mark` and
  `clear-to-mark` work against it.

A dictionary-stack lookup and a `save`/`restore` rollback, verbatim:

```trix
(a name resolves through the dict stack) << /pi 3 >> begin pi end 3 eq assert

(restore rolls back everything since the save) /counter 0 def
save /snap exch def
    /counter 99 def
snap restore
counter 0 eq assert
```

Names behave as you expect -- slash is data, bare is code:

```trix
/double { 2 mul } def
(an executable name runs its value) 5 double 10 eq assert
(a slashed name is just data)       /double type /name-type eq assert
```

> Note: `assert` takes its message first -- `(why) bool assert` -- and `=`
> prints and drops the top of the stack.  Reals print without a trailing `.0`
> (`10.0` shows as `10`).

### Operators that carried over unchanged

These keep their PostScript names and meanings:

| Category   | Operators                                                       |
| ---------- | --------------------------------------------------------------- |
| Stack      | `dup` `exch` `pop` `copy` `index` `roll` `clear` `count` `mark` |
| Dictionary | `def` `begin` `end` `dict` `load` `where`                       |
| Access     | `get` `put` `length`                                            |
| Control    | `exec` `if` `for` `repeat` `loop` `exit`                        |
| Memory     | `save` `restore`                                                |
| Allocation | `string` `array`                                                |

---

## 2. Same idea, different spelling

Trix hyphenates the multi-word operators that PostScript runs together.  If a
name reads as two words, put a hyphen in it and you are usually right.

| PostScript    | Trix            | Notes                                  |
| ------------- | --------------- | -------------------------------------- |
| `ifelse`      | `if-else`       | `bool { } { } if-else`                 |
| `forall`      | `for-all`       | iterate an array, dict, string, or set |
| `counttomark` | `count-to-mark` |                                        |
| `cleartomark` | `clear-to-mark` |                                        |
| `getinterval` | `get-interval`  |                                        |
| `known`       | `known?`        | predicates end in `?`                  |
| `cvi` / `cvr` | `cast`          | `value /real-type cast` (see below)    |
| `cvs`         | `to-string`     | `any dst -- str`                       |

A few PostScript operators have no direct Trix counterpart: `currentdict`,
`aload` / `astore`, and the `cvx` / `cvlit` executability toggles map onto
different Trix mechanisms (executable strings, `make-executable`, the array
operators in [Collections](collections.md)).  PostScript's `count` /
`cleartomark` idioms work; `depth` is spelled `count`.

---

## 3. What's gone

Everything tied to the imaging model.  There is no graphics state and therefore
no `gsave` / `grestore`, no `currentpoint` / `moveto` / `lineto` / `curveto`, no
`fill` / `stroke` / `clip`, no `setrgbcolor` / `setgray`, no `image`, no fonts
or `show`, and no `showpage`.  (The local/global VM split is *not* on this
list -- it carries over with a different surface; see
[Memory model](#44-memory-model-local-and-global-vm).)

If you embed Trix in a host that *does* draw, you expose your drawing primitives
as host operators -- see [Host Integration](host-integration.md).

---

## 4. What's new

This is where Trix departs from PostScript in substance, not just spelling.

### 4.1 Strict typing

PostScript is loosely dynamic: mixed-type arithmetic silently promotes (`5 2.0
mul` yields `10.0`), and access control is advisory.  Trix is **strictly**
dynamic -- still no static types, but the interpreter verifies every operator's
operands and refuses to guess.

- **No implicit numeric coercion.**  Operands must already share a type;
  convert explicitly with `cast`.

```trix
(PostScript promotes here; Trix will not) { 5 2.0 mul } try /type-check eq assert
(cast first, then the types match)        5 /real-type cast 2.0 mul 10.0 eq assert
```

- **Access control is enforced by the VM**, not by convention.  A value built
  read-only (`#r`) rejects mutation with a real error:

```trix
(a read-only string rejects put) { (frozen)#r 0 65b put } try /read-only eq assert
```

- **A richer type tower.**  Where PostScript has integer / real, Trix has
  `Byte`, `Integer`, `UInteger`, `Long`, `ULong`, `Real`, `Double`, `Int128`,
  and `UInt128`, plus first-class `Name`, `Record`, `Tagged`, `Coroutine`, and
  more.  `type` returns a name (`/integer-type`, `/string-type`, ...), the
  direct analogue of PostScript's `integertype` / `stringtype`, and Trix adds
  predicates (`is-integer`, `is-string`, `is-number`, ...) so you rarely need
  `type ... eq`.  See [Type System](type-system.md) and
  [Types Cheatsheet](types-cheatsheet.md).

### 4.2 A real exception model

PostScript error handling is `stopped` plus the `errordict` / `$error`
machinery: `{ ... } stopped` returns a bare boolean, and recovering the *cause*
means digging through `$error`.

Trix keeps the structure but makes it first-class.  `try` is `stopped` that
returns the **error name** (or `/no-error` when the body ran clean):

```trix
(try yields the error's name, not a bare boolean) { 1 0 div } try /div-by-zero eq assert
(/no-error signals a clean run)                   { 6 7 mul pop } try /no-error eq assert
```

`throw` raises a named error; `throw-with` attaches structured data; and a
handler dictionary dispatches by error name (`try-catch`), with the cause
available through `last-error`, `last-error-message`, and `last-error-data`:

```trix
(a handler can dispatch by name and read structured error data)
<< /range-check { pop last-error-data /min get } >>
{ /range-check << /min 0 /max 9 >> throw-with }
try-catch
0 eq assert
```

`rethrow` re-raises the current error after cleanup, and unhandled errors fall
through to a customizable global handler.  The error vocabulary will look
familiar -- `/type-check`, `/range-check`, `/undefined`, `/opstack-underflow`,
`/dict-full` echo PostScript's `typecheck`, `rangecheck`, `undefined`,
`stackunderflow`, `dictfull` -- just with finer granularity (PostScript's
`invalidaccess`, for instance, splits into `/read-only` and `/invalid-access`).
The full model is in [Error Handling](error-handling.md) and
[Errors Cheatsheet](errors-cheatsheet.md).

### 4.3 Modern composite types and control

PostScript gives you arrays, dictionaries, and strings.  Trix keeps those and
adds a layer PostScript never had:

```trix
% Records -- the typed, immutable evolution of the dict-as-object
/point [ /x /y ] record-type def
(a record has named fields) 3 4 point /x get 3 eq assert

% Tagged values -- sum types for algebraic data
(a tag discriminates a variant) 42 /some tag /some tag? assert
```

- **[Records](record.md)** -- product types with named, immutable fields.
- **[Tagged Values](tagged-values.md)** -- sum types / discriminated unions.
- **[Pattern Matching](pattern-matching.md)** -- `let`, `destructure`, and
  `match` name stack values and deconstruct data instead of juggling `roll` and
  `index`.
- **[Protocols](protocols.md)** and **[Modules](modules.md)** -- open type
  dispatch and scoped namespaces.
- **Concurrency** -- [Coroutines](coroutines.md), [Pipelines](pipeline.md),
  [Actors](actors.md), and [Supervision](supervision.md): cooperative
  multitasking on the shared heap.
- **[Lazy Sequences](lazy-sequences.md)**, **[Reactive Cells](reactive.md)**,
  **[Logic](logic.md)**, **[Effects](effects.md)**, and
  **[Contracts](contracts.md)** -- higher-level computation models.
- **Infix** -- `$( max(3, 7) )` desugars to `3 7 max` when postfix is awkward
  (see [Scanner Syntax](scanner-syntax.md)).

### 4.4 Memory model: local and global VM

The one Level-2 idea that carries over in substance is the split between
**local VM** and **global VM**. The asymmetry is inherited almost verbatim;
what changed is *how* you pick a region and *why* the global heap exists.

Inherited unchanged:

- Two heaps. The **local VM** is reclaimed by `save` / `restore`; the
  **global VM** survives `restore` and is reclaimed only by garbage collection.
- `restore` reverts the local heap and undoes the mutations recorded since the
  matching `save` (array writes, dict entries, name bindings). Global
  allocations and mutations are left alone.
- The **one-way reference rule**: a global container may not hold a reference to
  a local object -- PostScript raises `invalidaccess`, Trix raises
  `/invalid-access`. The reverse (a local container holding global data) is
  always legal. The reason is the same in both: the local reference would
  dangle once `restore` reclaimed it.

So if you wrote PostScript that respected `invalidaccess`, the instinct
transfers directly -- you build the inner object in the region you store it
into, which in Trix means a `${...}` block:

```trix
/d ${ << >> } def
save /sv exch def
(a locally-built array cannot go into a global dict) { d 7 [ 1 2 3 ] put } try /invalid-access eq assert
(build the array in global with dollar-brace; now the store works) d 7 ${ [ 1 2 3 ] } put
d 7 get 1 get 2 eq assert
sv restore
(and the global array survives restore) d 7 get 1 get 2 eq assert
(ok) =
```

What differs:

| Aspect | PostScript | Trix |
| --- | --- | --- |
| Choosing the region | Modal `setglobal` / `currentglobal`; everything allocated while the mode is set -- including scanned literals -- lands in that region | Lexical `${...}` (plus `$/name`, the `#=` eqref family, the `-persist` operators); only operator *results* are made global, so a literal scanned inside `${...}` stays local |
| Why global exists | Sharing across execution contexts: each context has a private local VM, and global VM is the shared arena for fonts and resources | Outliving a `save` scope; one save lineage is shared across all coroutines, so there is no per-context private heap to escape |
| Name objects | Simple objects -- a name has no VM residence, so it never participates in the rule and a name key stores anywhere | Heap-allocated and region-bound -- a name first interned above a save barrier is local and restore-fragile, so it must be interned global (`$/k` or `${ /k }`) before it keys a global container |
| Rolling back a global write | Never -- `restore` does not touch global VM | Almost never -- the lone exception is logic-variable bindings, journaled so backtracking can undo them |
| Local-heap reclamation | `save` / `restore` plus a garbage collector that also reclaims unreachable local VM | `save` / `restore` only; there is no local-heap GC, just the global mark-sweep collector |

Two Trix-only contracts have no PostScript analog: the `-persist` and root `#=`
(eqref) families bypass journaling, so they reject *any* above-barrier operand
-- even a plain number -- with `/above-barrier`. The full rules, with the
value-vs-container test that tells you exactly when a store needs help, are in
[Local and Global VM](local-global-vm.md); for the allocator and collector
internals see [VM Regions](vm-regions.md) and [Global Heap / GC](gvm-heap-gc.md).

---

## 5. A worked translation

A guarded operation. In PostScript you reach for `stopped`:

```postscript
% PostScript: stopped returns true/false; the cause lives in $error
{ 1 0 div } stopped { handleerror } if
```

In Trix the same guard tells you *which* error fired:

```trix
(the guard sees the specific error) { 1 0 div } try /div-by-zero eq assert
```

An "object" as a dictionary. PostScript leans on `dict`:

```postscript
% PostScript: a 2-entry dict used as a record
/Point 2 dict def
Point /x 3 put
Point /y 4 put
Point /x get        % => 3
```

In Trix that pattern grows up into a typed, immutable record (the loose dict is
still available when you want it):

```trix
/Point [ /x /y ] record-type def
(the typed, immutable version of the dict-as-object) 3 4 Point /x get 3 eq assert
```

---

## 6. Where to go next

- **[Trix Reference](trix-reference.md)** -- every operator, with stack effects
  and error behaviour.
- **[Operator Cheatsheet](operator-cheatsheet.md)**,
  **[Types Cheatsheet](types-cheatsheet.md)**,
  **[Errors Cheatsheet](errors-cheatsheet.md)** -- quick lookups.
- **[Scanner Syntax](scanner-syntax.md)** -- names, numbers, literals,
  suffixes, and the infix forms in full.
- **[Type System](type-system.md)** and **[Error Handling](error-handling.md)**
  -- the two areas where Trix is most deliberately stricter than PostScript.
- **[examples/](../examples/README.md)** -- complete programs to read and run.
