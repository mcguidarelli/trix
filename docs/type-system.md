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

# Trix Type System and Type Safety

Trix is a strongly typed language with zero implicit conversions. Every
operator validates every operand before execution. Integer overflow is
detected and reported. Floating-point infinities and NaN values are rejected
by arithmetic. Read-only objects cannot be modified. There is no way to
produce undefined behavior from Trix code.

This document covers the complete type system: the 30 user-visible types, the
8-byte tagged value representation, the verify bitmask system, type predicates,
conversions, access control, and the design rationale.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [The 8-Byte Object](#3-the-8-byte-object)
4. [Type Catalog](#4-type-catalog)
5. [Tutorial](#5-tutorial)
6. [The Verify System](#6-the-verify-system)
7. [Numeric Type Safety](#7-numeric-type-safety)
8. [Type Predicates and Inspection](#8-type-predicates-and-inspection)
9. [Type Conversion](#9-type-conversion)
10. [Access Control and Execution](#10-access-control-and-execution)
11. [Structural Equality](#11-structural-equality)
12. [Immutability](#12-immutability)
13. [Design Decisions](#13-design-decisions)

---

## 1. Overview

Trix manages 30 user-visible types in a uniform 8-byte tagged representation.
Every value carries its type, access mode, execution attribute, and a per-type
flag in a single byte, leaving 7 bytes for a save level, a length field, and the
value (inline, or a heap offset for large types). This design enables:

- **Uniform representation.** All values are the same size. No boxing, no heap
  allocation for small values, no pointer indirection for scalars.
- **Zero implicit conversion.** `1 + 1.0` is a type error. `(3) + 1` is a
  type error. Every mixed-type operation requires explicit `promote` or `cast`.
- **Pre-execution validation.** Every operator declares its type requirements
  as a bitmask. Operands are checked before the operation executes -- not
  during, not after.
- **Complete overflow detection.** Every integer arithmetic operation uses
  hardware overflow detection (`__builtin_*_overflow`). Every floating-point
  result is checked for infinity and NaN.
- **Access enforcement.** Read-only arrays, strings, and dicts cannot
  be modified. The access bit is checked by the verify system before any
  mutation.

| Property                  | Trix             | Python           | Lua              | Ruby             |
| ------------------------- | ---------------- | ---------------- | ---------------- | ---------------- |
| Implicit numeric coercion | Never            | Always           | Always           | Always           |
| Integer overflow          | Detected         | Silent (bigint)  | Silent (float)   | Silent (bigint)  |
| FP overflow detection     | Immediate error  | Silent infinity  | Silent infinity  | Silent infinity  |
| Operand type checking     | Before execution | During execution | During execution | During execution |
| Read-only enforcement     | Bit-level        | Convention       | None             | `.freeze`        |

---

## 2. Quick Reference

### All Types

| Type         | Literal                         | Size     | Storage   | Category            |
| ------------ | ------------------------------- | -------- | --------- | ------------------- |
| Null         | `null`                          | 0        | Inline    | Singleton           |
| Byte         | `42b`                           | 8-bit    | Inline    | Unsigned integer    |
| Integer      | `42`                            | 32-bit   | Inline    | Signed integer      |
| UInteger     | `42u`                           | 32-bit   | Inline    | Unsigned integer    |
| Long         | `42l`                           | 64-bit   | ExtValue  | Signed integer      |
| ULong        | `42ul`                          | 64-bit   | ExtValue  | Unsigned integer    |
| Int128       | `42q`                           | 128-bit  | WideValue | Signed integer      |
| UInt128      | `42uq`                          | 128-bit  | WideValue | Unsigned integer    |
| Address      | `42a`                           | 64-bit   | ExtValue  | Pointer             |
| Real         | `3.14`                          | 32-bit   | Inline    | Floating-point      |
| Double       | `3.14d`                         | 64-bit   | ExtValue  | Floating-point      |
| Boolean      | `true` / `false`                | 1-bit    | Inline    | Logic               |
| Operator     | (built-in)                      | --       | Inline    | Callable            |
| Mark         | `mark`                          | --       | Inline    | Stack marker        |
| Name         | `/foo`                          | varies   | VM heap   | Interned name       |
| Array        | `[1 2 3]`                       | varies   | VM heap   | Container           |
| Packed       | (compressed)                    | varies   | VM heap   | Container           |
| String       | `(hello)`                       | varies   | VM heap   | Container           |
| Stream       | `stream`                        | varies   | VM heap   | I/O                 |
| Dict         | `<< /a 1 >>`                    | varies   | VM heap   | Container           |
| Curry        | `curry` result                  | 16 bytes | VM heap   | Callable            |
| Thunk        | `thunk` result                  | 24 bytes | VM heap   | Lazy                |
| SourceLoc    | (internal)                      | --       | Inline    | Internal            |
| Set          | `{{ 1 2 3 }}`                   | varies   | VM heap   | Container           |
| Tagged       | `tag` result                    | 16 bytes | VM heap   | Discriminated value |
| Record       | `record` result                 | varies   | VM heap   | Composite           |
| Coroutine    | `coroutine-launch` result       | --       | VM heap   | Runtime             |
| PipeBuffer   | (internal)                      | --       | VM heap   | Runtime             |
| Cell         | `cell` / `cell-computed` result | --       | VM heap   | Reactive            |
| Continuation | `capture` result                | --       | VM heap   | Runtime             |
| OpaqueHandle | `make-screen` result            | --       | VM heap   | Host handle         |

### Type Predicates

| Operator          | True For                                                            |
| ----------------- | ------------------------------------------------------------------- |
| `is-null`         | Null                                                                |
| `is-byte`         | Byte                                                                |
| `is-integer`      | Integer                                                             |
| `is-uinteger`     | UInteger                                                            |
| `is-long`         | Long                                                                |
| `is-ulong`        | ULong                                                               |
| `is-int128`       | Int128                                                              |
| `is-uint128`      | UInt128                                                             |
| `is-address`      | Address                                                             |
| `is-real`         | Real                                                                |
| `is-double`       | Double                                                              |
| `is-boolean`      | Boolean                                                             |
| `is-operator`     | Operator                                                            |
| `is-mark`         | Mark                                                                |
| `is-name`         | Name                                                                |
| `is-array`        | Array                                                               |
| `is-packed`       | Packed                                                              |
| `is-string`       | String                                                              |
| `is-stream`       | Stream                                                              |
| `is-dict`         | Dict                                                                |
| `is-curry`        | Curry                                                               |
| `is-thunk`        | Thunk                                                               |
| `is-set`          | Set                                                                 |
| `is-tagged`       | Tagged                                                              |
| `is-record`       | Record                                                              |
| `is-coroutine`    | Coroutine                                                           |
| `is-pipebuffer`   | PipeBuffer                                                          |
| `is-cell`         | Cell                                                                |
| `is-continuation` | Continuation                                                        |
| `is-number`       | Byte, Integer, UInteger, Long, ULong, Int128, UInt128, Real, Double |
| `is-signed`       | Integer, Long, Int128                                               |
| `is-unsigned`     | Byte, UInteger, ULong, UInt128                                      |
| `is-float`        | Real, Double                                                        |
| `finite?`         | Real or Double, finite value                                        |
| `inf?`            | Real or Double, infinity                                            |
| `nan?`            | Real or Double, NaN                                                 |
| `normal?`         | Real or Double, normal (not subnormal/zero/inf/NaN)                 |
| `readable?`       | Object has read access                                              |
| `writable?`       | Object has write access                                             |
| `executable?`     | Object has executable attribute                                     |

### Type Conversion Operators

| Operator | Stack Effect | Description |
| --- | --- | --- |
| `type` | `any -- name` | Push type name (e.g., `/integer-type`) |
| `cast` | `num typename -- num` | Convert with value preservation |
| `reinterpret` | `num typename -- num` | Reinterpret bit pattern |
| `promote` | `num num -- num num` | Widen pair to common type |
| `promote` | `array -- array` | Widen all elements to common type |
| `coerce` | `container typename -- container` | Widen all elements/values of an array, packed array, or dict to the named numeric/boolean type (narrowing raises `/type-check`) |
| `type-case` | `any dict -- ...` | Dispatch by type name |

---

## 3. The 8-Byte Object

Every Trix value is an 8-byte `Object` consisting of a 1-byte attribute tag
and 7 bytes of header plus value: a 1-byte save level, a 2-byte type-specific
length field, and a 4-byte value/offset slot.

### The Attribute Byte (aat_t)

```
  7   6   5   4   3   2   1   0
+---+---+---+---+---+---+---+---+
| X | W | F | T | T | T | T | T |
+---+---+---+---+---+---+---+---+
  |   |   |   \               /
  |   |   |    -------+-------
  |   |   |           |
  |   |   |           +-----  Type (5 bits, values 0-30)
  |   |   +----------- F: per-type special flag
  |   +--------------- W: 0 = ReadOnly, 1 = ReadWrite
  +------------------- X: 0 = Literal, 1 = Executable
```

Four properties are encoded in a single byte:

| Field | Bits | Values | Purpose |
| --- | --- | --- | --- |
| **Type** | 0-4 | 0-30 | Identifies which of 31 types the value is (30 user-visible + SourceLoc) |
| **Special** | 5 | per-type | `F` flag: Boolean value / Address-cache bit / eqref marker, otherwise 0 |
| **Access** | 6 | 0=ReadOnly, 1=ReadWrite | Controls mutation permission |
| **Execution** | 7 | 0=Literal, 1=Executable | Controls interpreter behavior |

### Value Storage

The remaining 7 bytes are a 1-byte save level, a 2-byte length field, and a
4-byte value slot. Small values (Byte, Integer,
UInteger, Real, Boolean) store directly inline. Large values (Long, ULong,
Double, Address) store a VM offset pointing to an 8-byte ExtValue on the
heap. 128-bit values (Int128, UInt128) store a VM offset pointing to a
16-byte WideValue on the heap. Composite values (Array, String, Dict, Name)
store an offset plus a length.

| Storage Model    | Types                                                        | Heap Required |
| ---------------- | ------------------------------------------------------------ | ------------- |
| **Inline**       | Null, Byte, Integer, UInteger, Real, Boolean, Operator, Mark | No            |
| **ExtValue**     | Long, ULong, Double, Address                                 | 8 bytes       |
| **WideValue**    | Int128, UInt128                                              | 16 bytes      |
| **VM composite** | Name, Array, Packed, String, Stream, Dict, Curry, Thunk, Set | Variable      |

This design means that most common values (integers, booleans, small floats)
require zero heap allocation. Only 64-bit and 128-bit numeric types and
composite objects need VM heap space.

---

## 4. Type Catalog

### Numeric Types

#### Byte (8-bit unsigned)

```
42b                             % literal with b suffix
0b                              % minimum: 0
255b                            % maximum: 255
42b type                        % => /byte-type
```

Bytes are unsigned 8-bit integers (0-255). They are the element type for
string indexing and I/O operations. Overflow is detected:

```
{ 255b 1b add } try            % => /numerical-overflow
```

#### Integer (32-bit signed)

```
42                              % default integer literal
-2147483648                     % minimum
2147483647                      % maximum
16#FF#                          % hex literal: 255
2#1010#                         % binary literal: 10
```

The default integer type. Overflow is detected via GCC hardware builtins:

```
{ 2147483647 1 add } try       % => /numerical-overflow
{ -2147483648 neg } try        % => /numerical-overflow
{ -2147483648 abs } try        % => /numerical-overflow
```

#### UInteger (32-bit unsigned)

```
42u                             % literal with u suffix
0u                              % minimum
4294967295u                     % maximum: 2^32-1
```

#### Long (64-bit signed)

```
42l                             % literal with l suffix
-9223372036854775808l           % minimum
9223372036854775807l            % maximum
```

Long values require ExtValue storage (8 bytes on the VM heap). They are
managed automatically by save/restore.

#### ULong (64-bit unsigned)

```
42ul                            % literal with ul suffix
0ul                             % minimum
18446744073709551615ul          % maximum: 2^64-1
```

#### Int128 (128-bit signed)

```
42q                                       % literal with q (quad) suffix
-170141183460469231731687303715884105728q % minimum: -(2^127)
170141183460469231731687303715884105727q  % maximum: 2^127 - 1
16#7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF#q     % INT128_MAX (hex)
```

Int128 values require WideValue storage (16 bytes on the VM heap). They
are managed automatically by save/restore. Arithmetic raises
`numerical-overflow` on overflow; `__builtin_*_overflow` intrinsics
detect overflow natively on 128-bit.

#### UInt128 (128-bit unsigned)

```
42uq                                      % literal with uq (unsigned quad) suffix
0uq                                       % minimum
340282366920938463463374607431768211455uq % maximum: 2^128 - 1
16#FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF#uq    % UINT128_MAX (hex)
```

UInt128 values require WideValue storage. Typical uses: crypto/hash
literals, UUIDs, Snowflake IDs, Unix nanosecond timestamps with decade
spans, financial multiplications without precision loss.

#### Real (32-bit IEEE 754 single precision)

```
3.14                            % default float literal
1.5e-10                         % scientific notation
3.14r                           % explicit r suffix
inf                             % positive infinity
nan                             % quiet NaN
```

Real values store inline (32 bits fit in the Object). Arithmetic rejects
non-finite inputs and results:

```
{ inf 1.0 add } try            % => /numerical-inf
{ nan 1.0 mul } try            % => /numerical-inf
```

#### Double (64-bit IEEE 754 double precision)

```
3.14d    % literal with d suffix
1.5e-10d % scientific notation
inf#d    % positive infinity
nan#d    % quiet NaN
```

Double values require ExtValue storage.

### Non-Numeric Types

#### Null

```
null                            % the only null value
null is-null                    % => true
```

Null is a singleton type. It cannot be used as a dict key.

#### Boolean

```
true                            % true
false                           % false
true is-boolean                 % => true
```

Booleans are not numbers. `true + 1` is a type error.

#### Name

<!-- doctest: skip (name-form syntax illustration; \foo and //foo require foo defined) -->
```
/foo                            % literal name (data)
\foo                            % executable name (looked up when executed)
//foo                           % immediate lookup (resolved at parse time)
/foo type                       % => /name-type
```

Names are immutable interned strings stored in the VM. They serve as
dict keys and variable names. Names cache their hash value
and can cache their most recent dict binding for O(1) re-lookup.

#### String

```
(hello world)                   % string literal
(hello) length                  % => 5
(hello) 0 get                   % => 104b (ASCII 'h' as Byte)
```

Strings are byte sequences stored in VM memory. They can be read-only or
read-write. String indexing returns Byte values.

#### Array

```
[1 2 3]                         % array of 3 integers
[1 (hello) true]                % heterogeneous array
[1 2 3] length                  % => 3
[1 2 3] 0 get                   % => 1
```

Arrays are ordered sequences of Objects stored in VM memory. They support
random access by integer index.

#### Packed Array

Packed arrays are a compressed representation of arrays, using a compact
encoding (31 packed types) that reduces memory usage. They support the same
operations as arrays but are always read-only.

```
save /sv exch def
1 2 3 3 packed                  % create packed array from top 3 stack items
dup length =                    % => 3
0 get =                         % => 1
sv restore
```

#### Dict

```
<< /a 1 /b 2 >>      % dict literal
<< /a 1 >> /a get    % => 1
<< /a 1 >> /a known? % => true
```

Dictionaries are hash tables mapping Names to Objects. Three modes control
mutability:

| Mode             | Can Add Keys?       | Can Modify Values? |
| ---------------- | ------------------- | ------------------ |
| ReadOnly         | No                  | No                 |
| ReadWriteFixed   | No (capacity fixed) | Yes                |
| ReadWriteDynamic | Yes                 | Yes                |

#### Stream

```
(file.txt) 114b stream          % open file for reading ('r' = 114)
```

Streams are I/O abstractions for file and string input/output. They carry
access control (read-only or read-write) and are managed by `with-stream`
for guaranteed cleanup.

#### Save tokens

```
save                            % push save checkpoint as Integer token
/sv exch def
% ... operations ...
sv restore                      % roll back all changes
% or:
save save save                  % three nested levels
-3 restore                      % unwind 3 levels at once via -N
```

`save` returns an inline `Integer` whose 32-bit value packs the slot
identity for the new save level (`level | (gen ^ barrier_low23) << 8`).
Stale tokens (slot recycled by a subsequent save+restore) are rejected
on `restore` with `/invalid-restore`.  A negative integer pops `|N|`
save levels relatively.

#### Operator

Operators are native C++ function pointers. They are the implementations of
built-in operations like `add`, `def`, `get`, etc. Users cannot create
operators directly (except via the user operator API from C++).

#### Mark

```
mark                            % push a mark on the stack
1 2 3 count-to-mark             % => 3
```

Marks are stack markers used for variadic operations like array construction
(`[` pushes a mark, `]` collects to it).

#### Address

```
42a                             % address literal
```

Address values hold raw pointers for FFI and embedded use. They support
pointer arithmetic (`add`, `sub`) and `reinterpret` for casting to/from
integer types.

#### Curry

```
42 { add } curry                % create partial application
7 exch exec                     % => 49 (42 + 7)
```

Curry objects capture a value and a callable, implementing partial
application. When executed, the captured value is pushed before the callable.

#### Thunk

```
{ 3 4 add } thunk % create lazy evaluation wrapper
force             % => 7 (proc is called, result cached)
force             % => 7 (cached, proc not called again)
```

Thunk objects wrap a procedure for lazy evaluation. The procedure is not
executed until the thunk is forced. Once forced, the result is cached and
subsequent forces return the cached value in O(1). Thunks support cycle
detection (forcing a thunk during its own evaluation raises
`/undefined-result`). State can be inspected with `thunk-evaluated?` and
reset with `thunk-reset`.

#### Set

```
{{ 1 2 3 }}                     % integer set
{{ /a /b /c }}                  % name set
{{ 1 /a (hello) true }}         % mixed-type (heterogeneous) set
{{ 1 2 3 }} length              % => 3
{{ 1 2 3 }} 2 set-member?       % => true
```

Sets are unordered collections of unique keys stored in VM memory. Internally,
each entry is a 12-byte `SetEntry` (chain + Object key, no value), sharing
Dict's `PackedType` encoding with a header bit to discriminate sets from dicts.
Any hashable key type is accepted, and a single set may contain keys of
different types (heterogeneous). Sets support `length`, `set-member?`, `set-add`,
`set-remove`, `for-all`, and set-theoretic operations. Like dicts, sets carry
a ReadOnly access bit: `{{ }}` literals are read-only; `set` allocates a
read-write set.

---

## 5. Tutorial

### 5.1 Type Safety in Action

Every operator checks its operand types before executing:

```
% Same-type arithmetic works
1 2 add     % => 3 (Integer + Integer)
1.0 2.0 add % => 3.0 (Real + Real)
1l 2l add   % => 3l (Long + Long)

% Mixed-type arithmetic is rejected
{ 1 1.0 add } try              % => /type-check
{ 1 1l add } try               % => /type-check
{ (3) 1 add } try              % => /type-check
{ true 1 add } try             % => /type-check
```

This is the core principle: **no operation silently changes the type of a
value.** If you want to add an Integer to a Real, you must explicitly promote:

```
1 1.0 promote add               % => 2.0 (both promoted to Real)
```

### 5.2 Integer Overflow Detection

Every integer arithmetic operation is checked for overflow using GCC hardware
builtins (`__builtin_sadd_overflow`, `__builtin_smul_overflow`, etc.):

```
% Addition overflow
{ 2147483647 1 add } try        % => /numerical-overflow

% Subtraction overflow
{ -2147483648 1 sub } try       % => /numerical-overflow

% Multiplication overflow
{ 2147483647 2 mul } try        % => /numerical-overflow

% Negation overflow (INT_MIN has no positive counterpart)
{ -2147483648 neg } try         % => /numerical-overflow
{ -2147483648 abs } try         % => /numerical-overflow

% Byte overflow
{ 255b 1b add } try             % => /numerical-overflow
{ 0b 1b sub } try               % => /numerical-overflow

% All integer types are checked: Byte, Integer, UInteger, Long, ULong
{ 18446744073709551615ul 1ul add } try  % => /numerical-overflow
```

Compare this to other languages:

```python
# Python: silently promotes to unlimited-precision bigint
2147483647 + 1                  # 2147483648 (no error)

# JavaScript: silently loses precision
2147483647 + 1                  // 2147483648 (float64, not int)

# Lua: silently converts to float
2147483647 + 1                  -- 2147483648.0 (no error)
```

### 5.3 Floating-Point Error Detection

Trix rejects arithmetic that produces or receives infinity or NaN:

```
% Division by zero
{ 1.0 0.0 div } try            % => /div-by-zero

% Overflow to infinity
{ 3.4e38 3.4e38 mul } try      % => /numerical-inf

% Operations on infinity
{ inf 1.0 add } try            % => /numerical-inf

% Operations on NaN
{ nan 1.0 mul } try            % => /numerical-inf

% Domain errors
{ -1.0 sqrt } try              % => /range-check
```

### 5.4 Read-Only Protection

Objects can be marked read-only. The verify system enforces this before any
mutation:

```
% Read-only string
(hello)#r                   % create read-only string
{ (hello)#r 0 65b put } try % => /read-only

% Read-only array (use the #r suffix; bare literals are writable)
[1 2 3]#r                  % read-only array
{ [1 2 3]#r 0 99 put } try % => /read-only

% systemdict is read-only
{ systemdict begin /test 1 def } try  % => /read-only

% Writable array
3 array                         % creates writable 3-element array
dup 0 42 put                    % succeeds
```

### 5.5 Type Inspection

```
42 type                         % => /integer-type
42l type                        % => /long-type
(hello) type                    % => /string-type
true type                       % => /boolean-type
[1 2] type                      % => /array-type

% Conditional dispatch by type (value first, then the handler dict)
42 << /integer-type  { (it is an integer) }
      /string-type   { (it is a string) }
      /default       { (it is something else) }
   >> type-case
% => (it is an integer)
```

### 5.6 The Difference: Comparison with Python

Consider a function that processes user input:

```python
# Python: silent coercion hides bugs
def area(width, height):
    return width * height

area("10", 5)      # "1010101010" (string repetition, not multiplication!)
area(True, 5)      # 5 (True coerced to 1)
```

In Trix, both are type errors caught before execution:

```
/area { mul } def

% { (10) 5 mul } try       => /type-check
% { true 5 mul } try       => /type-check

% Correct usage
10 5 area                   % => 50
```

---

## 6. The Verify System

### How Operators Declare Type Requirements

Every built-in operator declares its operand types using a bitmask system.
The `verify_t` type is a 64-bit integer where:

- **Bits 0-30**: One bit per type (1 = accepted, 0 = rejected)
- **Bits 32+**: Constraint flags (non-zero, non-negative, finite, etc.)

<!-- doctest: skip (C++ verify-declaration illustration, not Trix code) -->
```
% add requires two Numbers (any of 9 numeric types)
verify_operands(VerifyNumbers, VerifyNumbers)

% def requires a Key and a ReadWrite Dict
verify_operands(VerifyAny, VerifyKey, VerifyRWDict)

% get requires an Integer index and an Indexable (String|Array|Packed)
verify_operands(VerifyIntegers, VerifyIndexable)

% try requires an executable Proc (Array|Packed with Executable attribute)
verify_operands(VerifyProc)

% asin requires a Float with |value| <= 1
verify_operands(VerifyFloats | VerifyIsAbsLessEqOne)
```

### Pre-Defined Type Groups

| Mask                     | Types Accepted                                                      |
| ------------------------ | ------------------------------------------------------------------- |
| `VerifyAny`              | All user-visible types                                              |
| `VerifyNumbers`          | Byte, Integer, UInteger, Long, ULong, Int128, UInt128, Real, Double |
| `VerifyIntegers`         | Byte, Integer, UInteger, Long, ULong, Int128, UInt128               |
| `VerifySignedIntegers`   | Integer, Long, Int128                                               |
| `VerifyUnsignedIntegers` | Byte, UInteger, ULong, UInt128                                      |
| `VerifyFloats`           | Real, Double                                                        |
| `VerifyArrays`           | Array, Packed                                                       |
| `VerifyProc`             | Array or Packed with Executable attribute                           |
| `VerifyIndexable`        | String, Array, Packed                                               |
| `VerifyKey`              | All types except Null (valid dict keys)                             |
| `VerifyHasLength`        | Name, Array, Packed, String, Dict, Set, Record                      |
| `VerifyHasAccess`        | Array, Packed, String, Stream, Dict, Set                            |

### Constraint Flags

Beyond type checking, the verify system enforces value constraints:

| Flag                   | Meaning                                           | Error            |
| ---------------------- | ------------------------------------------------- | ---------------- |
| `VerifyRW`             | Requires ReadWrite access                         | `/read-only`     |
| `VerifyExe`            | Requires Executable attribute                     | `/type-check`    |
| `VerifyNotZero`        | Numeric value must be non-zero                    | `/range-check`   |
| `VerifyNotNegative`    | Numeric value must be >= 0                        | `/range-check`   |
| `VerifyNotInf`         | Float must not be infinity                        | `/numerical-inf` |
| `VerifyNotNan`         | Float must not be NaN                             | `/numerical-nan` |
| `VerifyIsFinite`       | Float must be finite (not inf or NaN)             | `/numerical-inf` |
| `VerifyIsNormal`       | Float must be normal (not subnormal/zero/inf/NaN) | `/numerical-inf` |
| `VerifyIsAbsLessEqOne` | Absolute value must be <= 1                       | `/range-check`   |

These constraints are combined with type masks using bitwise OR:

<!-- doctest: skip (C++ verify-declaration illustration, not Trix code) -->
```
% An operator requiring a non-zero finite second operand declares:
verify_operands(VerifyNumbers | VerifyIsFinite,
                VerifyNumbers | VerifyIsFinite | VerifyNotZero)
% (div itself uses VerifyIsFinite on both operands and does its own zero
%  check, raising /div-by-zero rather than the /range-check VerifyNotZero gives.)
```

### Match Policy

Some operators require all operands to be the same type:

<!-- doctest: skip (C++ verify-declaration illustration, not Trix code) -->
```
% add uses MatchPolicy::AllMustMatch -- both operands same type
verify_matched_operands(VerifyNumbers, VerifyNumbers)

1 2 add           % OK: Integer + Integer
{ 1 1.0 add } try % /type-check: Integer != Real
```

### Error Messages

When verification fails, the error message identifies the exact operand and
the mismatch:

```
{ 1 (hello) add } try pop
last-error-message
% => "operand #1 type mismatch: expected Byte|Integer|UInteger|Long|ULong|Real|Double|Int128|UInt128, actual String"

{ [1 2 3]#r 0 99 put } try pop
last-error-message
% => "operand #3 is ReadOnly Array while ReadWrite access is required"
```

---

## 7. Numeric Type Safety

### Nine Numeric Types

Trix has nine numeric types arranged in two dimensions:

|                      | 8-bit | 32-bit   | 64-bit | 128-bit |
| -------------------- | ----- | -------- | ------ | ------- |
| **Unsigned integer** | Byte  | UInteger | ULong  | UInt128 |
| **Signed integer**   | --    | Integer  | Long   | Int128  |
| **Floating-point**   | --    | Real     | Double | --      |

Every arithmetic operator works on all nine types but requires both operands
to be the same type. There is no implicit widening, no silent truncation, and
no automatic float-to-int conversion.

### Overflow Detection by Type

| Type     | Detection Method                        | Example                                      |
| -------- | --------------------------------------- | -------------------------------------------- |
| Byte     | Range check (0-255)                     | `255b 1b add` => overflow                    |
| Integer  | `__builtin_sadd_overflow`               | `2147483647 1 add` => overflow               |
| UInteger | `__builtin_uadd_overflow`               | `4294967295u 1u add` => overflow             |
| Long     | `__builtin_saddll_overflow`             | `9223372036854775807l 1l add` => overflow    |
| ULong    | `__builtin_uaddll_overflow`             | `18446744073709551615ul 1ul add` => overflow |
| Int128   | `__builtin_add_overflow` (type-generic) | `INT128_MAX 1q add` => overflow              |
| UInt128  | `__builtin_add_overflow` (type-generic) | `UINT128_MAX 1uq add` => overflow            |
| Real     | `std::isfinite()` result check          | `3.4e38 3.4e38 mul` => inf                   |
| Double   | `std::isfinite()` result check          | `1.7e308d 2.0d mul` => inf                   |

Every arithmetic operator (`add`, `sub`, `mul`, `div`, `mod`, `neg`, `abs`)
checks for overflow on every invocation. Integer checks use GCC hardware
overflow builtins that compile to a single instruction + branch on the
overflow flag. Floating-point checks verify the result is finite after each
operation.

### No Implicit Coercion

Trix never silently converts between numeric types:

```
% All of these are type errors:
{ 1 1.0 add } try              % Integer + Real => /type-check
{ 1 1l add } try               % Integer + Long => /type-check
{ 1b 1 add } try               % Byte + Integer => /type-check
{ 1u 1 add } try               % UInteger + Integer => /type-check
{ 1.0 1.0d add } try           % Real + Double => /type-check
```

To perform mixed-type arithmetic, use `promote` to explicitly widen both
operands to a common type:

```
1 1.0 promote add               % => 2.0 (Integer -> Real, then add)
1 1l promote add                % => 2l (Integer -> Long, then add)
1.0 1l promote add              % => 2.0d (both -> Double, then add)
```

### Promotion Rules

`promote` widens both operands to the narrowest type that can represent both
without loss. The rules, in priority order:

| # | Condition | Result |
| --- | --- | --- |
| 1 | Either is Double | Double |
| 2 | Real + Long/ULong/Int128/UInt128 | Double (Real cannot represent 64/128-bit integers) |
| 3 | Either is Real | Real |
| 4a | UInt128 + Integer | Int128 (chosen as the type representing both, mirroring Integer+UInteger -> Long) |
| 4b | Either is UInt128 (other combos) | UInt128 (Int128 -> UInt128 sign-loss accepted) |
| 5 | Either is Int128 | Int128 |
| 6 | Either is ULong | ULong |
| 7 | Either is Long | Long |
| 8 | Integer + UInteger | Long (safe: both fit in 64-bit signed) |
| 9 | Either is UInteger | UInteger |
| 10 | Otherwise | Integer |

```
% Examples of promotion results
1 2.0 promote type              % => /real-type (rule 3)
1 2l promote type               % => /long-type (rule 7)
1 2u promote type               % => /long-type (rule 8: safe widening)
1.0 2l promote type             % => /double-type (rule 2)
1.0 2.0d promote type           % => /double-type (rule 1)
```

Array form promotes all elements to the common type:

```
[ 1 2.0 3l ] promote           % => [ 1.0d 2.0d 3.0d ] (Double is common)
[ 1 2 3u ] promote             % => [ 1l 2l 3l ] (Long is common)
```

### Cast vs Reinterpret

**`cast`** converts the value, potentially changing it (truncation, rounding):

```
3.7 /integer-type cast          % => 3 (truncated toward zero)
-1 /uinteger-type cast          % => 4294967295u (bit pattern preserved)
42 /boolean-type cast           % => true (non-zero = true)
0 /boolean-type cast            % => false
```

**`reinterpret`** preserves the bit pattern and only changes the type tag:

```
42 /real-type reinterpret       % => 5.9e-44 (raw bits as float)
1.0 /integer-type reinterpret   % => 1065353216 (IEEE 754 bits as int)
```

`cast` is for value conversion. `reinterpret` is for bit-level manipulation.

---

## 8. Type Predicates and Inspection

### Per-Type Predicates

Every user-visible type has a corresponding `is-*` predicate, except
OpaqueHandle. OpaqueHandle values are tested with `screen?` (true iff the
operand is an OpaqueHandle of kind Screen) or `handle-kind` (returns `/screen`,
or null for non-handles); their `/opaque-handle-type` result of `type` also
identifies them.

```
42 is-integer                   % => true
42 is-real                      % => false
(hello) is-string               % => true
[1 2] is-array                  % => true
null is-null                    % => true
true is-boolean                 % => true
```

### Category Predicates

Four predicates test numeric categories:

```
% is-number: any of the 9 numeric types
42 is-number                    % => true
3.14 is-number                  % => true
(hello) is-number               % => false

% is-signed: Integer, Long, or Int128
42 is-signed                    % => true
42l is-signed                   % => true
42u is-signed                   % => false

% is-unsigned: Byte, UInteger, ULong, or UInt128
42b is-unsigned                 % => true
42u is-unsigned                 % => true
42 is-unsigned                  % => false

% is-float: Real or Double
3.14 is-float                   % => true
3.14d is-float                  % => true
42 is-float                     % => false
```

### Floating-Point Classification

```
1.0 finite?                   % => true
inf inf?                      % => true
nan nan?                      % => true
1.0 normal?                   % => true
0.0 normal?                   % => false (zero is not normal)
```

### Access and Execution Predicates

```
[1 2 3] writable?             % => true (array literals are read-write)
3 array writable?             % => true (allocated arrays are read-write)
(hello) readable?             % => true

% Executable vs literal names
/foo executable?  % => false (literal name)
{ 1 } executable? % => true (executable procedure)
```

### The `type` Operator

`type` replaces the top value with its type name:

```
42 type                         % => /integer-type
3.14d type                      % => /double-type
[1 2] type                      % => /array-type
```

Type names are interned Name objects. The complete set:

```
/null-type /byte-type /integer-type /uinteger-type
/long-type /ulong-type /int128-type /uint128-type
/address-type /real-type /double-type /boolean-type
/operator-type /mark-type /name-type /array-type
/packed-type /string-type /stream-type /dict-type
/curry-type /thunk-type /set-type /tagged-type
/record-type /coroutine-type /pipebuffer-type /cell-type
/continuation-type /opaque-handle-type
```

### Type-Case Dispatch

`type-case` dispatches to a handler based on the operand's type. The value
comes first, then the handler dict; `type-case` consumes both, so a
handler runs with neither on the stack (no leading `pop`):

```
42 << /integer-type  { (integer) }
      /string-type   { (string) }
      /real-type     { (float) }
      /default       { (other) }
   >> type-case
% => (integer)

(hello) << /integer-type  { (integer) }
           /default       { (other) }
        >> type-case
% => (other)
```

`type-case` is equivalent to `exch type case` -- it swaps to put the value on
top, extracts its type name, and dispatches through the dict.

---

## 9. Type Conversion

### Allowed Cast Conversions

`cast` converts between numeric types and Boolean:

| From \ To | Byte | Int | UInt | Long | ULong | Int128 | UInt128 | Real | Double | Bool |
| --------- | ---- | --- | ---- | ---- | ----- | ------ | ------- | ---- | ------ | ---- |
| Byte      | --   | Y   | Y    | Y    | Y     | Y      | Y       | Y    | Y      | Y    |
| Integer   | Y    | --  | Y    | Y    | Y     | Y      | Y       | Y    | Y      | Y    |
| UInteger  | Y    | Y   | --   | Y    | Y     | Y      | Y       | Y    | Y      | Y    |
| Long      | Y    | Y   | Y    | --   | Y     | Y      | Y       | Y    | Y      | Y    |
| ULong     | Y    | Y   | Y    | Y    | --    | Y      | Y       | Y    | Y      | Y    |
| Int128    | Y    | Y   | Y    | Y    | Y     | --     | Y       | Y    | Y      | Y    |
| UInt128   | Y    | Y   | Y    | Y    | Y     | Y      | --      | Y    | Y      | Y    |
| Real      | Y    | Y   | Y    | Y    | Y     | Y      | Y       | --   | Y      | Y    |
| Double    | Y    | Y   | Y    | Y    | Y     | Y      | Y       | Y    | --     | Y    |
| Boolean   | Y    | Y   | Y    | Y    | Y     | Y      | Y       | Y    | Y      | --   |

Non-numeric types (String, Array, Dict, etc.) cannot be cast. There is no
`cast` to or from String -- use `to-string` and `to-number` instead.

### Cast Semantics

- **Integer to float:** Exact if representable, nearest otherwise
- **Float to integer:** Truncates toward zero (like C-style cast)
- **Signed to unsigned:** Bit pattern preserved (e.g., -1 -> UINT_MAX)
- **Wide to narrow:** Truncates (e.g., Long to Integer keeps low 32 bits)
- **To Boolean:** 0 -> false, non-zero -> true
- **From Boolean:** false -> 0, true -> 1

```
3.7 /integer-type cast          % => 3 (truncated)
-3.7 /integer-type cast         % => -3 (truncated toward zero)
-1 /byte-type cast              % => 255b (bit pattern)
true /integer-type cast         % => 1
false /real-type cast           % => 0.0
```

### Reinterpret Semantics

`reinterpret` changes the type tag without converting the value. The bit
pattern is preserved exactly:

```
% Integer 1065353216 has the same bit pattern as Real 1.0
1065353216 /real-type reinterpret  % => 1.0

% Real 1.0 has the same bit pattern as Integer 1065353216
1.0 /integer-type reinterpret     % => 1065353216

% Address from integer
42l /address-type reinterpret     % => address at 0x2A
```

### coerce

`coerce` is the container-level analogue of `cast`: it widens all elements of
an array or packed array, or all values of a dict, to a named numeric or
boolean target type. It is widening-only -- narrowing raises `/type-check`. An
empty array coerces to an empty array; a dict coerces its values (keys are
untouched).

```
[1b 2 3l] /long-type coerce       % => [1l 2l 3l] (all widened to Long)
[1 2 3] /real-type coerce         % => [1.0 2.0 3.0]
<< /a 1 /b 2 >> /long-type coerce % => << /a 1l /b 2l >> (values coerced)
```

### to-string and to-number

```
% to-string:  any rwbuf -- str   (formats `any` into a caller-supplied buffer)
42   32 string to-string        % => (42)
3.14 32 string to-string        % => (3.14)
true 32 string to-string        % => (true)

(42) to-number                  % => 42 (Integer)
(3.14) to-number                % => 3.14 (Real)
(42l) to-number                 % => 42l (Long)
{ (hello) to-number } try       % => /type-check
```

---

## 10. Access Control and Execution

### Access Modes

Objects that hold data (Arrays, Packed Arrays, Strings, Streams, Dicts, Sets)
carry an access control bit:

| Mode      | Can Read? | Can Modify? |
| --------- | --------- | ----------- |
| ReadOnly  | Yes       | No          |
| ReadWrite | Yes       | Yes         |

The verify system checks access before every mutation:

```
% Read-only string -- #r suffix creates read-only
(hello)#r 0 get             % OK: reading
{ (hello)#r 0 65b put } try % /read-only: writing rejected

% Read-only dict -- systemdict is read-only
{ systemdict begin /test 1 def } try  % /read-only

% Writable array
3 array dup 0 42 put            % OK: allocated arrays are read-write

% Make writable array read-only
3 array dup make-readonly       % now read-only
{ 0 42 put } try                % /read-only
```

### Execution Attribute

The execution attribute controls how the interpreter treats a value when it
encounters it on the exec stack:

| Attribute  | Behavior                     |
| ---------- | ---------------------------- |
| Literal    | Push to operand stack (data) |
| Executable | Execute (code)               |

<!-- doctest: skip (literal-vs-executable syntax illustration; \foo requires foo defined) -->
```
% Literal name: pushed as data
/foo                            % pushes the name /foo

% Executable name: looked up and executed
\foo                            % looks up foo in dict stack, executes result

% Literal array: data
[1 2 3]                         % pushed as array

% Executable array: procedure
{ 1 2 add }                     % pushed as executable procedure, runs when called
```

Some types ignore the execution attribute -- they are always pushed as data
regardless: Byte, Integer, UInteger, Long, ULong, Address, Real, Double,
Boolean, Mark, Dict, Set.

### Syntax Modifiers

String literals support modifiers to set attributes:

| Modifier | Effect                                          |
| -------- | ----------------------------------------------- |
| `#r`     | ReadOnly                                        |
| `#w`     | ReadWrite (explicit; default for bare strings)  |
| `#x`     | Executable                                      |
| `#l`     | Literal (explicit; default)                     |
| `#=`     | Temporary storage (for intermediate values)     |
| `#b`     | Byte (single character)                         |
| `#a`     | Byte array (each byte becomes an array element) |

```
(hello)#r                       % read-only string
(hello)#x                       % executable string (runs through scanner)
(r)#b                           % byte value 114 (ASCII 'r')
```

---

## 11. Structural Equality

### eq vs deep-eq

`eq` compares values at the top level. For composites, it compares identity
(same VM offset), not contents -- except String, which `eq` compares
byte-for-byte by content:

```
1 1 eq             % => true (same value)
(hello) (hello) eq % => true (strings compare by content)
[1 2 3] [1 2 3] eq % => false (different array objects)
```

`deep-eq` recursively compares the contents of arrays, packed arrays, dicts,
sets, curry objects, tagged values, thunk objects, and records:

```
[1 2 3] [1 2 3] deep-eq        % => true (same contents)
[1 [2 3]] [1 [2 3]] deep-eq    % => true (recursive)
[1 2 3] [1 2 4] deep-eq        % => false (element 2 differs)
```

### deep-eq Rules

1. Arrays and packed arrays are compared element-by-element recursively
   (an Array can deep-equal a Packed with the same contents)
2. Dicts compare by key/value (same keys, values recursively equal, key
   order independent); Sets compare by membership (order independent)
3. Records compare field-by-field; Tagged values compare tag and payload
4. Curry objects compare both the captured value and the callable
5. Thunk objects compare state, proc, and cached result
6. All other (scalar/identity) types — numbers, names, strings, streams,
   operators — use `eq` semantics
7. Recursion depth is limited to 64 (raises `/limit-check` if exceeded)
8. NaN != NaN even in deep-eq (IEEE 754 semantics)

```
% Array vs Packed: content comparison
save /sv exch def
1 2 3 3 packed                  % packed [1 2 3]
[1 2 3] deep-eq                 % => true
sv restore

% Nested comparison
[1 [2 [3]]] [1 [2 [3]]] deep-eq  % => true
[1 [2 [3]]] [1 [2 [4]]] deep-eq  % => false
```

---

## 12. Immutability

Immutability -- the guarantee that data cannot be modified after creation --
is a core property of Trix's type system. Every type in Trix is either
inherently immutable (value types) or supports explicit immutability via the
ReadOnly access bit (container types). There is no type in Trix where
mutation can occur without the programmer opting into it.

### Value Types Are Inherently Immutable

Numbers, Booleans, Null, Names, Marks, Operators, and Curry
objects are *values*. No Trix operator modifies them in place. When you
"change a variable," you are rebinding a name in a dict to a different
value -- the original value is untouched:

```
/x 42 def                      % bind /x to 42
/x 99 def                      % rebind /x to 99 -- does not mutate 42
```

This distinction is fundamental. The integer `42` has no identity separate
from its value. Two `42` values are indistinguishable. There is no operation
like `set_value(x, 99)` that would change `42` into `99` through a reference.

| Type | Mutable? | Reason |
| --- | --- | --- |
| Null | No | Singleton value |
| Byte | No | Copied on assignment, no in-place mutation operators |
| Integer | No | Copied on assignment, no in-place mutation operators |
| UInteger | No | Copied on assignment, no in-place mutation operators |
| Long | No | ExtValue is cloned when copied between containers |
| ULong | No | ExtValue is cloned when copied between containers |
| Real | No | Copied on assignment, no in-place mutation operators |
| Double | No | ExtValue is cloned when copied between containers |
| Boolean | No | Copied on assignment, no in-place mutation operators |
| Address | No | ExtValue is cloned when copied between containers |
| Operator | No | Function pointer, not modifiable from Trix code |
| Mark | No | Sentinel value, no mutation operators |
| Name | No | Interned and immutable by design |
| Curry | No | Created once, captured value and callable are fixed |
| Thunk | Mutable state | State transitions via `force`/`thunk-reset`; save/restore journals changes |
| Set | ReadOnly or ReadWrite | Like Dict: ReadOnly bit controls mutation; save/restore journals changes |
| Tagged | Always Literal | Pure data; `make-executable` raises type-check. Shares PackedType::Curry (X=0) |

Even the 64-bit ExtValue types (Long, ULong, Double, Address) that are stored
on the VM heap are semantically immutable. Every operation that moves an
ExtValue between containers clones it -- there is no aliasing. Two Objects
cannot point at the same ExtValue and observe mutations through each other.

**Thunk is a special case.** Like containers, a Thunk Object holds a
`vm_offset_t` handle to shared VM storage (3 Objects: state + proc + result).
Copying a Thunk copies the handle, not the storage -- all copies share the
same backing data. When any copy is forced, the cached result is visible
through every copy. This shared-reference semantics is essential for lazy
memoization. Unlike containers, Thunk mutation is not controlled by the
ReadOnly bit -- state transitions happen internally via `force` and
`thunk-reset`. See [VM Internals: Value vs Reference Semantics](vm-internals.md#value-vs-reference-semantics)
for the full storage model.

### Container Types Support Explicit Immutability

The six container types -- Array, Packed, String, Stream, Dict, and Set --
hold references to mutable storage. These types carry a ReadOnly access bit that
controls whether mutation operators (`put`, `def`, `write`, `append`, etc.)
are permitted:

```
% Mutable array: allocated read-write
3 array dup 0 42 put            % OK

% Immutable array: the #r suffix creates read-only
[1 2 3]#r                  % read-only (bare literals are read-write)
{ [1 2 3]#r 0 99 put } try % => /read-only

% Explicitly make an array immutable
3 array dup 0 42 put make-readonly % now read-only
{ 0 99 put } try                   % => /read-only

% Immutable string
(hello)#r                   % #r creates read-only
{ (hello)#r 0 65b put } try % => /read-only

% Immutable dict: the #r suffix creates read-only
<< /a 1 /b 2 >>#r                    % read-only (bare literals are read-write-fixed)
{ << /a 1 >>#r begin /a 99 def } try % => /read-only
```

The ReadOnly bit is enforced by the verify system *before* the mutation
operator executes. The operator never sees the read-only object -- the
`/read-only` error is raised during operand validation.

Packed arrays are always read-only -- the compressed encoding does not support
in-place modification.

### Binding Protection via Dict Access Control

Immutability of variable *bindings* (preventing reassignment) is enforced at
the dict level, not the value level:

```
% systemdict is read-only: built-in names cannot be modified in place
{ systemdict /add 42 put } try  % => /read-only

% User-created read-only dictionary protects all its bindings
<< /pi 3.14159 /e 2.71828 >>#r begin
{ /pi 0 def } try              % => /read-only
end

% Dict modes control binding mutability
% ReadOnly:          no new keys, no value changes
% ReadWriteFixed:    no new keys, values can change
% ReadWriteDynamic:  keys and values can change freely
```

This is the Trix equivalent of `const` in C++ or `let` in Rust. The
protection is on the *dict* that holds the binding, not on the value
itself. A read-only dict with a mutable array value means the binding
cannot be reassigned, but the array contents can be modified if the array
itself is read-write.

### Save/Restore as Transactional Rollback

The save/restore mechanism provides a different form of protection -- most VM
modifications since a save point can be rolled back:

```
save /sv exch def
/x 42 def
3 array dup 0 99 put
% ... modifications ...
sv restore                      % array and dict changes are undone
```

Save/restore journals modifications to **arrays** and **dicts** (and
packed array name bindings). When `restore` is called, all journaled
mutations are replayed in reverse, returning those containers to their
pre-save state. VM heap allocations made after the save point are discarded.

**String exception:** String byte modifications via `put` are **not
journaled** by save/restore. Changes to string contents persist across a
restore. This is the only container type with this behavior:

| Container  | Journaled by save/restore?                                   |
| ---------- | ------------------------------------------------------------ |
| Array      | Yes -- element writes are journaled                          |
| Dict       | Yes -- entry writes and structural changes are journaled     |
| Packed     | Yes -- name bindings are journaled                           |
| Stream     | Partially -- streams opened after save are closed on restore |
| Set        | Yes -- key insertions and removals are journaled             |
| Tagged     | Yes -- VM storage tracked like Curry                         |
| **String** | **No -- byte writes are not journaled**                      |

This is a deliberate design trade-off, not an oversight. A string is a
compact byte sequence -- each element is a single byte stored directly in
VM memory. Journaling each byte write would require an 8+ byte journal entry
per modified byte, making single-byte `put` operations many times more
expensive and consuming journal space disproportionate to the data being
protected. Arrays do not have this problem because each element is an 8-byte
Object -- the journal entry and the data it protects are the same size.

**Workaround -- array of bytes:** When transactional rollback of byte-level
data is required, use an array of Byte values instead of a string. Array
elements are fully journaled by save/restore:

```
save /sv exch def
% Array of bytes: fully journaled
[ 72b 101b 108b 108b 111b ] % equivalent to (Hello)
dup 0 65b put               % modify element 0
pop                         % drop the array before restoring
sv restore                  % modification is rolled back
```

Alternatively, allocate the string after the save point so it is discarded
entirely when the VM heap is reset:

```
save /sv exch def
5 string dup 0 65b put          % allocated after save
pop                             % drop the string before restoring
sv restore                      % string is discarded with the heap
```

In this sense, strings are a more memory-efficient representation of byte
sequences that trades save/restore journaling for compactness. An array of
N Byte values consumes 8N bytes (one Object per element); a string of N
bytes consumes N bytes. For most use cases -- parsing, formatting, I/O
buffers -- the compactness advantage far outweighs the journaling limitation.

### The Complete Immutability Picture

Trix's immutability model has three layers:

| Layer | Mechanism | Scope |
| --- | --- | --- |
| **Value immutability** | Inherent (no mutation operators) | All value types: numbers, booleans, names, etc. |
| **Container immutability** | ReadOnly access bit | Arrays, packed arrays, strings, streams, dicts, sets |
| **Binding immutability** | Dict access mode | Name-to-value mappings in any dict |
| **Transactional rollback** | Save/restore | Arrays, dicts, packed name bindings, heap allocations (not string bytes) |

These layers compose. A read-only dict containing read-only arrays of
integers is deeply immutable at every level -- the bindings cannot change, the
array contents cannot change, and the integer values are inherently immutable.

### Comparison with Other Language Approaches

| Language | Value immutability | Container immutability | Binding immutability |
| --- | --- | --- | --- |
| **Trix** | Inherent for all value types | ReadOnly bit on containers | Dict access modes |
| **Python** | Inherent (int, str, tuple) | No (`list`, `dict` always mutable) | No (`const` does not exist) |
| **Rust** | All values immutable by default | `let` vs `let mut` | `let` vs `let mut` |
| **JavaScript** | Inherent (primitives) | `Object.freeze` (shallow) | `const` (binding only) |
| **Ruby** | Inherent (Integer, Symbol) | `.freeze` (runtime mutation) | No built-in mechanism |
| **Lua** | Inherent (number, string) | No (tables always mutable) | No (`local` is mutable) |

Trix's approach is closest to Rust's: value types are inherently immutable,
containers opt into mutability explicitly, and bindings are protected by their
enclosing scope (dict). The key difference is that Trix enforces
container immutability at runtime via the verify system, while Rust enforces
it at compile time via the borrow checker.

---

## 13. Design Decisions

### Why 8 Bytes?

The 8-byte Object is the fundamental design choice that everything else builds
on:

- **Cache line friendly.** Four Objects fit in a 32-byte cache half-line.
  The operand stack is a contiguous array of Objects, maximizing cache
  utilization.
- **No boxing.** In Python, every integer is a heap-allocated object with
  reference count, type pointer, and value -- 28 bytes minimum. In Trix, an
  integer is 8 bytes total, inline, no allocation.
- **Uniform size.** All values are the same size. Arrays are contiguous
  sequences of Objects. Stack operations are simple pointer arithmetic.
  No variant types, no union discriminators, no pointer chasing.

### Why No Implicit Coercion?

Implicit coercion is the single largest source of type-related bugs across
all scripting languages:

- **Python:** `"10" * 3` returns `"101010"` (string repetition).
  `True + True` returns `2`.
- **JavaScript:** `"5" - 3` returns `2` but `"5" + 3` returns `"53"`.
- **Lua:** `"3" + 2` returns `5.0` (string to number coercion).
- **Ruby:** `nil + 1` raises `NoMethodError` (not a type error).

Trix eliminates all of these by requiring every type conversion to be explicit.
The cost is that you must write `promote` before mixed-type arithmetic. The
benefit is that type errors are caught immediately, not silently propagated.

### Why Bitmask Verification?

The verify system uses a 64-bit bitmask rather than a function call or type
hierarchy because:

- **One instruction per check.** `(mask & (1 << type)) != 0` compiles to a
  single `BT` (bit test) instruction on x86.
- **Composable.** `VerifyNumbers | VerifyNotZero | VerifyIsFinite` combines
  type acceptance with value constraints in a single mask. No need for
  separate type check + range check + finiteness check.
- **Self-documenting.** The verify mask is the operator's type signature. It
  can be printed as a human-readable description (e.g., "Numbers", "RW|Dict",
  "X|Array|Packed").
- **Extensible.** Adding a new constraint flag costs one bit. Adding a new
  type costs one bit (the 5-bit type field holds up to 32 types; 31 are
  currently defined, of which 30 are user-visible).

### Why Seven Integer Types?

Most scripting languages have one integer type. Trix has seven because:

- **Embedded use.** Byte is essential for I/O, protocol parsing, and
  memory-mapped hardware registers.
- **FFI accuracy.** When interfacing with C/C++ libraries, the caller needs
  to match the exact integer type of the API. A 32-bit `int` and a 64-bit
  `long` are not interchangeable.
- **Overflow semantics.** Unsigned overflow (wrapping) and signed overflow
  (undefined in C, error in Trix) have different behaviors. Having separate
  types makes the overflow behavior explicit.
- **Memory efficiency.** A Byte uses 8 bytes total (Object). An Integer uses
  8 bytes. A Long uses 16 bytes (Object + ExtValue). When you know a value
  fits in 32 bits, using Integer instead of Long saves 50% memory.
- **Wide values.** Int128 and UInt128 (128-bit, WideValue storage) cover
  crypto/hash literals, UUIDs, Snowflake IDs, and nanosecond timestamps with
  decade spans -- ranges that overflow the 64-bit types.

### Why Both Real and Double?

- **Memory efficiency.** Real stores inline (no heap allocation). Double
  requires an ExtValue (8 bytes on heap). For large arrays of floating-point
  data, Real uses roughly half the memory.
- **Precision control.** When 32-bit precision suffices (graphics, audio,
  sensor data), using Real avoids unnecessary heap allocation and provides
  better cache behavior.
- **No silent precision change.** In Trix, `1.0` is always 32-bit and `1.0d`
  is always 64-bit. There is no hidden promotion to double when you did not
  ask for it.

### Why Separate eq and deep-eq?

`eq` is a shallow, O(1) comparison: same type, same value (or same identity
for composites). `deep-eq` is a recursive, O(n) comparison that descends
into arrays and dicts.

Combining them (like Python's `==`) would make equality comparison O(n) for
all values, including cases where identity comparison is the correct semantic
(e.g., checking if two variables reference the same array). Separating them
lets the programmer choose the right semantics for each use case.

---

