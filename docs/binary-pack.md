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

# Binary Pack/Unpack: Structured Binary Data in Trix

Updated: 2026-03-28

Copyright 2026 Mark Guidarelli

---

## 1. Overview

Trix provides three operators for structured binary data encoding and decoding:

| Operator    | Stack Effect                     | Purpose                             |
| ----------- | -------------------------------- | ----------------------------------- |
| `pack`      | `mark v1 ... vN fmt-str -- str`  | Encode values into a binary string  |
| `unpack`    | `str fmt-str -- v1 ... vN count` | Decode a binary string into values  |
| `pack-size` | `fmt-str -- int`                 | Compute the byte count for a format |

These operators convert between Trix's typed value system and raw byte sequences
using a compact format string that describes the binary layout.  The format string
specifies type widths, signedness, byte order, repeat counts, padding, and
fixed-length string fields.

### 1.1 Why Binary Pack/Unpack?

Embedded scripting languages live at the boundary between structured software and
raw hardware.  Configuration registers, network protocol headers, sensor readings,
file format fields, and shared-memory structures are all sequences of bytes with
defined layouts.  Without pack/unpack, working with binary data requires tedious
manual byte arithmetic:

```
% Without pack: read a big-endian 32-bit unsigned integer from a 4-byte string
/buf 4 string def
%  ... fill buf from hardware/file ...
buf 0 get /uinteger-type cast 24 shift-left
buf 1 get /uinteger-type cast 16 shift-left or
buf 2 get /uinteger-type cast 8 shift-left or
buf 3 get /uinteger-type cast or
% 14 operators of byte plumbing (get returns a Byte, which shifts at
% byte width -- each one must be cast up first), easy to get wrong
```

With pack/unpack, the same operation is declarative:

```
buf (>I) unpack pop
% 1 operator, intent is clear, endianness is explicit
```

This is not a convenience feature.  In Trix's target domain -- embedded systems,
protocol handlers, binary file processing, hardware interfaces -- binary data
handling is a **primary use case**, not an edge case.

---

## 2. Format String Specification

The format string is a sequence of **endianness prefixes** and **type specifiers**,
optionally preceded by **repeat counts**.

### 2.1 Endianness Prefixes

| Prefix | Meaning       | Byte Order                                       |
| ------ | ------------- | ------------------------------------------------ |
| `>`    | Big-endian    | Most significant byte first (network byte order) |
| `<`    | Little-endian | Least significant byte first                     |
| `=`    | Native        | Platform byte order (ARM64: little-endian)       |

Endianness prefixes are **sticky**: they apply to all following specifiers until
another prefix appears.  If no prefix is given, **native** byte order is used.

```
(>HI)      % big-endian uint16 + big-endian uint32
(<HI)      % little-endian uint16 + little-endian uint32
(>H<I)     % big-endian uint16, then little-endian uint32
(HI)       % native endian for both (equivalent to =HI)
```

### 2.2 Type Specifiers

| Specifier | Width    | Trix Pack Type                            | Trix Unpack Type | C Equivalent        |
| --------- | -------- | ----------------------------------------- | ---------------- | ------------------- |
| `b`       | 1 byte   | Integer (range-checked -128..127)         | Integer          | `int8_t`            |
| `B`       | 1 byte   | Byte/Integer (range-checked 0..255)       | Byte             | `uint8_t`           |
| `h`       | 2 bytes  | Integer (range-checked -32768..32767)     | Integer          | `int16_t`           |
| `H`       | 2 bytes  | Integer/UInteger (range-checked 0..65535) | UInteger         | `uint16_t`          |
| `i`       | 4 bytes  | Integer                                   | Integer          | `int32_t`           |
| `I`       | 4 bytes  | UInteger                                  | UInteger         | `uint32_t`          |
| `l`       | 8 bytes  | Long                                      | Long             | `int64_t`           |
| `L`       | 8 bytes  | ULong                                     | ULong            | `uint64_t`          |
| `q`       | 16 bytes | Int128 (any narrower integer widened)     | Int128           | `__int128`          |
| `Q`       | 16 bytes | UInt128 (any narrower integer widened)    | UInt128          | `unsigned __int128` |
| `f`       | 4 bytes  | Real (or any numeric, cast to float)      | Real             | `float`             |
| `d`       | 8 bytes  | Double (or any numeric, cast to double)   | Double           | `double`            |
| `x`       | 1 byte   | *(none -- padding)*                       | *(skipped)*      | padding             |
| `Ns`      | N bytes  | String (truncated or zero-padded to N)    | String           | `char[N]`           |

