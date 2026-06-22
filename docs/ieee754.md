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

# Trix IEEE 754 Floating-Point Support

Trix provides IEEE 754 floating-point support with 99+ operators, full FP
environment control, quiet comparisons, NaN payloads, ULP arithmetic,
compensated summation, and the complete C++17/C++23 special functions library.

**Prototyping ahead of C/C++.** Because the whole environment is exposed
interactively, the REPL doubles as a numerical workbench.  Trix's `Real` and
`Double` *are* C's `float` and `double`, and the operators here are thin wrappers
over the same standard-library math (`<cmath>`, `<cfenv>`) you would call from a
host program -- so a routine you prototype and validate in the REPL, with the
rounding mode, special functions, and ULP behavior you intend to ship, ports to
C or C++ with its numerical behavior intact.  Sketch the algorithm in Trix, check
it against known values, then transcribe it once you are confident.

This document covers the full IEEE 754 feature set: types, operators, FP
environment, special functions, error handling, and design rationale.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [Types and Literals](#3-types-and-literals)
4. [Tutorial](#4-tutorial)
5. [Classification and Comparison](#5-classification-and-comparison)
6. [Rounding](#6-rounding)
7. [The Floating-Point Environment](#7-the-floating-point-environment)
8. [Elementary Functions](#8-elementary-functions)
9. [Special Mathematical Functions](#9-special-mathematical-functions)
10. [Advanced IEEE 754](#10-advanced-ieee-754)
11. [Error Handling for Floating-Point](#11-error-handling-for-floating-point)
12. [Design Decisions](#12-design-decisions)

---

## 1. Overview

Trix exposes both IEEE 754 single-precision (32-bit `Real`) and
double-precision (64-bit `Double`) types with full control over the
floating-point environment. The feature set spans four tiers:

| Tier | Coverage | Operator Count |
| --- | --- | --- |
| **Core IEEE 754** | Arithmetic, comparison, rounding, classification, special values | ~45 |
| **Elementary functions** | Trig, hyperbolic, exponential, logarithmic, power | ~28 |
| **C++17 special functions** | Bessel, elliptic integrals, polynomials, zeta, error functions | ~22 |
| **Advanced IEEE 754** | ULP, compensated summation, NaN payloads, FP environment | ~15 |

**Key capabilities:**

- **Dual precision.** Both 32-bit Real and 64-bit Double are first-class types
  with independent literals, constants, and type-preserving operations.
- **Full FP environment access.** Save/restore rounding modes, query/clear/raise
  exception flags, atomic hold/update -- the complete `<cfenv>` API.
- **IEEE 754-2019 operations.** `total-order?`, `total-order-mag?`,
  `round-even` (roundTiesToEven), quiet comparisons (`greater?`,
  `less?`, `unordered?`).
- **NaN payloads.** Create and extract custom payloads from quiet NaN values
  for diagnostic tagging.
- **ULP arithmetic.** `ulp` (unit in last place), `ulp-equal?` (compare within
  N ULPs) for principled floating-point comparison.
- **Compensated algorithms.** `kahan-sum` and `dot-product` use Kahan
  compensation to maintain precision across large arrays.
- **C++17 special functions.** Bessel functions (cylindrical and spherical),
  elliptic integrals (complete and incomplete), orthogonal polynomials
  (Hermite, Laguerre, Legendre), Riemann zeta, exponential integral, and more.
- **No implicit coercion.** `1.0 + 1` is a type error, not a silent
  conversion. Use `promote` for explicit widening.

---

## 2. Quick Reference

### Basic Arithmetic

| Operator    | Stack Effect      | Description                                   |
| ----------- | ----------------- | --------------------------------------------- |
| `add`       | `num num -- num`  | Addition                                      |
| `sub`       | `num num -- num`  | Subtraction                                   |
| `mul`       | `num num -- num`  | Multiplication                                |
| `div`       | `num num -- num`  | Division                                      |
| `fmod`      | `fp fp -- fp`     | Floating-point remainder (C `fmod`)           |
| `remainder` | `fp fp -- fp`     | IEEE remainder (round-to-nearest quotient)    |
| `rem-quo`   | `fp fp -- fp int` | IEEE remainder and partial quotient           |
| `fdim`      | `fp fp -- fp`     | Positive difference: max(x-y, 0)              |
| `abs`       | `num -- num`      | Absolute value                                |
| `neg`       | `num -- num`      | Negation                                      |
| `fma`       | `fp fp fp -- fp`  | Fused multiply-add: x*y + z (single rounding) |

### Comparison

| Operator           | Stack Effect      | Description                                      |
| ------------------ | ----------------- | ------------------------------------------------ |
| `eq`               | `any any -- bool` | Equal (NaN != NaN)                               |
| `ne`               | `any any -- bool` | Not equal                                        |
| `lt`               | `num num -- bool` | Less than (NaN returns false)                    |
| `le`               | `num num -- bool` | Less or equal (NaN returns false)                |
| `gt`               | `num num -- bool` | Greater than (NaN returns false)                 |
| `ge`               | `num num -- bool` | Greater or equal (NaN returns false)             |
| `greater?`         | `fp fp -- bool`   | Quiet > (NaN returns false)                      |
| `greater-equal?`   | `fp fp -- bool`   | Quiet >= (NaN returns false)                     |
| `less?`            | `fp fp -- bool`   | Quiet < (NaN returns false)                      |
| `less-equal?`      | `fp fp -- bool`   | Quiet <= (NaN returns false)                     |
| `less-greater?`    | `fp fp -- bool`   | Quiet <> (NaN returns false)                     |
| `unordered?`       | `fp fp -- bool`   | True if either operand is NaN                    |
| `total-order?`     | `fp fp -- bool`   | IEEE 754-2019 totalOrder (x <= y in total order) |
| `total-order-mag?` | `fp fp -- bool`   | IEEE 754-2019 totalOrderMag (abs(x) <= abs(y))   |

### Rounding

| Operator     | Stack Effect | Description                                            |
| ------------ | ------------ | ------------------------------------------------------ |
| `ceil`       | `fp -- fp`   | Round toward +infinity                                 |
| `floor`      | `fp -- fp`   | Round toward -infinity                                 |
| `round`      | `fp -- fp`   | Round half away from zero                              |
| `trunc`      | `fp -- fp`   | Round toward zero                                      |
| `nearby-int` | `fp -- fp`   | Round to nearest, ties to even (current rounding mode) |
| `rint`       | `fp -- fp`   | Round to nearest (may raise FE_INEXACT)                |
| `round-even` | `fp -- fp`   | IEEE 754-2019 roundTiesToEven (always ties to even)    |

### Classification

| Operator | Stack Effect | Description |
| --- | --- | --- |
| `fp-classify` | `fp -- name` | FP classification: `/fp-normal`, `/fp-subnormal`, `/fp-zero`, `/fp-infinite`, `/fp-nan` |
| `nan?` | `fp -- bool` | True if NaN |
| `inf?` | `fp -- bool` | True if +infinity or -infinity |
| `finite?` | `fp -- bool` | True if finite (not NaN, not infinity) |
| `normal?` | `fp -- bool` | True if normal (not zero, subnormal, infinite, or NaN) |
| `sign-bit` | `fp -- bool` | True if sign bit set (detects -0.0) |

### FP Manipulation

| Operator      | Stack Effect   | Description                                                    |
| ------------- | -------------- | -------------------------------------------------------------- |
| `frexp`       | `fp -- fp int` | Decompose into fraction [0.5, 1.0) and exponent                |
| `ldexp`       | `fp int -- fp` | x * 2^exp                                                      |
| `scalbn`      | `fp int -- fp` | x * radix^exp                                                  |
| `modf`        | `fp -- fp fp`  | Separate integral and fractional parts                         |
| `logb`        | `fp -- fp`     | Extract unbiased exponent as float                             |
| `ilogb`       | `fp -- int`    | Extract unbiased exponent as integer                           |
| `copy-sign`   | `fp fp -- fp`  | Copy sign of second to first                                   |
| `next-after`  | `fp fp -- fp`  | Next representable value toward second                         |
| `next-toward` | `fp fp -- fp`  | Next representable value toward second (long double direction) |
| `midpoint`    | `fp fp -- fp`  | Midpoint of two values                                         |

### Min/Max

| Operator   | Stack Effect  | Description                               |
| ---------- | ------------- | ----------------------------------------- |
| `fmin`     | `fp fp -- fp` | Minimum (NaN-aware: ignores NaN operands) |
| `fmax`     | `fp fp -- fp` | Maximum (NaN-aware: ignores NaN operands) |
| `fmin-mag` | `fp fp -- fp` | Minimum by magnitude                      |
| `fmax-mag` | `fp fp -- fp` | Maximum by magnitude                      |

### Type Conversion

| Operator            | Stack Effect          | Description                       |
| ------------------- | --------------------- | --------------------------------- |
| `promote`           | `num num -- num num`  | Widen both to common type         |
| `promote`           | `array -- array`      | Widen all elements to common type |
| `cast`              | `num typename -- num` | Convert to specific type          |
| `/real-type cast`   | `num -- real`         | Convert to Real (via `cast`)      |
| `/double-type cast` | `num -- double`       | Convert to Double (via `cast`)    |

### NaN Payload

| Operator           | Stack Effect       | Description                                               |
| ------------------ | ------------------ | --------------------------------------------------------- |
| `nan-payload`      | `fp -- uint/ulong` | Extract NaN payload (UInteger for Real, ULong for Double) |
| `nan-with-payload` | `uint/ulong -- fp` | Create quiet NaN with custom payload                      |

### ULP and Compensated Arithmetic

| Operator      | Stack Effect            | Description                   |
| ------------- | ----------------------- | ----------------------------- |
| `ulp`         | `fp -- fp`              | Unit in last place            |
| `ulp-equal?`  | `fp fp int -- bool`     | True if within N ULPs         |
| `kahan-sum`   | `array -- double`       | Kahan compensated summation   |
| `dot-product` | `array array -- double` | Kahan compensated dot product |
| `product`     | `array -- num`          | Product of all elements       |

### FP Environment

| Operator          | Stack Effect   | Description                                   |
| ----------------- | -------------- | --------------------------------------------- |
| `fe-get-round`    | `-- name`      | Get current rounding mode                     |
| `fe-set-round`    | `name -- bool` | Set rounding mode, returns success            |
| `fe-test-except`  | `uint -- uint` | Test which exception flags are set            |
| `fe-clear-except` | `uint -- bool` | Clear exception flags                         |
| `fe-raise-except` | `uint -- bool` | Raise exception flags                         |
| `fe-get-env`      | `-- ulong`     | Save full FP environment (packed)             |
| `fe-set-env`      | `ulong --`     | Restore full FP environment                   |
| `fe-hold-except`  | `-- ulong`     | Save environment and clear exceptions         |
| `fe-update-env`   | `ulong --`     | Restore environment, merge current exceptions |

---

## 3. Types and Literals

### Two Floating-Point Types

| Type       | Precision       | Size    | Storage             | Suffix        | IEEE 754 |
| ---------- | --------------- | ------- | ------------------- | ------------- | -------- |
| **Real**   | Single (32-bit) | 4 bytes | Inline in Object    | (none) or `r` | binary32 |
| **Double** | Double (64-bit) | 8 bytes | ExtValue in VM heap | `d`           | binary64 |

Real values are stored inline within the 8-byte Object slot -- no heap
allocation required. Double values require an ExtValue allocation (8 bytes)
on the VM heap, managed automatically by save/restore and garbage collection.

### Literal Syntax

```
% Real (32-bit single precision) -- default for decimal literals
1.0                     % Real
-3.14                   % Real
1.5e-10                 % Real (scientific notation)
1.0r                    % Real (explicit suffix)

% Double (64-bit double precision)
1.0d                    % Double
-3.14d                  % Double
1.5e-10d                % Double (scientific notation)

% Hex float literals -- exact IEEE 754 bit patterns, no decimal rounding
0x1.921FB6p1            % Real: pi (exact 32-bit representation)
0x1.921FB54442D18p1#d   % Double: pi (exact 64-bit representation)
0x1.Fp4                 % Real: 31.0 (1.9375 * 2^4)
0xFFp0                  % Real: 255.0 (no fractional part)
0x1.0p-1                % Real: 0.5 (1.0 * 2^-1)
0x1.0p10#d              % Double: 1024.0 (suffix requires '#')

% Special values (top-level systemdict names)
inf   % Real positive infinity
nan   % Real quiet NaN
inf#d % Double positive infinity
nan#d % Double quiet NaN
```

### Special Value Access

Special values and numeric limits are available through dictionary paths:

```
% Real constants
//:systemdict:numbers:real-type:pi          % 3.1415927
//:systemdict:numbers:real-type:e           % 2.7182817
//:systemdict:numbers:real-type:infinity    % +inf
//:systemdict:numbers:real-type:quiet-NaN   % NaN
//:systemdict:numbers:real-type:denorm-min  % smallest subnormal (~1.4e-45)
//:systemdict:numbers:real-type:epsilon     % machine epsilon (~1.19e-7)
//:systemdict:numbers:real-type:max         % largest finite (~3.4e38)
//:systemdict:numbers:real-type:min         % smallest normal (~1.18e-38)
//:systemdict:numbers:real-type:lowest      % most negative (~-3.4e38)

% Double constants (same structure, higher precision)
//:systemdict:numbers:double-type:pi        % 3.141592653589793
//:systemdict:numbers:double-type:epsilon   % ~2.22e-16
//:systemdict:numbers:double-type:max       % ~1.8e308
```

### The numbers Dict Tree

The `systemdict:numbers` dict contains the full IEEE 754 numeric
environment organized as a tree:

```
numbers/
    classification/
        fp-normal           % Name: /fp-normal
        fp-subnormal        % Name: /fp-subnormal
        fp-zero             % Name: /fp-zero
        fp-infinite         % Name: /fp-infinite
        fp-nan              % Name: /fp-nan
        fp-other            % Name: /fp-other
    environment/
        fe-to-nearest       % Name (rounding mode)
        fe-downward         % Name (rounding mode)
        fe-upward           % Name (rounding mode)
        fe-toward-zero      % Name (rounding mode)
        fe-default          % Name (default rounding mode)
        fe-other            % Name (unknown rounding mode)
        fe-invalid          % UInteger (exception flag bitmask)
        fe-div-by-zero      % UInteger (exception flag bitmask)
        fe-overflow         % UInteger (exception flag bitmask)
        fe-underflow        % UInteger (exception flag bitmask)
        fe-inexact          % UInteger (exception flag bitmask)
        fe-all-except       % UInteger (all flags combined)
    real-type/              % 33 entries: limits + math constants
        min, max, lowest, epsilon, infinity, quiet-NaN,
        signaling-NaN, denorm-min, round-error,
        is-signed, is-iec559, tinyness-before,
        digits, digits10, max-digits10, radix,
        min-exponent, max-exponent, min-exponent10, max-exponent10,
        pi, e, ln2, ln10, log2e, log10e, sqrt2, sqrt3,
        inv-pi, inv-sqrt-pi, inv-sqrt-3, egamma, phi
    double-type/            % 33 entries: same structure, 64-bit values
    byte-type/              % 3 entries: is-signed, min, max
    integer-type/           % 3 entries: is-signed, min, max
    uinteger-type/          % 3 entries
    long-type/              % 3 entries
    ulong-type/             % 3 entries
```

### Type Promotion Rules

The `promote` operator widens numeric pairs to a common type using these
rules (highest priority first):

| Rule | Condition          | Result   |
| ---- | ------------------ | -------- |
| 1    | Either is Double   | Double   |
| 2    | Real + Long/ULong  | Double   |
| 3    | Either is Real     | Real     |
| 4    | Either is ULong    | ULong    |
| 5    | Either is Long     | Long     |
| 6    | Integer + UInteger | Long     |
| 7    | Either is UInteger | UInteger |
| 8    | Otherwise          | Integer  |

```
% Pairwise promotion
1.0 1l promote          % => 1.0d 1.0d (Real + Long -> Double)
1 2u promote            % => 1l 2l     (Integer + UInteger -> Long)
1.0 2 promote           % => 1.0 2.0   (Real + Integer -> Real)

% Array promotion
[ 1 2.0 3l ] promote    % => [ 1.0d 2.0d 3.0d ] (common type is Double)
```

---

## 4. Tutorial

### 4.1 Basic Floating-Point Arithmetic

All arithmetic operators are type-strict -- both operands must be the same
numeric type:

```
1.0 2.0 add             % => 3.0 (Real)
1.0d 2.0d mul           % => 2.0d (Double)
10.0 3.0 div            % => 3.3333333 (Real)
10.0d 3.0d div          % => 3.3333333333333335 (Double)

% Type mismatch is an error -- use promote first
{ 1.0 1 add } try       % => /type-check
1.0 1 promote add       % => 2.0 (Integer promoted to Real)
```

### 4.2 Fused Multiply-Add

`fma` computes x*y + z with a single rounding operation, which can be more
accurate than separate multiply and add:

```
% fma(2.0, 3.0, 4.0) = 2*3 + 4 = 10
2.0 3.0 4.0 fma         % => 10.0

% FMA avoids intermediate rounding error
% This matters for algorithms like dot product and polynomial evaluation
```

### 4.3 Detecting Special Values

```
inf inf?     % => true
inf neg inf? % => true (both +inf and -inf)
nan nan?     % => true
nan nan eq   % => false (IEEE 754: NaN != NaN)

1.0 finite?            % => true
inf finite?            % => false
nan finite?            % => false

1.0 normal?            % => true
0.0 normal?            % => false (zero is not normal)
0.0 ulp normal?        % => false (denorm-min is subnormal)
```

### 4.4 Signed Zero

IEEE 754 distinguishes +0.0 and -0.0. They compare equal with `eq` but
differ in sign bit:

```
0.0 -0.0 eq              % => true (IEEE 754: +0 == -0)
-0.0 sign-bit            % => true
0.0 sign-bit             % => false

% total-order? distinguishes them
-0.0 0.0 total-order?     % => true (-0 <= +0)
0.0 -0.0 total-order?     % => false (not +0 <= -0)
```

### 4.5 The IEEE 754 Total Order

The standard comparison operators have no total order on floating-point numbers
(NaN is unordered). The `total-order?` predicate provides a complete ordering
that places every floating-point value, including NaN and signed zero:

```
% Total order: -NaN < -inf < -finite < -0 < +0 < +finite < +inf < +NaN

nan neg inf neg total-order? % => true (-NaN <= -inf)
inf neg -1.0 total-order?    % => true (-inf <= -1)
-0.0 0.0 total-order?        % => true (-0 <= +0)
1.0 inf total-order?         % => true (1 <= +inf)
inf nan total-order?         % => true (+inf <= +NaN)

% total-order-mag? compares by absolute value
-3.0 5.0 total-order-mag?       % => true (|-3| <= |5|)
```

### 4.6 Comparisons and NaN

The relational comparisons (`lt`, `le`, `gt`, `ge`) return `false` when either
operand is NaN -- they never raise an error. The IEEE-named `?` variants
(`less?`, `greater?`, etc.) behave identically on NaN; they exist for naming
parity and to make NaN-tolerant intent explicit. The only operators with
distinct NaN semantics are `eq`/`ne` (NaN compares unequal to everything,
including itself) and the dedicated `unordered?` / total-order predicates.

```
% Relational comparisons return false on NaN -- no error
nan 1.0 lt                   % => false
1.0 nan gt                   % => false
{ nan 1.0 lt } try           % => /no-error (leaves the false result)

% The ? variants behave the same on NaN
nan 1.0 less?                % => false
1.0 nan greater?             % => false
nan nan less-greater?        % => false
nan 1.0 unordered?           % => true (either is NaN)

% Ordinary (non-NaN) comparisons
1.0 2.0 less?                % => true
3.0 2.0 greater-equal?       % => true
2.0 2.0 less-greater?        % => false (not less and not greater)
```

### 4.7 Floating-Point Classification

`fp-classify` returns a name identifying the IEEE 754 class of a value:

```
1.0 fp-classify                 % => /fp-normal
0.0 fp-classify                 % => /fp-zero
inf fp-classify                 % => /fp-infinite
nan fp-classify                 % => /fp-nan

% Subnormal (denormalized) values
//:systemdict:numbers:real-type:denorm-min
fp-classify                     % => /fp-subnormal
```

### 4.8 Rounding Modes

Trix provides seven rounding operators, each with different tie-breaking
behavior:

```
% round: half away from zero (schoolbook rounding)
1.5 round                       % => 2.0
2.5 round                       % => 3.0

% nearby-int: half to even (banker's rounding, uses current rounding mode)
1.5 nearby-int                  % => 2.0
2.5 nearby-int                  % => 2.0 (ties to even)

% round-even: always roundTiesToEven (ignores current rounding mode)
0.5 round-even                  % => 0.0 (even)
1.5 round-even                  % => 2.0 (even)
2.5 round-even                  % => 2.0 (even)
3.5 round-even                  % => 4.0 (even)

% floor/ceil/trunc
1.7 floor                       % => 1.0
1.2 ceil                        % => 2.0
-1.7 trunc                      % => -1.0
```

### 4.9 The Floating-Point Environment

Trix exposes the full C `<cfenv>` API for controlling rounding modes and
exception flags:

```
% Query current rounding mode
fe-get-round                      % => /fe-to-nearest (default)

% Change rounding mode
/fe-downward fe-set-round pop   % set rounding toward -infinity
3.1 nearby-int                  % => 3.0 (rounded down)
/fe-upward fe-set-round pop     % set rounding toward +infinity
3.1 nearby-int                  % => 4.0 (rounded up)
/fe-to-nearest fe-set-round pop % restore default

% round-even ignores the rounding mode
/fe-upward fe-set-round pop
4.1 round-even                  % => 4.0 (still rounds to nearest even)
/fe-to-nearest fe-set-round pop
```

### 4.10 FP Exception Flags

The FP environment tracks five exception flags that record floating-point
events:

```
% Load exception flag constants
/fe-all     //:systemdict:numbers:environment:fe-all-except def
/fe-inex    //:systemdict:numbers:environment:fe-inexact def
/fe-oflow   //:systemdict:numbers:environment:fe-overflow def

% Clear all flags
fe-all fe-clear-except pop

% Perform operations that raise flags
1.0 3.0 div pop                 % inexact (1/3 is not representable)

% Check which flags are set
fe-inex fe-test-except fe-inex eq % => true (inexact was raised)

% Manually raise a flag
fe-oflow fe-raise-except pop
fe-oflow fe-test-except fe-oflow eq  % => true

% Clear all flags when done
fe-all fe-clear-except pop
```

### 4.11 Saving and Restoring the FP Environment

For isolated FP computations, save the entire environment and restore it
afterward:

```
% Save complete environment (rounding mode + exception flags)
fe-get-env                         % => ulong (packed environment)

% ... perform computations that may change rounding/flags ...
/fe-downward fe-set-round pop
//:systemdict:numbers:environment:fe-overflow fe-raise-except pop

% Restore original environment
fe-set-env                         % rounding mode and flags restored
fe-get-round                       % => /fe-to-nearest (original)
```

The `fe-hold-except`/`fe-update-env` pair is more powerful -- it saves the
environment, clears exceptions, then merges any new exceptions when restoring:

```
% Save env, clear flags for isolated computation
fe-hold-except                     % => saved-env; flags now clear

% Perform computation -- only new exceptions accumulate
1.0 3.0 div pop                 % raises fe-inexact

% Restore original env, but merge new exceptions
fe-update-env                     % original flags OR new fe-inexact
```

---

## 5. Classification and Comparison

### IEEE 754 Number Classes

Every floating-point value belongs to exactly one class:

| Class     | `fp-classify`   | `is-*` Predicate    | Examples                    |
| --------- | --------------- | ------------------- | --------------------------- |
| Normal    | `/fp-normal`    | `normal?`           | 1.0, -42.5, 3.14e10         |
| Subnormal | `/fp-subnormal` | (use `fp-classify`) | denorm-min, ~1.4e-45 (Real) |
| Zero      | `/fp-zero`      | (use `fp-classify`) | 0.0, -0.0                   |
| Infinite  | `/fp-infinite`  | `inf?`              | inf, -inf                   |
| NaN       | `/fp-nan`       | `nan?`              | nan, nan#d                  |

The `finite?` predicate returns true for normal, subnormal, and zero
(everything except infinite and NaN).

### Comparisons and NaN

| Comparison Type | NaN Behavior | Operators |
| --- | --- | --- |
| **Relational** | Returns false | `lt`, `le`, `gt`, `ge` |
| **Relational (IEEE names)** | Returns false | `less?`, `less-equal?`, `greater?`, `greater-equal?` |
| **Identity** | NaN != NaN | `eq`, `ne` |
| **Unordered test** | Returns true if NaN | `unordered?` |
| **Total order** | NaN has defined position | `total-order?`, `total-order-mag?` |

The relational operators (`lt`/`le`/`gt`/`ge`) and their IEEE-named `?` variants
all return `false` on NaN -- there is no signaling/quiet distinction for them.
NaN comparisons never raise; only *arithmetic* on infinities/NaN raises
`/numerical-inf` (see Section 11).

### The Total Ordering

The IEEE 754-2019 `totalOrder` predicate defines a complete ordering on all
floating-point values, including NaN and signed zeros:

```
-NaN < -inf < ... < -0.0 < +0.0 < ... < +inf < +NaN
```

Within NaN values, ordering is by payload (sign, then payload magnitude).
This total ordering is useful for sorting, canonical comparisons, and
deterministic algorithms.

---

## 6. Rounding

### Rounding Operators Summary

| Operator     | Tie-Breaking                                 | Affected by `fe-set-round`? |
| ------------ | -------------------------------------------- | --------------------------- |
| `round`      | Half away from zero                          | No                          |
| `nearby-int` | Current rounding mode                        | Yes                         |
| `rint`       | Current rounding mode (may raise FE_INEXACT) | Yes                         |
| `round-even` | Half to even (banker's)                      | No                          |
| `ceil`       | Always toward +infinity                      | No                          |
| `floor`      | Always toward -infinity                      | No                          |
| `trunc`      | Always toward zero                           | No                          |

### Rounding Mode Constants

| Name              | Effect                                   |
| ----------------- | ---------------------------------------- |
| `/fe-to-nearest`  | Round to nearest, ties to even (default) |
| `/fe-downward`    | Round toward -infinity                   |
| `/fe-upward`      | Round toward +infinity                   |
| `/fe-toward-zero` | Round toward zero (truncation)           |

### Changing the Rounding Mode

```
% Set and query
/fe-upward fe-set-round           % => true (success)
fe-get-round                      % => /fe-upward

% Restore default
/fe-to-nearest fe-set-round pop
```

The rounding mode affects `nearby-int`, `rint`, and all arithmetic operations.
The `round-even` operator is independent of the rounding mode -- it always
uses roundTiesToEven regardless of the current setting.

---

## 7. The Floating-Point Environment

### Exception Flags

Five IEEE 754 exception flags track floating-point events:

| Flag             | Constant         | Triggered By                                 |
| ---------------- | ---------------- | -------------------------------------------- |
| Invalid          | `fe-invalid`     | Invalid operation (e.g., 0/0, sqrt(-1) in C) |
| Division by zero | `fe-div-by-zero` | Exact infinity result from finite operands   |
| Overflow         | `fe-overflow`    | Result exceeds representable range           |
| Underflow        | `fe-underflow`   | Result is subnormal or zero due to magnitude |
| Inexact          | `fe-inexact`     | Rounded result differs from exact result     |

Exception flags are sticky -- once raised, they remain set until explicitly
cleared. Trix uses a flag-only model (no trap handlers).

### Flag Operations

```
/fe-all   //:systemdict:numbers:environment:fe-all-except def
/fe-inex  //:systemdict:numbers:environment:fe-inexact def
/fe-oflow //:systemdict:numbers:environment:fe-overflow def

% Clear all flags
fe-all fe-clear-except pop

% Test specific flags
fe-inex fe-test-except             % => 0u (not set)

% Raise a flag manually
fe-oflow fe-raise-except pop
fe-oflow fe-test-except fe-oflow eq  % => true

% Test multiple flags with bitwise OR
fe-inex fe-oflow or fe-test-except   % => overflow flag value
```

### Environment Save/Restore

The full FP environment (rounding mode + exception flags) can be saved and
restored as a single packed ULong value:

| Bits  | Content         |
| ----- | --------------- |
| 0-31  | Exception flags |
| 32-63 | Rounding mode   |

```
/fe-inex  //:systemdict:numbers:environment:fe-inexact def
/fe-oflow //:systemdict:numbers:environment:fe-overflow def

% fe-get-env / fe-set-env: save and restore entire environment
fe-get-env                         % save
/fe-downward fe-set-round pop      % change rounding
fe-set-env                         % restore (rounding mode + flags)

% fe-hold-except: save, clear flags, then merge on restore
fe-oflow fe-raise-except pop       % set overflow
fe-hold-except                     % save env (with overflow), clear flags
fe-inex fe-raise-except pop        % raise inexact during computation
fe-update-env                      % restore original + merge: overflow | inexact
```

### Nested Environment Management

`fe-hold-except`/`fe-update-env` nest correctly for isolated computations:

```
fe-all fe-clear-except pop
fe-oflow fe-raise-except pop       % set overflow
fe-hold-except                     % outer: save overflow, clear
    fe-inex fe-raise-except pop    % raise inexact
    fe-hold-except                 % inner: save inexact, clear
        //:systemdict:numbers:environment:fe-underflow
        fe-raise-except pop        % raise underflow
    fe-update-env                  % inner restore: inexact | underflow
fe-update-env                      % outer restore: overflow | inexact | underflow
% all three flags now set
```

---

## 8. Elementary Functions

### Trigonometric

| Operator             | Stack Effect  | Description                            |
| -------------------- | ------------- | -------------------------------------- |
| `sin`                | `fp -- fp`    | Sine                                   |
| `cos`                | `fp -- fp`    | Cosine                                 |
| `tan`                | `fp -- fp`    | Tangent                                |
| `asin`               | `fp -- fp`    | Arcsine                                |
| `acos`               | `fp -- fp`    | Arccosine                              |
| `atan`               | `fp -- fp`    | Arctangent                             |
| `atan2`              | `fp fp -- fp` | Two-argument arctangent (y x -- angle) |
| `degrees-to-radians` | `fp -- fp`    | Degrees to radians                     |
| `radians-to-degrees` | `fp -- fp`    | Radians to degrees                     |

```
/pi //:systemdict:numbers:real-type:pi def
pi 4.0 div tan              % => ~1.0
0.0 cos                     % => 1.0
1.0 atan                    % => ~0.785398 (pi/4)
1.0 1.0 atan2               % => ~0.785398 (pi/4)

% Degree/radian conversion
180.0 degrees-to-radians     % => ~3.14159 (pi)
pi radians-to-degrees        % => ~180.0

% Round-trip
45.0 degrees-to-radians radians-to-degrees   % => ~45.0
```

All trigonometric functions preserve the input type (Real in, Real out;
Double in, Double out).

### Hyperbolic

| Operator | Stack Effect | Description                |
| -------- | ------------ | -------------------------- |
| `sinh`   | `fp -- fp`   | Hyperbolic sine            |
| `cosh`   | `fp -- fp`   | Hyperbolic cosine          |
| `tanh`   | `fp -- fp`   | Hyperbolic tangent         |
| `asinh`  | `fp -- fp`   | Inverse hyperbolic sine    |
| `acosh`  | `fp -- fp`   | Inverse hyperbolic cosine  |
| `atanh`  | `fp -- fp`   | Inverse hyperbolic tangent |

### Exponential and Logarithmic

| Operator | Stack Effect | Description                   |
| -------- | ------------ | ----------------------------- |
| `exp`    | `fp -- fp`   | e^x                           |
| `exp2`   | `fp -- fp`   | 2^x                           |
| `expm1`  | `fp -- fp`   | e^x - 1 (accurate near zero)  |
| `log`    | `fp -- fp`   | Natural logarithm             |
| `log2`   | `fp -- fp`   | Base-2 logarithm              |
| `log10`  | `fp -- fp`   | Base-10 logarithm             |
| `log1p`  | `fp -- fp`   | log(1+x) (accurate near zero) |

```
1.0 exp                      % => ~2.71828 (e)
8.0 log2                     % => 3.0
100.0 log10                  % => 2.0

% expm1 and log1p are essential for numerical stability near zero
% Without them: exp(1e-15) - 1 loses precision due to catastrophic cancellation
1e-15d expm1                 % accurate result (~1e-15)
1e-15d log1p                 % accurate result (~1e-15)
```

### Power and Root

| Operator | Stack Effect     | Description                                  |
| -------- | ---------------- | -------------------------------------------- |
| `sqrt`   | `fp -- fp`       | Square root                                  |
| `cbrt`   | `fp -- fp`       | Cube root                                    |
| `pow`    | `fp fp -- fp`    | Power: x^y                                   |
| `hypot`  | `fp fp -- fp`    | Hypotenuse: sqrt(x^2 + y^2) without overflow |
| `lerp`   | `fp fp fp -- fp` | Linear interpolation: a + t*(b-a)            |

```
4.0 sqrt                     % => 2.0
27.0 cbrt                    % => 3.0
-8.0 cbrt                    % => -2.0 (real cube root of negatives)
2.0 10.0 pow                 % => 1024.0
3.0 4.0 hypot                % => 5.0 (3-4-5 triangle)

% lerp: linear interpolation
0.0 10.0 0.5 lerp            % => 5.0 (halfway)
0.0 10.0 0.0 lerp            % => 0.0 (at a)
0.0 10.0 1.0 lerp            % => 10.0 (at b)
2.0 8.0 0.25 lerp            % => 3.5
```

---

## 9. Special Mathematical Functions

Trix exposes the full C++17 `<cmath>` special functions library. All functions
work with both Real and Double, preserving the input type.

### Error and Gamma Functions

| Operator | Stack Effect  | Description                                          |
| -------- | ------------- | ---------------------------------------------------- |
| `erf`    | `fp -- fp`    | Error function                                       |
| `erfc`   | `fp -- fp`    | Complementary error function: 1 - erf(x)             |
| `tgamma` | `fp -- fp`    | True gamma function                                  |
| `lgamma` | `fp -- fp`    | Natural log of gamma function                        |
| `beta`   | `fp fp -- fp` | Beta function: B(x,y) = gamma(x)*gamma(y)/gamma(x+y) |

```
1.0 lgamma                   % => 0.0 (log(gamma(1)) = log(1) = 0)
5.0 lgamma                   % => ~3.178 (log(4!) = log(24))
0.5 0.5 beta                 % => ~3.14159 (pi)
```

### Bessel Functions

| Operator       | Stack Effect    | Description                            |
| -------------- | --------------- | -------------------------------------- |
| `cyl-bessel-j` | `fp fp -- fp`   | Cylindrical Bessel J_nu(x), first kind |
| `cyl-bessel-i` | `fp fp -- fp`   | Modified Bessel I_nu(x)                |
| `cyl-bessel-k` | `fp fp -- fp`   | Modified Bessel K_nu(x)                |
| `cyl-neumann`  | `fp fp -- fp`   | Neumann Y_nu(x), second kind           |
| `sph-bessel`   | `uint fp -- fp` | Spherical Bessel j_n(x)                |
| `sph-neumann`  | `uint fp -- fp` | Spherical Neumann y_n(x)               |

```
% J_0(0) = 1, J_1(0) = 0
0.0 0.0 cyl-bessel-j         % => 1.0
1.0 0.0 cyl-bessel-j         % => 0.0

% J_0(1) ~ 0.7652
0.0 1.0 cyl-bessel-j         % => ~0.7652

% Spherical Bessel: j_0(x) = sin(x)/x
0u 1.0 sph-bessel            % => ~0.8415 (sin(1)/1)
```

### Elliptic Integrals

| Operator        | Stack Effect     | Description                                          |
| --------------- | ---------------- | ---------------------------------------------------- |
| `ellint-1`      | `fp fp -- fp`    | Incomplete elliptic integral, 1st kind F(k, phi)     |
| `ellint-2`      | `fp fp -- fp`    | Incomplete elliptic integral, 2nd kind E(k, phi)     |
| `ellint-3`      | `fp fp fp -- fp` | Incomplete elliptic integral, 3rd kind Pi(k, n, phi) |
| `comp-ellint-1` | `fp -- fp`       | Complete elliptic integral, 1st kind K(k)            |
| `comp-ellint-2` | `fp -- fp`       | Complete elliptic integral, 2nd kind E(k)            |
| `comp-ellint-3` | `fp fp -- fp`    | Complete elliptic integral, 3rd kind Pi(k, n)        |

```
% K(0) = pi/2
0.0 comp-ellint-1            % => ~1.5708 (pi/2)

% E(0) = pi/2
0.0 comp-ellint-2            % => ~1.5708 (pi/2)

% F(0, phi) = phi (degenerate case: k=0)
0.0 1.0 ellint-1             % => 1.0
```

### Orthogonal Polynomials

| Operator         | Stack Effect         | Description                     |
| ---------------- | -------------------- | ------------------------------- |
| `hermite`        | `uint fp -- fp`      | Hermite polynomial H_n(x)       |
| `laguerre`       | `uint fp -- fp`      | Laguerre polynomial L_n(x)      |
| `legendre`       | `uint fp -- fp`      | Legendre polynomial P_n(x)      |
| `assoc-laguerre` | `uint uint fp -- fp` | Associated Laguerre L_n^m(x)    |
| `assoc-legendre` | `uint uint fp -- fp` | Associated Legendre P_n^m(x)    |
| `sph-legendre`   | `uint uint fp -- fp` | Spherical harmonic Y_l^m(theta) |

```
% H_0(x) = 1, H_1(x) = 2x, H_2(x) = 4x^2 - 2
0u 1.0 hermite                % => 1.0
1u 3.0 hermite                % => 6.0
2u 1.0 hermite                % => 2.0

% P_0(x) = 1, P_1(x) = x, P_2(x) = (3x^2-1)/2
0u 0.5 legendre               % => 1.0
1u 0.5 legendre               % => 0.5
2u 1.0 legendre               % => 1.0

% Spherical harmonic Y_0^0 = 1/(2*sqrt(pi)) ~ 0.282
0u 0u 0.0 sph-legendre        % => ~0.282
```

### Other Special Functions

| Operator       | Stack Effect | Description                |
| -------------- | ------------ | -------------------------- |
| `expint`       | `fp -- fp`   | Exponential integral Ei(x) |
| `riemann-zeta` | `fp -- fp`   | Riemann zeta function      |

```
% Ei(1) ~ 1.895
1.0 expint                    % => ~1.895

% zeta(2) = pi^2/6 ~ 1.645
2.0 riemann-zeta              % => ~1.645

% Famous values
0.0 riemann-zeta              % => -0.5
-1.0 riemann-zeta             % => -1/12 (~-0.0833)
```

---

## 10. Advanced IEEE 754

### 10.1 Unit in Last Place (ULP)

The `ulp` operator returns the distance between a floating-point value and
the next representable value. This is the fundamental unit of precision at
that magnitude.

```
1.0 ulp                       % => ~1.19e-7 (Real epsilon)
1.0d ulp                      % => ~2.22e-16 (Double epsilon)
0.0 ulp                       % => denorm-min (smallest subnormal)
1000000.0 ulp                 % larger than ulp(1.0)

% Properties:
%   ulp(x) = ulp(-x)           -- symmetric
%   ulp(1.0) = machine epsilon -- by definition
%   x + ulp(x) = next-after(x, inf)  -- for normal positive x
%   ulp(inf) = NaN             -- undefined for non-finite
```

### 10.2 ULP-Based Comparison

`ulp-equal?` compares two values within a tolerance of N ULPs, which is the
principled way to test floating-point equality:

```
% Exact equality = 0 ULP
1.0 1.0 0 ulp-equal?           % => true

% Adjacent floats are 1 ULP apart
1.0 1.0 inf next-after 1 ulp-equal?       % => true
1.0 1.0 inf next-after 0 ulp-equal?       % => false

% Practical: test computed results
/pi //:systemdict:numbers:real-type:pi def
pi 6.0 div sin 0.5 4 ulp-equal?           % => true (sin(pi/6) ~ 0.5)

% Hex float literals for exact test vectors (no decimal rounding)
0x1.921FB6p1 pi 0 ulp-equal?              % => true (exact Real pi)
0x1.921FB54442D18p1#d
    //:systemdict:numbers:double-type:pi
    0 ulp-equal?                           % => true (exact Double pi)

% NaN is never ULP-equal to anything
nan nan 1000 ulp-equal?         % => false

% +inf equals +inf, but not -inf
inf inf 0 ulp-equal?            % => true
inf inf neg 1000 ulp-equal?     % => false
```

### 10.3 NaN Payloads

IEEE 754 quiet NaN values carry a payload -- an unsigned integer stored in
the significand bits. Trix exposes this for diagnostic tagging:

```
% Create NaN with specific payload
42u nan-with-payload           % => NaN (Real, payload=42)
42ul nan-with-payload          % => NaN (Double, payload=42)

% Extract payload
42u nan-with-payload nan-payload   % => 42u (UInteger)
42ul nan-with-payload nan-payload  % => 42ul (ULong)

% Payload limits
% Real:   22-bit payload (max 4,194,303)
% Double: 51-bit payload (max 2,251,799,813,685,247)

% Diagnostic tagging use case
/ERR_OVERFLOW  1u nan-with-payload def
/ERR_UNDERFLOW 2u nan-with-payload def
/ERR_INVALID   3u nan-with-payload def

% Later, check which error occurred:
ERR_OVERFLOW nan-payload 1u eq   % => true
```

### 10.4 Kahan Compensated Summation

`kahan-sum` uses the Kahan summation algorithm to accumulate array elements
with compensation for floating-point error. It always returns Double for
maximum precision.

```
% Basic usage
[ 1 2 3 4 5 ] kahan-sum       % => 15.0d

% The classic pathological case:
% Naive summation of 1e7 + 10 * 0.0001 loses the small terms
[ 10000000.0d
  0.0001d 0.0001d 0.0001d 0.0001d 0.0001d
  0.0001d 0.0001d 0.0001d 0.0001d 0.0001d
] kahan-sum
% => 10000000.001d (accurate; naive sum would lose the 0.001)

% big + small - big should be exactly small
[ 1e15d 1.0d -1e15d ] kahan-sum  % => 1.0d (exact!)

% Works with all numeric types (promoted to Double internally)
[ 1 2.0 3l 4.0d ] kahan-sum   % => 10.0d

% Empty array returns 0.0d
[ ] kahan-sum                  % => 0.0d
```

### 10.5 Compensated Dot Product

`dot-product` computes the inner product of two arrays using Kahan
compensation. Always returns Double.

```
[ 1 2 3 ] [ 4 5 6 ] dot-product     % => 32.0d (1*4 + 2*5 + 3*6)
[ 1.0 0.0 ] [ 0.0 1.0 ] dot-product % => 0.0d (orthogonal)

% Pathological case: cancellation without compensation
[ 1e15d 1.0d -1e15d ] [ 1.0d 1.0d 1.0d ] dot-product
% => 1.0d (exact! naive would lose the 1.0 term)

% Unit vector magnitude ~ 1
/pi-d //:systemdict:numbers:double-type:pi def
pi-d 4.0d div dup cos exch sin
2 array dup 0 4 index put dup 1 3 index put exch pop exch pop
dup dot-product 1.0d 4 ulp-equal?   % => true (cos^2 + sin^2 ~ 1)
```

### 10.6 Array Product

`product` computes the product of all elements in an array, preserving the
element type:

```
[ 1 2 3 4 5 ] product           % => 120 (Integer)
[ 1.0 2.0 3.0 ] product         % => 6.0 (Real)
[ 1.0d 2.0d 3.0d 4.0d ] product % => 24.0d (Double)
[ ] product                     % => 1 (identity element)

% 10! via product
[ 1 2 3 4 5 6 7 8 9 10 ] product  % => 3628800
```

---

## 11. Error Handling for Floating-Point

### Trix FP Error Policy

Trix takes a strict approach to floating-point errors: non-finite results from
arithmetic operations are rejected rather than silently propagated.

| Error Code | Condition | Example |
| --- | --- | --- |
| `/numerical-inf` | Arithmetic produces or receives infinity/NaN | `inf 1.0 add`, `nan 1.0 add` |
| `/numerical-nan` | Operation requires non-NaN input | `nan comp-ellint-1` |
| `/numerical-overflow` | Integer overflow in FP context | `[ 2147483647 2 ] product` |
| `/div-by-zero` | Division by zero | `1.0 0.0 div`, `0.0 0.0 remainder` |
| `/range-check` | Invalid domain | `-1.0 sqrt`, NaN payload overflow |
| `/undefined-result` | Mathematically undefined (NaN result, divisor non-zero) | `inf 1.0 remainder` |

This design prevents silent NaN propagation chains -- a common source of bugs
in other languages where a single NaN can contaminate an entire computation
without any error being raised.

```
% Arithmetic with inf/nan is an error
{ inf 1.0 add } try            % => /numerical-inf
{ nan 1.0 mul } try            % => /numerical-inf

% Domain errors
{ -1.0 sqrt } try              % => /range-check

% Division by zero
{ 1.0 0.0 div } try            % => /div-by-zero

% All errors are catchable
<< /numerical-inf { pop 0.0 }
   /div-by-zero   { pop 0.0 }
>> { some-fp-computation } try-catch
```

### Strict Arithmetic vs Classification

While arithmetic rejects non-finite values, classification and comparison
operators accept them -- that is their purpose:

```
% Classification operators accept all values
inf inf?        % => true (no error)
nan nan?        % => true (no error)
nan fp-classify % => /fp-nan (no error)

% Quiet comparisons accept NaN
nan 1.0 less?                 % => false (no error)
nan 1.0 unordered?            % => true (no error)

% Negation works on inf/nan (it flips the sign bit)
inf neg inf?                  % => true (no error)
nan neg nan?                  % => true (no error)
```

### Working with FP Exception Flags

The five IEEE 754 exception flags provide a non-throwing alternative to Trix's
error system. They record events without interrupting computation:

```
/fe-all //:systemdict:numbers:environment:fe-all-except def
fe-all fe-clear-except pop        % start clean

% Perform a series of computations
1.0 3.0 div pop                 % raises fe-inexact (but no Trix error)

% Check what happened
/fe-inex //:systemdict:numbers:environment:fe-inexact def
fe-inex fe-test-except fe-inex eq % => true

fe-all fe-clear-except pop        % clean up
```

---

## 12. Design Decisions

### Why Both Real and Double?

Most scripting languages provide only 64-bit double. Trix provides both
because:

- **Memory efficiency.** Real (32-bit) stores inline in the 8-byte Object slot
  with no heap allocation. Double requires an ExtValue (8 bytes on the VM
  heap). For large arrays of floating-point values, Real uses half the memory.
- **Embedded targets.** Trix supports pre-allocated memory constructors for
  embedded systems where 32-bit float may be the only hardware-supported
  precision.
- **Explicit precision.** When 64-bit precision is needed, the programmer
  writes `1.0d`. When 32-bit suffices, `1.0` avoids unnecessary heap
  allocation. This is a conscious choice, not a silent default.

### Why No Implicit FP Coercion?

```
{ 1.0 1 add } try      % => /type-check (not silently promoted)
```

Implicit coercion is the single largest source of floating-point surprises in
other languages. Python silently promotes `1 + 1.0` to `2.0`, losing the
information that the integer was exact. Julia's promotion rules are complex
enough to have their own documentation section. Trix eliminates this class of
bugs by requiring explicit `promote` before mixed-type operations.

### Why Strict Arithmetic Errors?

Trix raises `/numerical-inf` when arithmetic produces or receives infinity or
NaN, rather than silently propagating non-finite values. This prevents a
pattern that causes real-world bugs in other languages:

```python
# Python: NaN propagates silently through an entire pipeline
x = float('nan')
y = x + 1           # nan -- no error
z = y * 2           # nan -- no error
result = z > 0      # False -- silently wrong
```

In Trix, the first operation raises an error that can be caught and handled.
Silent NaN propagation is impossible.

### Why Expose the Full FP Environment?

Most scripting languages hide the FP environment because "users don't need
it." Trix exposes it because:

- **Interval arithmetic** requires rounding mode control to compute tight
  bounds.
- **Reproducibility** sometimes requires matching a specific platform's FP
  behavior.
- **Validation** of numerical algorithms requires checking which exception
  flags were raised during computation.
- **It costs nothing.** The operators map directly to C `<cfenv>` calls. If
  you do not use them, they have zero overhead.

### Why round-even Independent of Rounding Mode?

`round-even` always uses roundTiesToEven regardless of the current rounding
mode set by `fe-set-round`. This is intentional:

- The IEEE 754-2019 standard defines roundTiesToEven as the recommended
  default. It minimizes statistical bias in repeated rounding.
- If `round-even` respected `fe-set-round`, it would be redundant with
  `nearby-int`. Having an independent operator provides a stable rounding
  behavior that cannot be affected by environmental changes.

### Why NaN Payloads?

NaN payloads are a rarely-used IEEE 754 feature, but they solve a real problem:
when a computation produces NaN, *which* operation failed? In a long pipeline,
the NaN could have originated anywhere. With payload tagging:

<!-- doctest: skip (synopsis: result is a stand-in NaN-producing value) -->
```
/ERR_SQRT    1u nan-with-payload def
/ERR_LOG     2u nan-with-payload def
/ERR_DIVIDE  3u nan-with-payload def

% Later, when a NaN appears:
result nan? {
    result nan-payload
    dup 1u eq { pop (sqrt domain error) = } {
    dup 2u eq { pop (log domain error) = } {
                    (division error) =
    } if-else } if-else
} if
```

This provides per-operation error provenance without the overhead of
exception handling on every operation.

---

