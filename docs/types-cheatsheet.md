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

# Trix Type Cheat Sheet

Every Object is an 8-byte tagged union with a 5-bit type tag and a
type-specific payload.  Trix has **30 user-visible types** (the
`Type` enum has 31 entries; one is internal-only).  The big
operational rules:

  1. **Arithmetic and comparison ops are strict-typed.**  `add`,
     `sub`, `mul`, `div`, `mod`, `lt`, `le`, `gt`, `ge` all raise
     `/type-check` on mixed types.  Use `$[ ... ]` infix or call
     `promote` first.
  2. **`eq` is strict on type AND value.**  `1 1.0 eq` returns
     `false` (no error).  Cross-type comparisons via `eq` always
     return false.
  3. **`$[ ... ]` promoted infix is the auto-widening path.**  Mix
     numeric types freely inside `$[ ... ]`; outside, you must
     match types or use `promote` / `reinterpret` explicitly.
  4. **Type predicate idiom:** `obj type /TYPE-type eq` — there is
     no `integer?` op; the type-name Names are the dispatch keys.

---

## 1. The 31 types

| Tag | Type | Size | Encoding | Notes |
| --- | --- | --- | --- | --- |
| 00000 | `Null` | 0 | Tag only | The unit value; used as "no value" sentinel |
| 00001 | `Byte` | 8 | u8 inline | Single byte; arithmetic checks overflow |
| 00010 | `Integer` | 32 | i32 inline | Default integer literal type |
| 00011 | `UInteger` | 32 | u32 inline |  |
| 00100 | `Long` | 64 | i64 ExtValue |  |
| 00101 | `ULong` | 64 | u64 ExtValue |  |
| 00110 | `Address` | 64 | `address_t` (void*) ExtValue | VM offset (heap pointer) |
| 00111 | `Real` | 32 | f32 inline | Default float literal type |
| 01000 | `Double` | 64 | f64 ExtValue |  |
| 01001 | `Boolean` | 1 | bool inline | `true` / `false` |
| 01010 | `Operator` | 32 | `SystemName` index inline | Built-in op handle |
| 01011 | `Mark` | 0 | Tag only | Stack sentinel for `[`, `<<`, `{{` collectors |
| 01100 | `Name` | 32 | offset to interned `Name` struct | Literal `/foo` or executable `\foo` |
| 01101 | `Array` | 64 | offset + length | Heap-allocated `Object[]` |
| 01110 | `Packed` | 64 | offset + length | Compact bytecode-style proc body |
| 01111 | `String` | 64 | offset + length | Mutable / immutable byte buffer |
| 10000 | `Stream` | 64 | offset to `Stream` struct | File / memory / process stream |
| 10001 | `Dict` | 64 | offset to `Dict` struct | Hash table |
| 10010 | `SourceLoc` | — | — | **Internal** — call-site location; never user-visible |
| 10011 | `Curry` | 64 | offset + arity | Partial application: captured value + callable |
| 10100 | `Thunk` | 64 | offset + state | Lazy evaluation: proc + cached result |
| 10101 | `Set` | 64 | offset to `Set` struct | Dict without values; unique-key collection |
| 10110 | `Tagged` | 64 | offset to `[Name, Object]` | Discriminated value (ADT carrier) |
| 10111 | `Record` | 64 | offset to `[Schema, Object*]` | Immutable named-field composite |
| 11000 | `Coroutine` | 64 | offset to `CoroutineContext` | Stackful coroutine handle |
| 11001 | `PipeBuffer` | 64 | offset to `PipeBufferHeader` | Bounded inter-coroutine pipe |
| 11010 | `Cell` | 64 | offset to `CellHeader` | Reactive cell (FRP) |
| 11011 | `Continuation` | 64 | offset to `ContinuationContext` | One-shot delimited continuation |
| 11100 | `Int128` | 128 | offset to `WideValue` | Signed 128-bit (boxed) |
| 11101 | `UInt128` | 128 | offset to `WideValue` | Unsigned 128-bit (boxed) |
| 11110 | `OpaqueHandle` | 64 | kind + offset | Sub-typed: Screen, future Tilemap, … |