**Case convention:** Lowercase specifiers (`b`, `h`, `i`, `l`) are **signed**.
Uppercase specifiers (`B`, `H`, `I`, `L`) are **unsigned**.  This mirrors the
convention used by Python's `struct` module and Lua's `string.pack`.

### 2.3 Repeat Counts

A decimal number before a specifier repeats it:

```
(4B)       % four uint8 values (4 bytes total)
(3i)       % three int32 values (12 bytes total)
(2d)       % two doubles (16 bytes total)
(3x)       % three padding bytes
(10s)      % one 10-byte string
```

For the `s` specifier, the count is the **byte length** of the string field, not
a repetition.  `10s` means one 10-byte string, not ten 1-byte strings.

### 2.4 Padding (`x`)

The `x` specifier inserts a zero byte during pack and skips a byte during unpack.
It consumes no values from the argument list and produces no values during unpack.

```
mark 42 99 (BxB) pack % 3 bytes: [42] [0] [99]
dup (BxB) unpack      % pushes: 42 99 2 (count=2, padding skipped)
```

Padding is useful for aligning fields in binary structures or skipping reserved
bytes in protocol headers.

---

## 3. Operator Reference

### 3.1 `pack` -- Encode Values to Binary

**Stack effect:** `mark v1 v2 ... vN fmt-str -- str`

Reads N values between the mark and the format string, encodes them according to
the format specifiers, and pushes a binary string.

**Arguments:**
- Values are consumed from the mark upward (left to right)
- The mark is consumed
- The format string is consumed
- One result string is pushed

**Errors:**
- `unmatched-mark` -- no mark on operand stack
- `range-check` -- argument count mismatch, or value out of range for specifier
- `type-check` -- value type incompatible with specifier
- `invalid-format-string` -- unknown specifier or malformed format
- `vm-full` -- insufficient VM memory for result string

**Example:**
```
mark 16#CAFE#u 42 42 3.14d (>HIBd) pack
% Result: 15-byte string (2+4+1+8)
% Bytes: CA FE 00 00 00 2A 2A [8 bytes of 3.14 as BE double]
```

### 3.2 `unpack` -- Decode Binary to Values

**Stack effect:** `str fmt-str -- v1 v2 ... vN count`

Reads bytes from the string according to the format, pushes the decoded values,
then pushes an integer count of values produced.

**Arguments:**
- The input string is consumed
- The format string is consumed
- N decoded values are pushed (types determined by specifiers)
- An integer count is pushed on top

**Errors:**
- `range-check` -- input string shorter than format requires
- `invalid-format-string` -- unknown specifier or malformed format

**Example:**
```
some_binary_string (>HIBd) unpack
% Stack: uint16_value uint32_value byte_value double_value 4
% Top of stack is count (4)
```

**String specifier (`Ns`) in unpack:** Allocates a new Trix string of length N
in VM memory and copies N bytes from the input.

### 3.3 `pack-size` -- Query Format Size

**Stack effect:** `fmt-str -- int`

Returns the total number of bytes the format would produce, without packing any
values.  Useful for pre-allocating buffers or validating input lengths.

**Example:**
```
(>HIBd) pack-size  % pushes 15
(4B3x2i) pack-size % pushes 15 (4+3+8)
() pack-size       % pushes 0
```

---

## 4. Type Coercion Rules

Pack accepts values that are **wider** than the target specifier and coerces them
with range checking.  This allows natural Trix idioms without explicit casting.

### 4.1 Integer Specifiers (`b`, `B`, `h`, `H`, `i`, `I`, `l`, `L`, `q`, `Q`)

**Accepted input types:** Byte, Integer, UInteger, Long, ULong (and, for the
128-bit `q` / `Q` specifiers only, Int128 and UInt128).

**Signed specifiers** (`b`, `h`, `i`, `l`): Extract a signed 64-bit value from
the input, then range-check against the target width.  Out-of-range values raise
`range-check`.

**Unsigned specifiers** (`B`, `H`, `I`, `L`): Extract an unsigned 64-bit value.
Negative inputs raise `range-check`.  Values exceeding the target width raise
`range-check`.

**128-bit specifiers** (`q` signed / `Q` unsigned): Extract a 128-bit value,
widening any narrower integer input.  `q` carries the full `__int128` range, so
no narrowing check applies; `Q` raises `range-check` on a negative input.  The
64-bit specifiers above **reject** Int128 / UInt128 inputs with `type-check` — a
128-bit value must be packed with `q` / `Q`.  Unpacking `q` yields an Int128,
`Q` a UInt128.

