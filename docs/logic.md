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

# Logic / Backtracking: Technical Reference

## 1. Overview

The logic layer adds Prolog-style logic programming to Trix, built
entirely on the existing save/restore transaction mechanism.  It provides
four capabilities:

1. **Logic variables (lvars)** -- first-class unknowns that can be unified with values
2. **Structural unification** -- pattern matching that binds variables and
   recurses into arrays, records, and tagged values
3. **Choice points with automatic backtracking** -- try alternatives; on
   failure, the VM state rolls back and the next alternative is tried
4. **Cut** -- commit to the current choice branch, pruning remaining
   alternatives (Prolog's `!`)

All operators are implemented in C++ within `src/ops_logic.inl` (~1815
lines), compiled into the single-header `trix.h`.  Zero external
dependencies.  The choice point mechanism integrates with the interpreter's
`try_catch_handler` via a new `@choice-barrier` control operator.

### Why logic programming in an embeddable runtime?

Most logic programming requires a full Prolog implementation (SWI-Prolog:
~50MB, SICStus: ~20MB).  MiniKanren-style embedding in Scheme or Python
requires continuation-passing or monadic frameworks that are hard to reason
about and impossible to serialize.

Trix's save/restore mechanism gives us backtracking for free:

- **Save** captures the VM state at a choice point
- **Restore** undoes all mutations (variable bindings, heap allocations,
  dict modifications) atomically
- **The operand stack survives restore** -- results propagate upward

This means logic programming in Trix is:

- **Correct by construction**: no manual trail management, no undo lists,
  no continuation-passing.  Save/restore handles rollback atomically.
- **Composable with concurrency**: lvars and choice points work
  within actors, inside supervised processes, alongside pipelines.
- **Serializable**: snap-shot/thaw captures lvars and bindings
  as part of the VM heap.  A running constraint search can be checkpointed
  and resumed.
- **Embeddable in 256KB**: 20 operators, ~1815 lines of C++.  No heap
  allocation beyond the VM's pre-allocated arena.

No other embeddable runtime offers fault-tolerant concurrent actors +
constraint solving + VM serialization in a single-header library.

---

## 2. Architecture

### 2.1 Logic Variables

A logic variable is a tagged value wrapping a 1-element array:

```
logic-var  -->  /lvar [null] tag
```

Memory layout on the VM heap:

```
Tagged pair (16 bytes):
  +0: Object  -- Name /lvar (tag name)
  +8: Object  -- Array (1-element, offset to binding cell)

Binding cell (8 bytes):
  +0: Object  -- null (unbound) or bound value
```

Total cost: **24 bytes per lvar** (16 for tagged pair + 8 for
the binding cell).

**Why a tagged value wrapping an array?**

| Alternative | Pros | Cons |
| --- | --- | --- |
| Dedicated heap struct | O(1) bound check | New type slot, new dispatch paths, snapshot impact |
| Tagged with integer ID + substitution dict | Standard Prolog approach | Dict lookup on every deref, GC pressure |
| **Tagged 1-element array** | **Zero new types, journaled by save/restore, O(1) deref** | 24 bytes per lvar (acceptable) |

The 1-element array is the key insight: Trix's save/restore already
journals array `put` operations.  When an lvar is bound (the
array element is written), the old null value is saved in the journal.
On restore, the null is written back -- the variable is unbound.  No
separate Prolog-style trail is needed (the one twist -- binding cells that
live in global VM -- is handled by §2.2).

### 2.2 Binding Mechanism

Binding is a journaled array write, following the same pattern as the
`put` operator:

```cpp
// From lvar_bind() in ops_logic.inl
auto elem_data = payload.array_objects(trx);
auto curr_save_level = trx->m_curr_save_level;
if (elem_data[0].save_level() != curr_save_level) {
    if (trx->is_global(trx->ptr_to_offset(&elem_data[0]))) {
        // lvar created inside ${...}: its cell lives in GLOBAL VM, yet the
        // binding must still be undone on backtracking -- journal it anyway
        // (tagged Flavor::LvarBinding, the one journaled-global slot).
        Save::save_object_journal_global(trx, &elem_data[0]);
    } else {
        Save::save_object(trx, &elem_data[0]);  // local cell: ordinary journal
    }
} else {
    elem_data[0].maybe_free_extvalue(trx);       // same save level: overwrite directly
}
elem_data[0] = value.make_copy(curr_save_level); // store the bound value
```

At BASE save level (no active save), bindings are unjournaled (permanent).
At any higher level, the journal entry ensures restore undoes the binding.

**The one global-VM exception.**  Global VM is otherwise journal-skipped:
writes to it survive `restore` -- that is exactly why §8.3 builds persistable
*results* in `${...}`.  A logic-variable *binding cell* is the sole
exception.  An lvar created inside `${...}` keeps its 1-element array in
global VM, yet a binding made during backtracking must be undone by the
per-alternative `restore` -- otherwise every alternative after the first
would unify against the stale binding and silently fail.  So `lvar_bind`
routes a global binding slot through `Save::save_object_journal_global`,
which tags the journal entry `Flavor::LvarBinding` -- the only journal
flavor whose recorded slot is intentionally global.  `restore` reverts it
exactly like a local binding, and `gc_walk_chain` pins the owning block so
the slot stays valid between the bind and the restore.  Logic-variable
bindings are therefore the one case in Trix where a write to global VM is
journaled (and thus rolled back).

### 2.3 Deref (Binding Chain Walk)

An lvar may be bound to another lvar (chaining).
`deref` walks the chain:

```
deref(term):
  while term is an lvar and term is bound:
    term = term's bound value
  return term
```

Depth-limited to 64 to guard against accidental cycles (no occurs check).
Returns the lvar itself if unbound -- this is how unbound variables
propagate through computation.

### 2.4 Unification Algorithm

Structural matching with deref-on-entry.  `unify(a, b)` returns true if
a and b can be made identical by binding unbound lvars.

```
unify(a, b):
  a = deref(a);  b = deref(b)
  if a == b:                          return true   (identical)
  if a is unbound lvar:  bind(a, b);  return true
  if b is unbound lvar:  bind(b, a);  return true
  if both arrays, same length:        unify elements pairwise
  if both records, same schema:       unify fields pairwise
  if both tagged, same tag:           unify payloads
  otherwise:                          return false
```

**Not atomic on failure**: when a structural match fails partway (e.g. a
later array element or record field mismatches), the lvars bound by the
*earlier*, matching sub-elements stay bound -- `unify` does no rollback of
its own.  Wrap it in a `choice` alternative or a `save`/`restore` scope when
you need bindings undone on a false result (see §3.2).

**Occurs check**: omitted.  Same as SWI-Prolog's default.  If user writes
`x [x] unify`, the deref chain becomes cyclic.  The depth limit prevents
infinite loops but the binding is logically unsound.  This is a pragmatic
trade-off: occurs check is O(n) per unification and rarely needed in
practice.

**Recursion depth**: limited to 64.  Exceeding raises `limit-check`.
Sufficient for any practical nested data structure.

**Object equality**: uses `Object::equal()` which treats strings and
names as interchangeable (`(hello)` equals `/hello`).  This matches
Trix's general equality semantics.

**Record schema comparison**: two records unify only if they have the
same schema.  If the same field names are used in separate `record`
calls (producing different schema offsets), field names are compared
pairwise.  Same-offset schemas use a fast path.

### 2.5 Choice Points

`choice` takes an array of alternative procs and tries them with
automatic backtracking.  This is implemented as a control operator pair
(`@choice-barrier` / `@choice-fail`) integrated into the interpreter's
error handling system.

#### Stack Layout

After `choice` sets up the first alternative:

```
Exec stack:   ... | alt_array | index | save_object | @choice-barrier | proc
```

- `alt_array` -- the alternatives array (allocated before choice's save,
  survives restore)
- `index` -- current alternative index (Integer, no VM reference)
- `save_object` -- on the exec stack (part of the 4-slot choice frame)

#### Success Path

When an alternative completes without raising `Fail`:

1. `@choice-barrier` fires (interpreter pops it from exec stack)
2. `at_choice_barrier_op` then pops the remaining 3 frame slots in one
   operation -- save_object, index, alt_array (`m_exec_ptr -= 3`)
3. The save level stays active (bindings persist)
4. Whatever the alternative left on the operand stack is the result

#### Failure Path

When an alternative raises `Error::Fail` (via `fail` or `guard`):

1. `try_catch_handler` scans exec stack backward for barriers
2. Finds `@choice-barrier`, replaces it with `@choice-fail`
3. Resets exec stack to `@choice-fail`, throws `Exception::Error`
4. `@choice-fail` fires:
   a. Checks `m_last_error` -- only `Error::Fail` triggers backtracking.
      Non-fail errors (div-by-zero, type-check, etc.) are re-raised.
   b. Pops save_object from exec stack
   c. Restores (undoes all bindings and allocations from failed alternative)
   d. Increments index
   e. If all alternatives exhausted: raises `Fail` (propagates to outer choice)
   f. Otherwise: saves again, pushes new `@choice-barrier` + next proc

#### Why Save Levels Accumulate

On success, the choice point's save level is not committed (discarded
without restoring).  This is deliberate:

