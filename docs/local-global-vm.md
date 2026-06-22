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

# Local and Global VM: Values, Containers, and `${...}`

Trix has two heaps. The **local VM** is a bump-allocated arena that
`restore` rolls back to a save barrier; the **global VM** is a
mark-swept region that survives `restore`. Most of the time you never
think about this -- you `save`, allocate scratch, and `restore` to
reclaim it. But the moment you store one piece of data *into* another
that lives in the other region, the asymmetry between the two heaps
becomes visible, and a few simple rules apply.

This guide gives you those rules in one place. The headline is a single
distinction -- **values** versus **containers** -- that tells you, for
any store, whether it Just Works or whether you need to build the thing
inside a `${...}` block. Everything else is a corollary.

For the transaction mechanism itself see
[`save-restore.md`](save-restore.md); for the global allocator and its
GC see [`gvm-heap-gc.md`](gvm-heap-gc.md); for how this constraint
shapes logic programming see [`logic.md`](logic.md). Per-operator
semantics live in [`trix-reference.md`](trix-reference.md).

---

## Table of Contents

1. [The Root Cause](#1-the-root-cause)
2. [The User Model: Values vs Containers](#2-the-user-model-values-vs-containers)
3. [The Store Table](#3-the-store-table)
4. [Region-Aware Operator Results](#4-region-aware-operator-results)
5. [What This Unlocked: find-all Over Heap Results](#5-what-this-unlocked-find-all-over-heap-results)
6. [Special Cases](#6-special-cases)
7. [Garbage Collection Reaches Global Values Inside Local Containers](#7-garbage-collection-reaches-global-values-inside-local-containers)
8. [Worked Examples](#8-worked-examples)

---

## 1. The Root Cause

One fact drives every rule in this document:

> **Local VM is restore-reclaimed and journaled. Global VM survives
> restore and is journal-skipped.**

When you call `save`, Trix records a barrier. A later `restore` rolls
the local VM pointer back to that barrier in O(1) -- everything
allocated above it is gone -- and replays the journal to undo recorded
mutations (array writes, dict entries, name bindings). Global
allocations are different: the journal *skips* the global VM
entirely, so global data is not rolled back. It is reclaimed only by
the global mark-sweep GC, on its own schedule.

That asymmetry is the whole story. A pointer into local-above-the-barrier
memory dangles after `restore`; a pointer into global memory does not.
The rules below exist to keep you from creating the first kind of
pointer where it would outlive its target.

---

## 2. The User Model: Values vs Containers

You do not need to think in terms of "heap versus immediate" or "which
types use the VM heap." Think in terms of two kinds of things:

**VALUES** -- numbers and other scalars. Integer, UInteger, **Long**, **Int128**,
ULong, UInt128, Double, Address, Boolean, Byte, and Real. A value just
works. Storing a value into a global container at any save level Just
Works; the engine copies the value into the container's region as
needed. You never have to think about whether `Long` or `Int128`
"uses the VM heap" -- that is an implementation detail, not something
the model asks you to reason about. A value has no identity to preserve;
two copies of `42l` are interchangeable.

**CONTAINERS and strings** -- Array, Dict, Set, String, Tagged, Record,
Proc, and the like. A container has **identity and region**. To store a
container into a *global* container across a save scope, the inner
container must itself live in global VM. You build it there with a
`${...}` block. A container built in *local* VM, above a save barrier,
that you try to store into a global container is **rejected** with
`/invalid-access` ("cannot store restore-fragile local-VM `<type>` into
global container") -- because after `restore` reclaimed the local VM,
the global container would hold a dangling reference.

**Names** sit between the two. A name *first interned below the active
save barrier* -- every built-in, and any name you used before the `save`
-- behaves like a value: storing it just works. But a name *first
interned above the barrier* -- a brand-new key such as `/k` that appears
for the first time after the `save` -- is allocated in local VM and is
restore-fragile like a container, so it must be interned global (with
`${ /k }` or the `$/k` directive) before it goes into a global container.
In practice this only surfaces for a fresh key name; see
[§3](#3-the-store-table) and [§8](#8-worked-examples).

So the rule you actually learn is one sentence:

> **Values clone freely; containers must be built in the region you
> store them into.**

The good news is that operator *results* are already region-aware
(see [§4](#4-region-aware-operator-results)): an array, dict, set, or
string produced by an operator inside `${...}` lands in global VM
automatically. You only hit the rejection when you *hand-build* a local
container and then try to store it into a global one. The fix is always
the same: move the construction inside `${...}`.

The reverse direction is always safe. A **local** container may freely
hold **global** values and **global** containers, because global data
outlives the local VM -- there is nothing to dangle.

---

## 3. The Store Table

This is the value-vs-container rule made exhaustive. Read it as: *I am
storing V into container C.* C is either local or global; if global, the
store may go through an ordinary write or through the `-persist` family
(see [§6](#6-special-cases)). Every cell below was verified against the
current binary.

|  | V is a **value** (number/atom) | V is a **container/string** built local, above the barrier | V is a **container/string** built in `${}` (global) or below the barrier |
| --- | --- | --- | --- |
| **C is local** | OK | OK (journaled; undone on `restore`) | OK |
| **C is global** | OK (engine copies the value in) | **REJECTED** -- build it in `${}` | OK |
| **C is global, via `-persist`** | OK if below barrier / global; else `/above-barrier` | `/above-barrier` | OK |

A few notes that the table compresses:

- "Built local, above the barrier" means: allocated in the local VM at a
  save level of 1 or higher. At save level 0 (no barrier active) a
  local container is restore-immune, so there is nothing to reject; the
  rejection only appears once a `save` is in effect.
- The rejection message names the offending *type*. If your store
  involves a **name** or **string key** as well as a value (for example
  `dict /k value put`), remember that the key `/k`, if it is a fresh
  local name above the barrier, is itself a container and must also be
  global. Build the key and value together: `dict ${ /k value } put`,
  or use a value key (an integer), or a global-name directive (`$/k`).
  See [§8](#8-worked-examples) for the worked version.
- The `-persist` row is **stricter** than ordinary stores: it rejects
  *any* above-barrier operand -- even a plain number -- with
  `/above-barrier`. That is a different contract; see
  [§6](#6-special-cases).

---

## 4. Region-Aware Operator Results

You rarely build containers by hand. You build them with operators --
`concat`, `]` (array close), `>>` (dict close), `map`, `set-union`, and
so on. **Inside a `${...}` block, the *result* of an operator is
allocated in global VM.** That is what makes the model practical: you
do not relocate anything yourself; you just wrap the construction.

```trix
save /sv exch def
${ (a)(b) concat }
{ sv restore } try
(operator result inside dollar-brace survives restore) exch /no-error eq assert
(survives on the stack intact) exch (ab) eq assert
(ok) =
```

The `concat` result is global, so it survives `restore` sitting on the
operand stack -- `restore` returns `/no-error` and the string `ab` is
still there.

### The literal gotcha

`${...}` makes operator *results* global. It does **not** relocate the
*literals* scanned inside it. A bare string literal is scanned and
allocated in **local** VM even inside `${...}`, so if you leave it on
the stack across `restore` it still raises `/invalid-restore`:

```trix
save /sv exch def
${ (hello) }
{ sv restore } try
(string literal inside dollar-brace stays local) exch /invalid-restore eq assert
pop
sv restore
(ok) =
```

Contrast `${ (a)(b) concat }` above, whose `concat` *result* is global
and survives. An array literal of inline scalars such as `${ [1 2 3] }`
*is* built by the `]` operator, so it is global and survives. But an
array literal whose elements are *string literals* -- `${ [ (a) (b) ] }`
-- raises `/invalid-access` at scan time once a save barrier is active,
because each local string literal cannot be stored into the global
array the `]` operator is building. (At save level 0, with no barrier,
the same form simply builds.)

This is the same wording used in
[`save-restore.md`](save-restore.md#2-quick-reference), where the stack
rules during `restore` are spelled out in full.

---

## 5. What This Unlocked: find-all Over Heap Results

Region-aware results are what let `find-all`, `find-n`, and `aggregate`
collect *structured* solutions. Each alternative runs under its own
save point; the result is captured, then the save is restored before
the next alternative is tried. A scalar survives that restore directly.
A heap-referencing result (Array, Record, Tagged, Dict, String) survives
**only when it is built in global VM** -- so you wrap the result
construction in `${...}`:

```trix
[
    { ${ [ 1 2 ] } }
    { ${ [ 3 4 ] } }
    { ${ [ 5 6 ] } }
] find-all
/r exch def
(find-all collects three heap arrays) r length 3 eq assert
(first collected array is [1 2]) r 0 get 1 get 2 eq assert
(last collected array is [5 6]) r 2 get 1 get 6 eq assert
(ok) =
```

A heap result built in *local* VM instead raises `/type-check` ("...
cannot survive restore; ... wrap the result construction in `${...}`"),
because the local VM is rolled back between alternatives. The same
rule applies to `find-n` and `aggregate`. This was once described as the
single most significant limitation of the logic layer; making operator
results region-aware removed it. See
[`logic.md` §8.3](logic.md#83-find-all-scalar-extvalue-and--heap-results)
for the full treatment.

---

## 6. Special Cases

A handful of subsystems specialize the value-vs-container rule. They all
follow from the same root cause; each is called out here so you are not
surprised.

### The `-persist` family

The `-persist` operators -- `put-persist`, `update-persist`,
`set-add-persist`, `array-store-persist`, and `def`'s persist forms --
create entries whose header mutations are **not journaled**. Because a
journal entry is exactly what would undo a rolled-back mutation, persist
imposes a **stricter** contract: it rejects *any* above-barrier operand,
including a plain number, with `/above-barrier`. Persist needs
below-barrier or global operands:

```trix
/gc ${ << >> } def
save /sv exch def
(persist rejects an above-barrier operand) { gc /a 1 put-persist } try /above-barrier eq assert
(ok) =
```

Below the barrier (or with global operands), persist is fine, and you
may persist a global container into a global dict:

```trix
/gc ${ << >> } def
gc /a ${ [ 1 2 ] } put-persist
(persisted a global container below the barrier) gc /a get 1 get 2 eq assert
(ok) =
```

This is a *distinct* contract from the ordinary store rejection in
[§3](#3-the-store-table) -- different error name (`/above-barrier` vs
`/invalid-access`), different trigger (any above-barrier operand vs only
a fragile local container).

### Reactive cells

`cell-set` follows the value-vs-container rule with one convenience: a
cell **accepts a fragile value by cloning it into global**, rather than
rejecting. So storing a `Long` into a cell across a save scope works and
survives `restore`:

```trix
/c ${ 0 cell } def
save /sv exch def
42l c cell-set
(the cell cloned the fragile value into global) c cell-get 42l eq assert
sv restore
(and it survives restore) c cell-get 42l eq assert
(ok) =
```

### `eqref` containers

Root `eqref` containers (the eq-array / eq-dict / eq-set forms, built
with the `#=` directive family) bypass journaling by design, so -- like
`-persist` -- they reject above-barrier values. Store global or
below-barrier operands into them.

### Logic variables created in `${...}`

A logic variable created inside `${...}` lives in global VM, so it
survives `restore` (that is intentional -- a logic variable must outlive
the per-alternative save points that drive backtracking). Its
*bindings*, however, must still be undone when an alternative fails.
Trix journals those bindings specially so that backtracking still rolls
them back -- the one place a global slot is deliberately journaled.
You do not have to do anything; scalar `find-all` over a global logic
variable backtracks correctly:

```trix
/x logic-var def
[
    { x 1 unify guard  x deref }
    { x 2 unify guard  x deref 10 gt guard }
    { x 3 unify guard  x deref }
] find-all
/r exch def
(bindings of a global logic var still backtrack) r length 2 eq assert
(ok) =
```

(The middle alternative fails its guard, so only `1` and `3` are
collected -- proving each binding was undone before the next
alternative.)

---

## 7. Garbage Collection Reaches Global Values Inside Local Containers

A global value or container held *inside* a local container is reachable
by the global GC. During its mark phase the GC descends into local
containers (Array, Packed, Dict, Set, Cell, Tagged, Record, Curry,
Thunk, and Continuation contexts) and follows any global references they
hold. So a global dict stored into a local array is not collected while
the local array is alive:

```trix
/lc [ 0 ] def
lc 0 ${ << /a 123 >> } put
vm-global-gc
(global dict inside a local array survives GC) lc 0 get /a get 123 eq assert
(ok) =
```

You never have to root these references manually. For the allocator and
GC internals -- mark roots, sweep, growth direction -- see
[`gvm-heap-gc.md`](gvm-heap-gc.md).

---

## 8. Worked Examples

These tie the rules together. Each runs cleanly on the current binary.

### A value clones into a global container

A `Long` is a value, so storing it into a global dict works at any save
level and survives `restore`:

```trix
/d ${ << >> } def
save /sv exch def
d 1l 2l put
(long value clones into the global dict) d 1l get 2l eq assert
sv restore
(and survives restore) d 1l get 2l eq assert
(ok) =
```

### A local container is rejected; build it in `${...}`

Storing a *locally built* array into the global dict, above a barrier,
is rejected:

```trix
/d ${ << >> } def
save /sv exch def
(local array into global dict is rejected) { d /k [ 1 2 3 ] put } try /invalid-access eq assert
(ok) =
```

The fix is to build the array in `${...}`. Note that the **key** must be
global too: a fresh local name `/k` above the barrier is itself a
container. Build key and value together inside one `${...}`:

```trix
/d ${ << >> } def
save /sv exch def
d ${ /k [ 1 2 3 ] } put
(global key and array stored into global dict) d /k get 1 get 2 eq assert
sv restore
(and survives restore) d /k get 1 get 2 eq assert
(ok) =
```

Equivalent fixes: use a value key (`d 7 ${ [ 1 2 3 ] } put`) or a
global-name directive for the key (`d $/k ${ [ 1 2 3 ] } put`).

### A local container freely holds global data

The reverse direction needs no ceremony -- a local container may hold a
global container:

```trix
save /sv exch def
/lc [ 0 0 ] def
lc 0 ${ << /a 9 >> } put
(local array holds a global dict) lc 0 get /a get 9 eq assert
(ok) =
```

### Element writes on a local container are journaled

Writing into a *local* container at a save level journals the element,
so `restore` undoes it:

```trix
/lc [ 0 0 ] def
save /sv exch def
lc 0 [ 1 2 ] put
(element write took effect) lc 0 get 1 get 2 eq assert
sv restore
(restore undid the element write) lc 0 get 0 eq assert
(ok) =
```