```
mark 42 (B) pack       % OK: Integer 42 fits in uint8
mark 42b (i) pack      % OK: Byte 42 widened to int32
mark 256 (B) pack      % ERROR: range-check (256 > 255)
mark -1 (B) pack       % ERROR: range-check (negative for unsigned)
mark 42l (B) pack      % OK: Long 42 narrowed to uint8 (in range)
```

### 4.2 Float Specifiers (`f`, `d`)

**Accepted input types:** Byte, Integer, UInteger, Long, ULong, Real, Double.

All inputs are cast to `double` first, then narrowed to `float` for `f`.  This
allows packing integer values as floating-point without explicit conversion.

```
mark 42 (f) pack       % OK: Integer 42 -> float 42.0
mark 3.14 (d) pack     % OK: Real 3.14 -> double 3.14
mark 42 (d) pack       % OK: Integer 42 -> double 42.0
```

### 4.3 String Specifier (`Ns`)

**Accepted input type:** String only.

- If the input string is **shorter** than N bytes, the field is **zero-padded**
- If the input string is **longer** than N bytes, it is **truncated** to N bytes
- Exactly N bytes are always written

```
mark (Hi) (5s) pack          % 5 bytes: 'H' 'i' 0 0 0
mark (Hello World) (5s) pack % 5 bytes: 'H' 'e' 'l' 'l' 'o'
```

### 4.4 Rejected Types

Array, Dict, Packed, Stream, Operator, Mark, Curry, Thunk, Null, Boolean,
Name, and Address cannot be packed.  Attempting to do so raises `type-check`.

---

## 5. Design Philosophy and Trade-offs

### 5.1 Format Strings vs. Operator-Per-Type

An alternative design would provide individual operators: `pack-uint16-be`,
`pack-int32-le`, etc.  Trix chose format strings because:

- **Composability:** One call handles an entire structure, not one field at a time
- **Readability:** `(>HIBd)` is a visual schema of the binary layout
- **Parity:** Matches Lua and Python conventions that users already know
- **Compactness:** One operator + format string vs. N operators + N endian args

The trade-off is a runtime-parsed format string.  For hot loops, the format is
re-parsed on every call.  This is acceptable because:

1. Pack/unpack operations are typically I/O-bound (reading from streams, hardware)
2. The format parser is a single linear scan with no allocation
3. A future optimization (format pre-compilation) could eliminate re-parsing

### 5.2 Mark-Based Arguments for Pack

Pack uses the mark-based calling convention (`mark v1 v2 ... vN fmt-str pack`)
rather than an array-based convention (`[v1 v2 ... vN] fmt-str pack`) because:

- **Zero allocation:** No intermediate array is created
- **Consistency:** Matches `print-fmt` / `sprint-fmt` calling convention
- **Flexibility:** Values can be computed inline without building an array

The count of values is validated against the format at runtime.  A mismatch
raises `range-check` with a clear error message.

### 5.3 Unpack Pushes Values, Not an Array

Unpack pushes individual values followed by a count, rather than pushing a
single array.  This design:

- **Avoids allocation:** No array object created in VM
- **Enables direct use:** Values are immediately available on the stack
- **Matches scanf semantics:** The count tells the caller how many values to expect

The count is always the last value pushed (top of stack), making it easy to
verify or discard: `unpack pop` drops the count, leaving just the values.

### 5.4 Range Checking

Pack performs strict range checking for narrowing conversions.  Packing 256 as
`B` (uint8) raises `range-check` rather than silently truncating.  This prevents
silent data corruption in binary protocols where an out-of-range value could
cause hardware damage or protocol violations.

The check happens at the Trix level, not the C++ level, producing actionable
error messages: `"pack 'B': value 256 out of uint8 range"`.

### 5.5 Endianness as Format Metadata

Endianness is specified **in the format string** rather than as a separate argument.
This allows mixed-endian structures (common in protocol bridges) and makes the
byte order part of the schema documentation:

```
(>H<I)    % big-endian header length + little-endian payload offset
```

The `=` prefix explicitly selects native byte order, which is useful when the
format is used with memory-mapped structures on the local platform.

---

## 6. Implementation Details

### 6.1 Architecture

```
pack_op / unpack_op / packsize_op
        |
        v
  parse_pack_format<Visitor>    -- shared template, walks format string
        |
        v
  pack_extract_signed()         -- coerce any integer type to int64
  pack_extract_unsigned()       -- coerce any integer type to uint64
  pack_extract_double()         -- coerce any numeric type to double
        |
        v
  pack_maybe_swap<T>()          -- conditional byte-swap
        |
        v
  byteswap_helper<T>()          -- existing Trix utility (std::byteswap + bit_cast)
```