Committing a save level would orphan journal entries needed by
higher-level restores.  If a choice succeeds inside a user's
`save`/`restore` scope, the user expects `restore` to undo everything
since their save -- including the choice's bindings.  Leaving the save
level active ensures this works correctly.

The cost is save level consumption: each successful choice uses one
level.  With `DefaultSaveCount = 64`, this allows 64 nested successful
choices before exhaustion.  `require_save_capacity` raises an error
before overflow.

#### Important: Choices Commit on Success -- Use Recursion for Multi-Stage Search

When an alternative succeeds, the choice point is **gone**: its
4-slot frame is popped from the exec stack (steps 1-3 above), and
nothing on the exec stack remembers the remaining alternatives.  A
later `Fail` will not rewind to it -- it propagates to the
next-outer `@choice-barrier` (or the user's `try`).

This has a sharp consequence for programs that chain multiple
search stages in sequence at the top level:

```
[ ... row 0 alternatives ... ] choice    % succeeds, commits
[ ... row 1 alternatives ... ] choice    % succeeds, commits
[ ... row 2 alternatives ... ] choice    % every alternative fails
-> raises Fail out of the whole program: row 0 and row 1 cannot
   be revisited because their choice points are already gone.
```

The correct shape is **recursion**, so every choice is textually
nested inside an alternative of the previous one:

```
/solve-from {
    /idx exch def
    idx N eq { } {                                  % base: done
        [
            { ... place value for cell idx ...  idx 1 add solve-from }
            { ... alternative                   idx 1 add solve-from }
            ...
        ] choice
    } if-else
} def

0 solve-from
```

Now a dead-end deep in the search raises `Fail`, which walks up the
exec stack through every pending `@choice-barrier` -- each one
rolled back and given a chance to try its next alternative -- until
either some level has an untried alternative that succeeds, or the
outermost choice exhausts.  This is the shape used by
`examples/sudoku.trx`.

See Section 4.10 ("Multi-Stage Search via Recursion") for a worked
example.  Sections 4.4 and 4.10 are the canonical patterns; 4.1 and
4.2 use a single top-level choice, which is only sufficient when
the search space collapses to one decision.

---

## 3. Operator Reference

### 3.1 Logic Variables

```
logic-var      -- lvar
```

Creates an unbound lvar.  Allocates 24 bytes on the VM heap
(16-byte tagged pair + 8-byte binding cell).

```
is-logic-var   any -- bool
```

Returns true if the operand is a tagged value with tag `/lvar`.
Non-destructive: does not consume the operand's type.

```
bound?         lvar -- bool
```

Returns true if the lvar is bound (inner array element is not
null).  Raises `type-check` if the operand is not an lvar.

```
deref          term -- value
```

Walks the binding chain.  If the term is an unbound lvar,
returns the lvar itself.  If the term is a bound lvar, returns
the concrete value at the end of the chain.  If the term is not an
lvar, returns it unchanged.  Depth limit: 64.

### 3.2 Unification

```
unify          a b -- bool
```

Structural unification.  Returns true if `a` and `b` can be made
identical by binding unbound lvars.  Returns false on mismatch.

**Not atomic:** `unify` does no rollback of its own -- sub-elements that
matched before a later mismatch stay bound even though `unify` returns
false.  For example `[ a 1 ] [ 9 99 ] unify` returns false yet leaves `a`
bound to 9.  Run the `unify` inside a `choice` alternative (or an explicit
`save`/`restore` scope) when you need the bindings undone on failure.

Supported structures for pairwise unification:

| Structure     | Condition                 | Comparison         |
| ------------- | ------------------------- | ------------------ |
| Arrays        | Same length               | Element-by-element |
| Records       | Same schema (field names) | Field-by-field     |
| Tagged values | Same tag name             | Payloads           |
| Scalars       | Same type and value       | `Object::equal()`  |

Errors: `limit-check` if recursion depth exceeds 64.

### 3.3 Backtracking

```
fail           --
```

Raises `Error::Fail`.  Caught by the nearest `@choice-barrier` on the
exec stack.  If no choice point exists, propagates as a normal error
(catchable by user `try`).

```
guard          bool --
```

If false, calls `fail`.  Equivalent to `{ } { fail } if-else` but
implemented as a single C++ operator for performance and clarity.

```
choice         array -- ?
```

Takes an array of executable procs (alternatives).  Tries each in order
with automatic backtracking via save/restore.

- On success: returns whatever the successful alternative left on the
  operand stack
- On all-fail: raises `Error::Fail` (propagates to outer choice or
  user `try`)
- Non-fail errors propagate through choice (not caught)
- Empty array: immediate `Fail`

```
cut            --
```

Commits to the current branch of the nearest enclosing `choice`,
discarding all remaining alternatives.  After `cut`, a subsequent
`fail` will not backtrack to this choice point -- it propagates to the
enclosing choice or `try`.

- Raises `Error::InvalidExit` if there is no enclosing choice point
- Cannot cross `@run` or `@stop` boundaries

Implementation: `cut` nulls the 4-slot choice frame on the exec stack
with executable Null objects (interpreter no-ops).  The remaining proc
body continues executing normally.  The save level stays active (bindings
persist, same as the success path).

### 3.4 Copy-Term

```
copy-term      term -- term'
```

Deep copy of a term with fresh lvars.  Walks the term tree:

- **Scalars** (integer, real, string, name, boolean, etc.) -- cloned
- **Unbound lvar** -- replaced with a fresh lvar; if the same old lvar
  appears in multiple positions, all copies map to the same fresh lvar
  (shared variable preservation)
- **Bound lvar** -- dereferenced, then the bound value is copied
- **Array** -- new array, each element recursively copied
- **Tagged** -- new tagged pair, payload recursively copied
- **Record** -- new instance (same schema), fields recursively copied

Depth-limited to `MaxCopyTermDepth` (64) to guard against deep nesting.
Uses temp-region arrays for the lvar mapping (max 256 distinct lvars).

```
/x logic-var def
[ x 1 x ] copy-term     % -> [ y 1 y ]  (y is fresh, shared)
```

### 3.5 Negation-as-Failure

```
naf            proc -- bool
```

Runs `proc` under a fresh save point and reports whether it *failed*:

- `proc` raises `Error::Fail` (e.g. via `fail`, a false `guard`, or an
  exhausted `choice`) -- the goal is unsatisfiable, so `naf` pushes
  `true`.
- `proc` succeeds -- the goal is satisfiable, so `naf` pushes `false`.

Either way **all variable bindings made by `proc` are rolled back** when
`naf` returns: it is a pure test, not a binding step.  Non-`Fail` errors
(e.g. `/type-check`) propagate out of `naf` rather than being reported as
failure.  The argument may be a proc, a curry, or a continuation.

This is the operator form of the hand-rolled
`{ condition guard cut fail } try` idiom (see §4.7) -- prefer `naf` for
clarity.

```
/x logic-var def

% A goal that fails -> naf succeeds.
(naf of a failing goal) { x 1 unify guard  x deref 2 eq guard } naf assert

% A goal that succeeds -> naf returns false...
(naf of a succeeding goal) { x 1 unify guard } naf not assert
% ...and naf rolled the binding back: x is unbound again.
(naf rolls bindings back) x bound? not assert
```

`naf` composes with `find-all`/`find-n`/`aggregate` (§8.3): use it inside
a generate-and-test alternative to keep only solutions for which a
secondary goal has *no* proof.

### 3.6 Solution Collection and Search

These operators run alternatives and collect or fold their results.  Each
runs every alternative under its own save point, so results must survive
the per-alternative `restore` (see section 8.3 for the persistability rule
and the `${...}` workaround for heap results).

```
find-all          [procs] -- arr                % collect results of all succeeding alternatives
find-n            n [procs] -- arr              % collect the first n results (short-circuits)
once              proc -- ?                     % run proc; commit to its first solution, no backtracking
choice-count      [procs] -- int                % count succeeding alternatives (no results collected)
for-each-solution body [alts] --                % run body once per succeeding alternative
aggregate         init reducer [alts] -- result % fold solutions: reducer sees (acc result)
unify-match       value [[pat body]...] -- ?    % first [pattern body] whose pattern unifies wins
```

`once` collapses a search to a single solution: it runs `proc` (typically
wrapping a `choice`) and commits to the first success, discarding the
remaining alternatives.

`aggregate` folds on the fly -- the alternative's result may be any type
(it is consumed by the reducer before `restore`); only the accumulator
must be persistable.

`unify-match` is the unification-based dispatch form: it tries each
`[pattern body]` pair in order, and on the first `pattern` that unifies
with `value`, runs `body` with the bindings intact.

### 3.7 Named Logic Variables

```
named-var         name -- lvar              % create an unbound lvar with a debug name
lvar-name         lvar -- name true | false % read the debug name (false if unnamed)
```

`named-var` is `logic-var` with an attached debug name, useful when
inspecting bindings.  `lvar-name` returns the name and `true`, or just
`false` for an anonymous lvar.

See `docs/trix-reference.md` (section 3.31) for the full reference card
with worked examples for each of these operators.

---

## 4. Programming Patterns

### 4.1 Basic Search

Find a value satisfying a condition:

```
/x logic-var def
[
    { x 1 unify guard }
    { x 2 unify guard }
    { x 3 unify guard }
] choice
x deref =        % -> 1 (first succeeding alternative)
```

### 4.2 Generate-and-Test

Generate candidates, filter with guards:

```
/x logic-var def
[
    { x 1 unify guard   x deref 2 mod 0 eq guard }
    { x 2 unify guard   x deref 2 mod 0 eq guard }
    { x 3 unify guard   x deref 2 mod 0 eq guard }
    { x 4 unify guard   x deref 2 mod 0 eq guard }
] choice
x deref =        % -> 2 (first even number)
```

### 4.3 Structural Pattern Matching

Unify against complex structures to extract components:

```
/val logic-var def
/key logic-var def

% Match a tagged value
42 /:ok tag
val /:ok tag unify guard
val deref =      % -> 42

% Match an array pattern
/a logic-var def
/b logic-var def
[ 1 a 3 ] [ 1 2 3 ] unify guard
a deref =        % -> 2

% Match a record
/name logic-var def
(Alice) 30 [ /name /age ] record
name 30 [ /name /age ] record
unify guard
name deref =     % -> (Alice)
```

### 4.4 Nested Choice (Search Trees)

Model a search tree with nested choices:

```
% Find x, y such that x + y = 10 and x > y
/x logic-var def
/y logic-var def
% Each alternative validates its own x,y pair *inside* the alternative:
% post-choice guards cannot re-enter a committed choice (see section 2.5).
[
    { x 1 unify guard
      [ { y 9 unify guard } { y 8 unify guard } ] choice
      x deref y deref add 10 eq guard
      x deref y deref gt guard }
    { x 6 unify guard
      [ { y 4 unify guard } { y 3 unify guard } ] choice
      x deref y deref add 10 eq guard
      x deref y deref gt guard }
    { x 7 unify guard
      [ { y 3 unify guard } { y 2 unify guard } ] choice
      x deref y deref add 10 eq guard
      x deref y deref gt guard }
] choice
x deref =  y deref =    % -> 6 4 (first satisfying pair)
```

### 4.5 Configuration Validation

Validate a configuration against constraints:

```
% Validate that a server config is consistent
/server-config << /port 8080 /tls true /cert (server.pem) >> def

/port logic-var def
/tls logic-var def
/cert logic-var def

% Extract fields via unification
port server-config /port get unify guard
tls server-config /tls get unify guard
cert server-config /cert get unify guard

% Validate: if TLS is enabled, cert must not be empty
tls deref {
    cert deref length 0 gt guard
} if

(Config valid: port=) print port deref =
```

### 4.6 Rule Engine

Encode rules as choice alternatives:

```
% Simple diagnostic rule engine
/diagnose {
    % symptom dict on stack
    /symptoms exch def
    /cause logic-var def

    [
        {
            cause /overheating unify guard
            symptoms /temp get 90 gt guard
            symptoms /fan get (off) eq guard
        }
        {
            cause /memory-leak unify guard
            symptoms /mem-usage get 95 gt guard
            symptoms /uptime get 3600 gt guard
        }
        {
            cause /disk-full unify guard
            symptoms /disk-pct get 98 gt guard
        }
        {
            cause /unknown unify guard
        }
    ] choice

    cause deref
} def

% Usage
<< /temp 95 /fan (off) /mem-usage 40 /disk-pct 50 /uptime 100 >>
diagnose =       % -> /overheating
```

### 4.7 Pruning with Cut

Use `cut` to commit to the first matching branch and prevent
unnecessary backtracking:

```
% Classify a number -- once matched, don't try other rules
/classify {
    /n exch def
    /result logic-var def
    [
        { n 0 lt guard  result /negative unify guard  cut }
        { n 0 eq guard  result /zero unify guard      cut }
        { result /positive unify guard                 cut }
    ] choice
    result deref
} def

-5 classify =     % -> /negative
0  classify =     % -> /zero
7  classify =     % -> /positive
```

Without `cut`, a failure after the matching branch would backtrack
and try subsequent alternatives.  With `cut`, the choice is committed
and any later `fail` propagates outward.

Common `cut` idioms:
- **Deterministic rules**: `cut` after the first matching clause
  (equivalent to Prolog's green cut)
- **Negation-as-failure**: `{ condition guard  cut  fail } try` --
  succeeds only when `condition` is false (the dedicated `naf` operator,
  §3.5, is the idiomatic form)
- **First-match dispatch**: `cut` in each branch of a classifier

### 4.8 Type Inference (Simplified)

Use unification to propagate type constraints:

```
% Infer the return type of a binary operation
/infer-binop {
    % op ltype rtype -- result-type
    /rtype exch def
    /ltype exch def
    /op exch def
    /result logic-var def

    [
        % int + int -> int
        { ltype /int unify guard  rtype /int unify guard
          result /int unify guard  op /add eq guard }
        % int + real -> real (promotion)
        { ltype /int unify guard  rtype /real unify guard
          result /real unify guard  op /add eq guard }
        % real + int -> real (promotion)
        { ltype /real unify guard  rtype /int unify guard
          result /real unify guard  op /add eq guard }
        % real + real -> real
        { ltype /real unify guard  rtype /real unify guard
          result /real unify guard  op /add eq guard }
        % any comparison -> bool
        { result /bool unify guard
          [ { op /eq eq guard } { op /lt eq guard } { op /gt eq guard } ] choice }
    ] choice

    result deref
} def

/add /int /int infer-binop =     % -> /int
/add /int /real infer-binop =    % -> /real
/eq /real /real infer-binop =    % -> /bool
```

### 4.9 Constraint Satisfaction (N-Queens)

The N-queens problem is a canonical example of multi-stage search.
Each row must pick a column for its queen; placements must not share
a column or diagonal with any queen on earlier rows.  This is the
paradigm case where sequential top-level `choice` calls do not work
(see section 2.5 "Choices Commit on Success"): row 1's choice must
be able to force row 0 to retry a different column, which only
happens if row 1's choice is textually nested inside row 0's chosen
alternative.

The recursive shape:

```
% Check if placing queen at (row, col) conflicts with queens in
% rows 0 .. row-1.
/safe? {
    % queens-array row col -- bool
    /col exch def
    /row exch def
    /queens exch def
    /ok true def

    0 1 row 1 sub {
        /r exch def
        queens r get /c exch def
        c col eq { /ok false def } if
        c col sub abs row r sub abs eq { /ok false def } if
    } for

    ok
} def

% Place a queen in `row` of `queens`, then recurse to row+1.  The
% choice stays active until the recursive call returns, so a failure
% at any later row can rewind here and try a different column.
/place-queens {
    % queens row -- queens
    /row exch def
    /queens exch def
    row 4 eq { queens } {
        [
            { queens row 0 put  queens row 0 safe? guard
              queens row 1 add place-queens }
            { queens row 1 put  queens row 1 safe? guard
              queens row 1 add place-queens }
            { queens row 2 put  queens row 2 safe? guard
              queens row 1 add place-queens }
            { queens row 3 put  queens row 3 safe? guard
              queens row 1 add place-queens }
        ] choice
    } if-else
} def

[ 0 0 0 0 ] 0 place-queens ==   % first solution found
```

The board is an array whose `put` is journaled by save/restore, so
rolling back a choice alternative undoes earlier placements
automatically.  The `safe?` guard prunes each placement before
recursing.

Note: an earlier version of this section chained four top-level
`choice` calls (one per row).  That pattern appears to work but
cannot backtrack across rows -- if row 2 has no valid placement, it
cannot rewind row 0 or row 1 because their choices already
committed.  Always nest the choices via recursion.

---

## 5. Cost Model

### 5.1 Memory Costs

| Item                      | Bytes | Notes                                                             |
| ------------------------- | ----- | ----------------------------------------------------------------- |
| Logic variable            | 24    | 16 (tagged pair) + 8 (binding cell)                               |
| Binding (journaled)       | 12    | Save::Entry (12 bytes: next + ptr + bucket_count + flavor + data) |
| Choice point save barrier | 4     | vm_offset_t on heap                                               |
| Choice point exec state   | 32    | 4 Objects on exec stack (array + index + save + barrier)          |
| Save level slot           | 16    | save_stack + vm_temp_save + extvalue_active_save                  |

**Per-choice-point total**: ~48 bytes (barrier + exec frame + save slot).

**Per-variable-binding total**: ~36 bytes (binding cell write + journal entry).

### 5.2 Time Costs

| Operation                   | Cost     | Notes                                          |
| --------------------------- | -------- | ---------------------------------------------- |
| `logic-var`                 | O(1)     | Two heap allocations                           |
| `deref`                     | O(d)     | d = chain depth, max 64                        |
| `unify` (scalars)           | O(1)     | One `equal()` call                             |
| `unify` (arrays, length n)  | O(n * d) | Pairwise, depth d per element                  |
| `unify` (records, k fields) | O(k * d) | Pairwise + schema comparison                   |
| `fail`                      | O(s)     | s = exec stack depth to nearest choice barrier |
| `choice` setup              | O(1)     | One save + stack pushes                        |
| `restore` on fail           | O(j)     | j = journal entries since save                 |
| `guard`                     | O(1)     | Boolean check + conditional fail               |
| `cut`                       | O(e)     | e = exec stack depth to nearest choice barrier |

### 5.3 Save Level Budget

`DefaultSaveCount = 64`.  Each successful choice consumes one save level.
Nested choice points consume levels during execution and release them on
restore (failure) or leave them active (success).

Worst case: 64 nested successful choices.  This is generous for any
practical logic program.  If exhausted, `require_save_capacity` raises
`limit-check`.

`MaxSaveCount = 255` (configurable via Config struct).  Hosts expecting
deep logic nesting can increase this at construction time.

### 5.4 Error Stack Budget

Choice points do not use the error stack.  The save token is stored on
the exec stack as part of the 4-slot choice frame.  This means deep
choice nesting is limited by exec stack depth (default 2048), not error
stack depth (default 64).

---

## 6. Design Decisions

### 6.1 Why `guard` instead of `require`

The name `require` was already taken by the module system (`require-module`
loads a file with dedup).  `guard` is the standard term in constraint logic
programming and pattern matching (Erlang, Haskell, Prolog).

### 6.2 Why `unify` returns bool (not auto-fail)

Two options were considered:

| Option          | Stack effect  | Behavior on mismatch          |
| --------------- | ------------- | ----------------------------- |
| `unify -> bool` | `a b -- bool` | Returns false; caller decides |
| `unify -> --`   | `a b --`      | Calls fail automatically      |

The bool-returning variant is more composable:

```
/x logic-var def
x 42 unify { (bound) = } { (cannot bind) = } if-else % conditional
x 42 unify guard                                     % auto-fail
x 42 unify not { /error throw } if                   % custom error
```

The `unify guard` idiom provides the auto-fail behavior when needed.

### 6.3 Why no occurs check

The occurs check prevents cyclic bindings (e.g., `x [x] unify`).  It
requires traversing the entire term being bound to verify the variable
does not appear in it.  This is O(n) per unification.

Standard Prolog (ISO) specifies occurs check but SWI-Prolog omits it by
default for performance.  Most practical Prolog programs never create
cyclic terms.  Trix follows SWI-Prolog's pragmatic approach.

The depth limit on `deref` (64) prevents infinite loops if a cycle is
accidentally created.

### 6.4 Why choice uses a new barrier type

Three mechanisms were considered for `fail`/`choice` interaction:

| Mechanism | Integration | Complexity |
| --- | --- | --- |
| Trix-level `try-catch` | Wraps each alternative in try | Requires heap-allocated catch dict per alternative |
| Dedicated control operator | `@choice-barrier` in try_catch_handler | ~15 lines added to existing handler |
| Separate exception mechanism | Bypass try_catch_handler entirely | Duplicates unwinding logic |

The control operator approach adds minimal code to the existing error
handling path and correctly interacts with all other barrier types
(try, try-catch, finally, with-stream).

### 6.5 Why save levels are not committed on success

Committing a save level (removing it without restoring) would orphan
journal entries.  If a choice succeeds inside a user's save/restore
scope, the journal entries for the choice's bindings would be lost.
A later `restore` would fail to undo those bindings.

Leaving the save level active is correct: all bindings remain journaled
and are properly undone by any enclosing restore.

The trade-off is save level consumption (one per successful choice).
With `DefaultSaveCount = 64`, this is acceptable for all practical
use cases.

### 6.6 Why non-fail errors propagate through choice

If code inside a choice alternative raises a non-fail error (e.g.,
division by zero), it should NOT trigger backtracking.  Backtracking
is for logical failure ("this alternative doesn't satisfy the
constraints"), not for bugs or runtime errors.

`@choice-fail` checks `m_last_error`: if it is `Error::Fail`, it
backtracks.  Any other error is re-raised after cleaning up the choice
point's exec and error stack state.

### 6.7 Why cut nulls the exec stack (not stack surgery)

Three approaches were considered for `cut`:

| Approach | Mechanism | Drawback |
| --- | --- | --- |
| Remove slots from middle of exec stack | memmove remaining elements down | O(n) shuffle, fragile pointer arithmetic |
| Store save on error stack, dig past intermediate entries | Count barriers to find the right error stack slot | Couples cut to every barrier type; error-stack compaction |
| Null the frame with executable Null | Interpreter treats exec-Null as no-op | 4 wasted slots (harmless, reclaimed when proc finishes) |

The null approach is O(1), requires no knowledge of intermediate barrier
types, and avoids both stack compaction and error-stack surgery.  The
4 Null slots are consumed as no-ops when the interpreter reaches them.

This design required moving the save token from the error stack into
the exec stack as a choice frame companion (4 slots total: alt_array,
index, save, barrier).  This simplifies both `cut` and the
`@choice-barrier`/`@choice-fail` handlers.

---

## 7. Interaction with Other Layers

### 7.1 Save/Restore

Lvars and choice points are built on save/restore.  Key
interactions:

- **Bindings inside a save scope**: journaled normally.  `restore` undoes
  them.
- **Choice inside a save scope**: the choice's save level is nested
  inside the outer save.  Restoring the outer level also undoes the
  choice's bindings.
- **Logic variables created before a save**: the variable persists
  across restore (it is below the barrier).  Bindings made after the
  save are undone.
- **Logic variables created after a save**: the variable is above the
  barrier.  If it is on the operand stack during restore, `check_stack`
  raises an error (tagged values reference VM heap).  Create variables
  before the save scope or `deref` them to scalars before restore.

### 7.2 Coroutines and Actors

The save stack is global (shared across all coroutines).  Key constraints:

- **Choice inside a coroutine**: works correctly as long as the choice
  completes (succeeds or fails) before the coroutine yields.
- **Yielding inside a choice alternative**: the save level remains active
  while other coroutines run.  If another coroutine does save/restore at
  a higher level, the LIFO ordering is maintained.  However, if another
  coroutine does save/restore at a LOWER level, it would undo the choice's
  bindings.  This is a pre-existing constraint on save/restore with
  coroutines, not specific to logic programming.
- **Cross-coroutine safety**: `restore` now scans all suspended coroutine
  stacks for heap-referencing objects above the barrier.  If a suspended
  coroutine holds an array, record, or other composite object allocated
  after the save, `restore` raises `invalid-restore` instead of silently
  creating dangling references.  Coroutines that were not active during
  the save scope are skipped (optimization via `m_activation_sl`).
- **Recommendation**: keep choice points entirely within a single
  coroutine execution slice (no yields inside alternatives).

### 7.3 Records and Tagged Values

Unification recurses into records and tagged values:

- **Records**: two records unify if they have the same schema (field
  names in the same order) and all fields unify pairwise.  Different
  schema offsets are compared by field names.
- **Tagged values**: two tagged values unify if they have the same tag
  name and their payloads unify.
- **Logic variables inside structures**: fully supported.  An lvar
  inside an array, record, or tagged value can be bound by
  unification.

```
% lvar inside an array
/x logic-var def
[ 1 x 3 ] [ 1 2 3 ] unify guard
x deref =    % -> 2

% lvar inside a tagged value
/y logic-var def
y /:some tag  99 /:some tag unify guard
y deref =    % -> 99
```

### 7.4 Snap-shot/Thaw

Lvars are standard heap objects (tagged values wrapping arrays).
They are captured automatically by snap-shot as part of the VM heap blob.

- **Bindings**: stored as array element values on the heap.  Captured.
- **Active save levels**: the save stack is captured.  Choice points'
  save levels survive thaw.
- **Pre-interned `/lvar` name**: a `WellKnownName::LVar` entry in the
  well-known names table, which snap-shot serializes and thaw restores.

A running logic search can be checkpointed (snap-shot) and resumed
(thaw) on a different machine.

### 7.5 Composability Patterns

The six composability features -- protocols, closures, contracts, pattern
matching, transducers, and GenServer -- interact with logic/backtracking
in useful ways.

#### 7.5.1 Closures + Logic Variables

A closure captures the lvar *reference*, not a snapshot of its binding.
Bind the variable after capture; the closure sees the new binding:

```
/x logic-var def
{ x deref 10 mul } [/x] closure-capture /compute exch def
x 5 unify pop
compute exec =    % -> 50
```

This works because closures freeze the dict entry (a tagged lvar object
pointing into the heap), while `unify` writes the binding into the heap
array.  `deref` follows the heap pointer, so captured and live references
are equivalent.

#### 7.5.2 find-all + Transducers

Collect solutions with `find-all`, then filter and transform with a
composed transducer:

```
/n logic-var def
[
    { n 1 unify guard  n deref }
    { n 2 unify guard  n deref }
    { n 3 unify guard  n deref }
    { n 4 unify guard  n deref }
    { n 5 unify guard  n deref }
    { n 6 unify guard  n deref }
] find-all
{ 2 mod 0 eq } xf-filter { 10 mul } xf-map xf-compose
into    % => [20 40 60]
```

Here each alternative `deref`s to an integer, so `find-all` yields a
scalar array; structured results work too when built in `${...}` (see
§8.3).  The transducer filters for even values and scales by 10 in a
single pass.

#### 7.5.3 Protocol Guards

Check `protocol-satisfies?` as a guard before entering unification:

```
[/describe] /Describable def-protocol
{ pop (number) } /describe /integer-type def-method

/x logic-var def
42 dup /Describable protocol-satisfies? guard
x exch unify pop
x deref describe =    % -> (number)
```

If the value does not satisfy the protocol, `guard` triggers backtracking
before any unification occurs.

#### 7.5.4 Precondition vs Guard

`precondition` raises `/require` (a hard error caught by `try`).
`guard` raises `/fail` (backtracks via `choice`).  Choose based on
whether failure means "bug" or "try the next alternative":

```
% Hard error: caught by try, does NOT backtrack
{ 0 0 gt precondition } try =    % -> /require

% Backtrack: guard failure triggers next choice arm
[
    { 0 0 gt guard  (positive) }
    { (non-positive) }
] choice =    % -> (non-positive)
```

Use `precondition` for invariants that should never be false.
Use `guard` for conditions that prune the search space.

#### 7.5.5 GenServer + Unify

A GenServer handler can decompose structured messages with unification.
Fresh lvars are created per call:

```
mark {
    <<
        /init { 0 }
        /handle-call {
            /state exch def  /from exch def  /msg exch def
            /op logic-var def  /val logic-var def
            msg [ op val ] unify guard
            op deref /add eq {
                state val deref add
            } {
                state val deref mul
            } if-else
            dup /reply
        }
    >> gen-server /calc exch def
    calc [ /add 5 ] gen-call =     % -> 5
    calc [ /mul 3 ] gen-call =     % -> 15
    calc /done gen-stop
} actor-spawn coroutine-join pop
```

Each handler invocation creates fresh lvars, so bindings from previous
calls do not interfere.

#### 7.5.6 Match + Unify

`unify` returns a boolean (`a b -- bool`), making it a natural test proc
for `match`.  The test consumes the dup'd value and produces true/false:

```
/x logic-var def
/y logic-var def
[ /point 3 4 ] [
    { [ /line x y ] unify }
    { pop (line) }
    { [ /point x y ] unify }
    { pop x deref y deref add }
    { pop true }
    { pop (unknown) }
] match =    % -> 7
```

The first arm fails (`/point` != `/line`), so `unify` returns false and
no bindings occur.  The second arm succeeds, binding `x=3` and `y=4`.

---

## 8. Limitations and Future Work

### 8.1 Restore-Barrier Constraint

A result that references *local* VM heap allocated above a save barrier
is rolled back by `restore`, so it cannot cross a backtracking boundary.
This means:

- A scalar or ExtValue result -- integers, bytes, booleans, reals, names,
  or longs/doubles/addresses -- survives restore directly.  `deref` logic
  variables to such values on any path that leads to restore.
- A structured result (array, record, tagged, dict, string) survives
  restore when it is built in **global VM**: wrap its construction in
  `${...}`.  Global VM is journal-skipped, so `restore` does not roll the
  result back.  (The lone exception is a logic-variable *binding cell* in
  global VM, which is journaled so backtracking can undo it -- see §2.2.)
  See §8.3.
- This constraint primarily shapes `find-all` (collecting multiple
  solutions); each solution is captured, then its alternative is restored.

### 8.2 find-first is already `choice`

A dedicated `find-first` operator is unnecessary.  `choice` already
returns the first succeeding alternative and stops:

```
/x logic-var def
[
    { x 1 unify guard  x deref 2 mod 0 eq guard }
    { x 2 unify guard  x deref 2 mod 0 eq guard }
    { x 3 unify guard  x deref 2 mod 0 eq guard }
] choice
x deref =    % -> 2 (first even number)
```

### 8.3 find-all (scalar, ExtValue, and `${...}` heap results)

`find-all` is implemented using save/restore per alternative with
`@find-all-barrier`/`@find-all-fail` control operators.  Each alternative
runs under its own save point; the result is captured, then the save is
restored before the next alternative.

Scalar and ExtValue results -- inline in the Object, or re-allocatable
from raw bits -- survive that restore directly:

```
/x logic-var def
[
    { x 1 unify guard  x deref }
    { x 2 unify guard  x deref 10 gt guard }
    { x 3 unify guard  x deref }
] find-all    % -> [1, 3]
```

Heap-referencing results (Array, Record, Tagged, Dict, String) survive
the restore **when they are built in global VM** -- wrap the result
construction in `${...}`.  Global VM is journal-skipped, so `restore`
does not roll the result back, and `find-all` collects the structured
result (this skip is for *result objects*; an lvar *binding* in global VM
is the one journaled exception -- §2.2):

```
[
    { ${ [ 1 2 ] } }
    { ${ [ 3 4 ] } }
    { ${ [ 5 6 ] } }
] find-all    % -> [[1 2] [3 4] [5 6]]
```

A heap result built in *local* VM still raises `/type-check`
(`"... cannot survive restore; ... wrap the result construction in
${...}"`), because the local VM is rolled back.  The same rule applies
to `find-n` and `aggregate`.  The gate is `Object::is_persistable_now`: a
result is collectable if it is scalar/ExtValue or already lives in global
VM.

### 8.4 Clause database not needed (dict + choice is sufficient)

A Prolog-style clause database (`assert-fact`, `retract-fact`, `query`)
is not needed.  The same patterns are achievable with dicts and choice:

```
% Define rules as a dict of name -> array-of-procs
/rules << >> def

% "assert" a rule: add a proc to the rule's alternatives array
/assert-rule {
    % name proc --
    /proc exch def  /name exch def
    rules name known? {
        rules name get proc append  rules name rot put
    } {
        rules name [ proc ] put
    } if-else
} def

% "query" a rule: run choice over its alternatives
/query-rule {
    % name --
    rules exch get choice
} def

% Usage: build a rule set
/parent { /Y exch def  /X exch def
    [
        { X /tom unify guard    Y /bob unify guard }
        { X /tom unify guard    Y /liz unify guard }
        { X /bob unify guard    Y /ann unify guard }
    ] choice
} def

% Query: who is tom's child?
/child logic-var def
/tom child parent
child deref =     % -> /bob (first match)
```

This approach uses existing infrastructure (dicts for storage, choice
for backtracking) with no new operators needed.  For dynamic rule
sets that grow at runtime, build the alternatives array in a dict and
pass it to `choice`.

### 8.5 copy-term implemented

`copy-term` deep copies a term, replacing unbound lvars with
fresh ones (preserving shared variable identity).  Bound lvars are
dereferenced.  Arrays, tagged values, and records are recursively copied.
See section 3.4 for full documentation.

### 8.6 Occurs check not needed (deref depth limit is sufficient)

Cycle detection during unification (e.g., preventing `x [x] unify`
from creating a circular term) is omitted.  SWI-Prolog also omits it
by default for performance.  The `deref` depth limit (`MaxDerefDepth`
= 64) provides a safety net: if a cycle is accidentally created, deref
terminates rather than looping infinitely.

---

## 9. Implementation Summary

| Item | Value |
| --- | --- |
| File | `src/ops_logic.inl` |
| Lines | ~1815 |
| Standard operators | 20 (logic-var, named-var, lvar-name, is-logic-var, bound?, deref, unify, fail, guard, choice, cut, naf, once, copy-term, find-all, find-n, choice-count, for-each-solution, aggregate, unify-match) |
| Control operators | 20 (10 barrier/fail pairs: @choice-*, @choice-count-*, @find-all-*, @find-n-*, @for-each-solution-*, @aggregate-*, @aggregate-reduce-*, @unify-match-*, @naf-*, @once-*) |
| New error | `Error::Fail` (/fail) |
| New barrier types | `@choice-barrier`, `@find-all-barrier` in try_catch_handler |
| Pre-interned name | `WellKnownName::LVar` (`/lvar`), in the well-known names table |
| SnapShotHeader | unchanged (`/lvar` rides the existing well-known names table) |
| Save stack defaults | `DefaultSaveCount` = 64, `MaxSaveCount` = 255 |
| Test suite | 311 assertions, 182 sections (test_logic.trx) |
| Heap cost per lvar | 24 bytes |
| Heap cost per binding (journaled) | 12 bytes (Save::Entry) |
| Dependencies | None (uses existing save/restore, tagged, array infrastructure) |