Type-name Names (returned by the `type` op) all have form
`/TYPENAME-type`, lowercase, e.g. `/integer-type`, `/null-type`,
`/uint128-type`, `/opaque-handle-type`.

---

## 2. Numeric type ranks (widening order)

For `$[ ... ]` infix and `promote`, types widen along these rules:

```
Byte  <  UInteger  <  ULong  <  UInt128
                ↓                 ↓
Byte  <  Integer  <  Long   <  Int128  <  Double
                ↓        ↓        ↓
                Real  <  Double
```

  * **Byte (8b)** is below all wider integer types.
  * **Same-width signed-vs-unsigned (32b)**: pair promotes to next
    wider signed (`UInteger + Integer → Long`).
  * **Same-width signed-vs-unsigned (64b / 128b)**: unsigned wins,
    since the wider signed type can no longer hold the unsigned
    range (`Long + ULong → ULong`, `Int128 + UInt128 → UInt128`).
  * **Real (24-bit mantissa)** is enough for 32-bit integer mixes
    but loses precision on 64-bit+ integers; the lattice promotes
    `Long + Real → Double` etc. to preserve precision.

### Promotion table — `$[ A + B ]` infix result types

(Reading: row = LHS, column = RHS.  Cell = result type.)

|  | Byte | Integer | UInteger | Long | ULong | Int128 | UInt128 | Real | Double |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Byte** | Byte | Integer | UInteger | Long | ULong | Int128 | UInt128 | Real | Double |
| **Integer** | Integer | Integer | **Long** | Long | **ULong** | Int128 | **Int128** | Real | Double |
| **UInteger** | UInteger | **Long** | UInteger | Long | ULong | Int128 | UInt128 | Real | Double |
| **Long** | Long | Long | Long | Long | **ULong** | Int128 | **UInt128** | **Double** | Double |
| **ULong** | ULong | **ULong** | ULong | **ULong** | ULong | **Int128** | UInt128 | **Double** | Double |
| **Int128** | Int128 | Int128 | Int128 | Int128 | **Int128** | Int128 | **UInt128** | **Double** | Double |
| **UInt128** | UInt128 | **Int128** | UInt128 | **UInt128** | UInt128 | **UInt128** | UInt128 | **Double** | Double |
| **Real** | Real | Real | Real | **Double** | **Double** | **Double** | **Double** | Real | Double |
| **Double** | Double | Double | Double | Double | Double | Double | Double | Double | Double |