### 6.2 Format String Parser

`parse_pack_format<Visitor>` is a template function that walks the format string
once and calls a visitor lambda for each specifier.  The visitor receives:

- `char ch` -- the specifier character
- `length_t repeat` -- repeat count (1 if not specified; for `s`, this is byte length)
- `std::endian endian` -- current endianness state
- `length_t spec_size` -- bytes per element for this specifier

The parser handles endianness prefix tracking, repeat count parsing, specifier
size lookup, and total byte accumulation.  It raises `invalid-format-string` for
unknown specifiers and dangling repeat counts.

All three operators (`pack`, `unpack`, `pack-size`) share this parser.  `pack-size`
passes an empty visitor.  `pack` and `unpack` each make two passes:

- **Pass 1:** Count expected arguments (or results) and total byte size
- **Pass 2:** Read/write the actual data

### 6.3 Memory Allocation

**Pack:** Uses `vm_alloc<vm_t>(total_bytes + 1)` -- a single fixed-size allocation
determined by pass 1.  No streaming allocation, no reallocation.  The result
string is NUL-terminated for C interoperability.

**Unpack:** Pushes results directly onto the operand stack.  No intermediate
storage.  String specifiers (`Ns`) allocate individual string objects via
`vm_alloc`.  `require_op_capacity()` is called before pushing to ensure the
operand stack has room.

**Pack-size:** No allocation.  Walks the format string and returns a count.

### 6.4 Byte Order Conversion

Byte swapping uses the existing `byteswap_helper<T>()` template, which handles:

- **1-byte types:** No-op (byte order is irrelevant)
- **Integer types:** `std::byteswap()` (C++23)
- **Float types:** `std::bit_cast` to unsigned integer, `std::byteswap`, `std::bit_cast` back

The `pack_maybe_swap<T>()` wrapper adds the endianness check: if the target
endianness matches native, no swap is performed.

### 6.5 Type Extraction

Three helper functions extract values from Trix Objects for packing:

- `pack_extract_signed(trx, arg, spec)` -- returns `int64_t` from any integer type
- `pack_extract_unsigned(trx, arg, spec)` -- returns `uint64_t` from any integer type; rejects negatives
- `pack_extract_double(trx, arg, spec)` -- returns `double` from any numeric type

Each helper dispatches on `arg->type()` with exhaustive `switch` coverage
(required by `-Wswitch-enum`).  Non-matching types raise `type-check` with the
specifier character and actual type name in the error message.

---

## 7. Working Examples

### 7.1 Network Protocol Header

Pack and unpack a simplified TCP-like header:

```
% Pack: src_port(16) dst_port(16) seq_num(32) flags(8)
/src_port 8080u def
/dst_port 80u def
/seq_num 12345u def
/flags 2 def                    % SYN flag

mark src_port dst_port seq_num flags (>HHIb) pack
/header exch def

% Unpack
header (>HHIb) unpack pop
/flags exch def
/seq_num exch def
/dst_port exch def
/src_port exch def
```

### 7.2 Sensor Data Record

A sensor sends 12-byte records: timestamp(32-bit LE) + temperature(float LE) +
status(32-bit LE flags):

```
% Simulate a sensor record
mark 1711648000u 23.5 16#00000003#u (<IfI) pack
/record exch def

% Parse the record
record (<IfI) unpack pop
/status exch def
/temp exch def
/timestamp exch def

% Display
(\{timestamp}) print ( ) print
(\{temp}) print ( C, status=) print
(\{status}) =
```

### 7.3 Binary File Header (PNG-like)

```
% PNG signature: 8 bytes
mark 137 80 78 71 13 10 26 10 (8B) pack
/png_sig exch def

% IHDR chunk header: length(32 BE) + type(4s)
mark 13u (IHDR) (>I4s) pack
/ihdr_header exch def

% Verify the signature. unpack leaves the values on the stack in pack order
% (first byte deepest) plus a trailing count; collect them into an array via
% mark ... ] and compare. Unpacked B fields come back as byte values (137b).
mark png_sig (8B) unpack pop ] /sig-bytes exch def
sig-bytes 0 get (PNG byte 0) exch 137b eq assert
sig-bytes [137b 80b 78b 71b 13b 10b 26b 10b] deep-eq (PNG signature) exch assert
```

### 7.4 Mixed-Endian Protocol Bridge