Bold cells are the non-obvious results.  Note the asymmetry: when
**Integer (32-bit signed) meets UInt128**, the result is **Int128**
(signed wins at the wider rank, because Integer's narrow range fits
comfortably in Int128's sign-bit budget); but when **Long meets
UInt128**, the result is **UInt128** (the gap is small enough that
unsigned semantics make more practical sense).  Consult the table
in doubt — these rules are designed for sensible defaults, not for
strict derivation from a single principle.

`Address` does not participate in arithmetic and is omitted.

Division `$[ A / B ]` follows the same lattice but is type-preserving
when both sides are integer (`$[ 7 / 2 ] → 3`, Integer).  Use `$[ 7.0
/ 2 ]` if you want Real division.

---

## 3. Strict-typed ops (no auto-promotion)

These all raise `/type-check` when given mixed numeric types:

| Op family  | Members                                               |
| ---------- | ----------------------------------------------------- |
| Arithmetic | `add sub mul div mod neg abs min max`                 |
| Comparison | `lt le gt ge`                                         |
| Bitwise    | `and or xor not shift-left shift-right`               |
| Math       | `floor ceil round sqrt` (single-arg, type-preserving) |

Bare-form examples:

```trix
{ 1 1.0 add } try        % /type-check
{ 1b 1i add } try        % /type-check  (Byte vs Integer)
{ 1l 1ul add } try       % /type-check  (Long vs ULong)
1 2 add =                % 3      (Integer + Integer)
1.0 2.0 add =            % 3.0    (Real + Real)
```

To mix types, either use `$[ ... ]` infix:

```trix
$[ 1 + 1.0 ] =           % 2.0  (Real)
$[ 1b + 1l ] =           % 2    (Long)
```

Or call `promote` explicitly:

```trix
1 1.0 promote add =      % 2.0
% promote: num1 num2 :- num1' num2'  -- widens both to common type
```

---

## 4. `eq` semantics — strict type+value

`eq` does NOT auto-promote.  Cross-type comparisons always return
`false`; same-type comparisons compare values:

```trix
1   1   eq =     % true    (same type, same value)
1   1.0 eq =     % false   (Integer ≠ Real even with same value)
1   1l  eq =     % false   (Integer ≠ Long)
1   1u  eq =     % false   (Integer ≠ UInteger)
1.0 1.0d eq =    % false   (Real ≠ Double)
1   true eq =    % false   (Integer ≠ Boolean)
1   (1) eq =     % false   (Integer ≠ String)
null null eq =   % true
```

When you genuinely need cross-type equality, promote first:

```trix
1 1.0 promote eq =       % true
```

`ne` is the inverse.  Deep structural equality (recursive into
arrays/dicts/etc.) is `deep-eq` — same value-with-type rules per leaf
but recurses into containers.

---

## 5. Type predicate idiom

For a single type test, use the **`is-TYPE` prefix predicates**
(`is-integer`, `is-string`, `is-number`, `is-signed`, `is-unsigned`,
etc. -- the full list is in
[`operator-cheatsheet.md`](operator-cheatsheet.md) § 3.6).  There is
**no `integer?` / `real?` (suffix-`?`) op family**.

```trix
42 is-integer =                % true
42u is-unsigned =              % true
```

When you need to dispatch on the type-name itself (e.g. drive a
lookup dict), use the `type` op + `eq`:

```trix
% Check if top of stack is an Integer
42  type /integer-type eq    % true

% Branch on type
{ |x|
    x type
    /integer-type eq      { x 2 mul }
                          { x 0 eq if-else }                  % unused else
    if-else
} def
```

For "any numeric type" checks, use `is-number` directly:

```trix
/numeric? { is-number } def
42  numeric? =                 % true
(x) numeric? =                 % false
```

To dispatch on the type-name itself (when you want different code per
type, not just a boolean), look the name up in a dict with `type get`.
The dict is pushed first, then `exch type` brings the type-name on top
for `get`:

```trix
/label-of {
    <<
        /integer-type  (an integer)
        /real-type     (a real)
        /string-type   (a string)
    >> exch type get
} def
42  label-of =                 % an integer
3.5 label-of =                 % a real
```

(`get` raises `/undefined` on an unlisted type-name; add a
`/default` entry and a `known?` guard if you need a fallback.)

The 30 user-visible type-name Names are listed in § 1.  All have the
`-type` suffix.

---

## 6. Conversion ops

Trix's conversion vocabulary is intentionally narrow:

| Op | Direction | Stack effect | Notes |
| --- | --- | --- | --- |
| `promote` | numeric A, numeric B → both widened | `num1 num2 -- num1' num2'` | Applies the § 2 lattice |
| `promote` | numeric array → widened array | `arr -- arr'` | All elements widened to the common type; errors on non-numeric |
| `to-number` | string → number | `str -- num` | Parses the literal; raises `/type-check` on bad input.  Result type follows the literal's suffix (`123` → Integer, `123l` → Long, `3.14` → Real) |
| `to-string` | any → string | `any rwstr -- str` | Caller pre-allocates the buffer (`N string`); raises `/range-check` if too small |
| `to-name` | string → name | `str -- name` | Interns at the current `m_curr_alloc_global` |
| `reinterpret` | num + type-name → num | `num /type-name -- num'` | **Bit-cast**, NOT value-preserving (`3.7 /integer-type reinterpret` returns the IEEE-754 bit pattern interpreted as Integer) |
| `cast` | num + type-name → num | `num /type-name -- num'` | Value-preserving conversion (Float→Int truncates toward zero; signed/unsigned reinterprets bits; any→Boolean is nonzero-test).  Scalar only; use `coerce` for containers |
| `coerce` | container + type-name → container | `arr/pk/dict /type-name -- container'` | Widens all numeric elements/values to the named type; narrowing raises `/type-check` |
| `floor` `ceil` `round` `trunc` | numeric → numeric | `num -- num'` | Float → rounded float (still Real / Double); does NOT convert to Integer |
| `make-literal` `make-executable` | any → any | `any -- any'` | Toggle Literal / Executable attribute — does not change type |

### Common conversion recipes

```trix
% Float to Integer (value-preserving, truncates toward zero)
3.7 /integer-type cast                   % => 3 (truncated toward zero)

% Integer to Real
$[ 42 + 0.0 ] =                          % 42.0 (Real)

% Integer to Long (widening)
$[ 42 + 0l ] =                           % 42 as Long

% String to typed number
(123) to-number type =                    % integer-type
(123l) to-number type =                   % long-type
(3.14) to-number type =                   % real-type
(0xFF) to-number =                        % 255 (Integer)
```

---

## 7. Numeric literal suffixes (recap)

| Suffix               | Type                 | Example |
| -------------------- | -------------------- | ------- |
| *(none, int-shape)*  | `Integer`            | `42`    |
| *(none, real-shape)* | `Real`               | `3.14`  |
| `i`                  | `Integer` (explicit) | `42i`   |
| `u`                  | `UInteger`           | `42u`   |
| `l`                  | `Long`               | `42l`   |
| `ul`                 | `ULong`              | `42ul`  |
| `a`                  | `Address`            | `1000a` |
| `b`                  | `Byte`               | `255b`  |
| `r`                  | `Real` (explicit)    | `3.14r` |
| `d`                  | `Double`             | `3.14d` |
| `q`                  | `Int128`             | `42q`   |
| `uq`                 | `UInt128`            | `42uq`  |

Radix-prefixed integer: `0x10` / `0o20` / `0b10000` / `16#10#` —
all evaluate to Integer 16 (use type suffix to override:
`0xFF#ul` for ULong — the `#` separator is required for 0x/0o/0b prefixes).

---

## 8. Quick reference — what's where

  * **Predicate-style ops** that exist (`xxx?` form): `known?`,
    `executable?`, `readable?`, `bit?`, etc. — these test specific
    properties, not type membership.  For type membership use the
    `is-TYPE` predicates or `type` + `eq` (§ 5).
  * **Where the lattice lives in source:** `src/ops_convert.inl` (function `promote_common_type`; `ops_math.inl` only calls it) —
    each binary-op's promote callback derives the result type via
    a fixed table.
  * **Where types are defined:** `src/object.inl` ~ line 300, the
    `Type` enum.
  * **Type-system narrative + design rationale:**
    [`type-system.md`](type-system.md).

---

## Where to go next

  * **Per-op error conditions** that the strict-type rules raise:
    [`errors-cheatsheet.md`](errors-cheatsheet.md) § "Type / value errors".
  * **Format-string spec for printing typed values:**
    [`format-cheatsheet.md`](format-cheatsheet.md) § 3.
  * **All scanner sigils for literals:**
    [`scanner-syntax.md`](scanner-syntax.md) § 0 or
    [`trix-reference.md`](trix-reference.md) § 2.