Some protocols bridge big-endian network headers with little-endian payloads:

```
% Header (big-endian): msg_type(16) + payload_len(16)
% Payload (little-endian): sensor_id(32) + value(double)
mark 1u 12u 42u 98.6d (>HH<Id) pack
/packet exch def

% Parse
packet (>HH<Id) unpack pop
/value exch def
/sensor_id exch def
/payload_len exch def
/msg_type exch def
```

### 7.5 Pre-computing Buffer Sizes

```
/fmt (>3I2d4B) def
fmt pack-size               % 32 = 3*4 + 2*8 + 4*1
/buf exch string def        % pre-allocate exact buffer
```

### 7.6 Iterating Over a Binary Array

Unpack a sequence of 100 uint16 values from a binary blob:

```
/blob 200 string def
%  ... fill blob from I/O ...

% Unpack all 100 values
blob (100H) unpack pop
% Stack now has 100 UInteger values
```

---

## 8. Limitations

### 8.1 No Format Pre-compilation

The format string is re-parsed on every call.  For hot loops packing the same
format repeatedly, this adds overhead.  A future `compile-format` operator could
return a compiled format object that `pack`/`unpack` accept directly.  This is
not implemented because:

- Format parsing is a simple linear scan (no allocation, no backtracking)
- Pack/unpack calls are typically I/O-bound, not CPU-bound
- Premature optimization for a feature with no measured bottleneck

### 8.2 No Alignment or Struct Padding

The format string packs fields contiguously.  There is no automatic alignment
to natural boundaries (e.g., 4-byte alignment for `I`).  Use explicit `x`
padding to match C struct layouts that include padding:

```
% C struct: { uint8_t a; uint32_t b; } with 3 bytes padding
(B3xI)     % matches the C layout on most platforms
```

### 8.3 No Variable-Length Fields

All field sizes are fixed at format-parse time.  There is no "length-prefixed
string" specifier.  To handle variable-length data, unpack in stages:

<!-- doctest: skip (synopsis: data is a stand-in packed string) -->
```
% Read a length-prefixed string: uint16 length + N bytes
data (>H) unpack pop /len exch def
data 2 len get-interval    % extract the string bytes
```

### 8.4 No Bit-Level Packing

The minimum unit is one byte.  Bit fields (common in hardware registers and
protocol flags) are handled after unpacking using the `bit?`, `bit-set`,
`bit-clear`, and `bit-toggle` operators:

<!-- doctest: skip (snippet: assumes a packed byte on the stack) -->
```
/flags_byte exch def
flags_byte 0 bit?                % test bit 0
flags_byte 3 bit?                % test bit 3
flags_byte 4 bit-set 1 bit-clear % set bit 4, clear bit 1
```

### 8.5 Repeat Count Range

Repeat counts are stored as `length_t` (uint16_t), limiting them to 65535.  This
is sufficient for any practical binary structure.

---

## 9. Error Reference

| Error                   | Condition                        | Example                                     |
| ----------------------- | -------------------------------- | ------------------------------------------- |
| `unmatched-mark`        | `pack`: no mark on operand stack | `42 (B) pack` (missing `mark`)              |
| `range-check`           | Value out of range for specifier | `mark 256 (B) pack`                         |
| `range-check`           | Argument count mismatch          | `mark 1 2 (B) pack` (2 values, 1 specifier) |
| `range-check`           | Unpack string too short          | `2 string (I) unpack` (need 4 bytes)        |
| `type-check`            | Incompatible value type          | `mark (hello) (i) pack`                     |
| `invalid-format-string` | Unknown specifier character      | `mark 42 (z) pack`                          |
| `invalid-format-string` | Repeat count without specifier   | `(42) pack-size`                            |

---

## 10. Quick Reference Card

```
Format:  [>|<|=] [count] specifier ...

Specifiers:
  b/B   int8 / uint8         (1 byte)
  h/H   int16 / uint16       (2 bytes)
  i/I   int32 / uint32       (4 bytes)
  l/L   int64 / uint64       (8 bytes)
  q/Q   int128 / uint128     (16 bytes)
  f     float32               (4 bytes)
  d     float64               (8 bytes)
  x     padding               (1 byte, no value)
  Ns    string                (N bytes, zero-padded/truncated)

Endianness (sticky):
  >     big-endian (network byte order)
  <     little-endian
  =     native (default)

Operators:
  mark v1 ... vN fmt pack    -- encode values to binary string
  str fmt unpack              -- decode binary string to v1 ... vN count
  fmt pack-size               -- query byte count for format
```
