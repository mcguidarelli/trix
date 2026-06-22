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

# Trix Collections — Technical Reference

## 1. Collection Type Landscape

Trix provides seven collection types, each optimized for a different data
pattern. Together with two supporting types (Curry and Thunk), they form a
164-operator collection system that exceeds both Lua and Python in built-in
coverage.

| Type   | Mutability | Access        | Schema    | Memory/element | Use case              |
| ------ | ---------- | ------------- | --------- | -------------- | --------------------- |
| Array  | read-write | positional    | none      | 8 bytes        | ordered sequences     |
| Packed | read-only  | positional    | none      | 1-7 bytes      | compressed procedures |
| String | read-write | positional    | none      | 1 byte         | text, binary data     |
| Dict   | read-write | by key        | dynamic   | 20 bytes       | maps, namespaces      |
| Set    | read-write | membership    | dynamic   | 12 bytes       | unique collections    |
| Tagged | immutable  | by tag name   | fixed (1) | 16 bytes       | discriminated unions  |
| Record | immutable  | by field name | fixed (N) | 4 + N*8 bytes  | structured data       |

**Lazy sequences** are not a separate type — they use 2-element Arrays with
Thunks. 48 operators provide construction, transformation, and consumption.

### 1.1 Design Philosophy

Each collection type has a clear role:

- **Array/Packed**: ordered data with positional access. Packed is the
  compressed form for procedure bodies and data that doesn't need mutation.
- **String**: byte-level data with text operations. Not Unicode-aware at the
  type level (bytes, not codepoints), but supports UTF-8 encoding/decoding.
- **Dict**: dynamic key-value mapping with hash-based O(1) lookup. The
  workhorse for configuration, namespaces, and associative data.
- **Set**: Dict without values — membership testing, uniqueness, set algebra.
  12-byte entries vs Dict's 20 bytes.
- **Tagged**: discriminated unions (sum types). One tag name + one payload
  value. Pattern dispatch via tag-match.
- **Record**: product types. Fixed schema of named fields, immutable values.
  The complement to Tagged for algebraic data types.

### 1.2 Type Relationships

```
                    Collection Types
                    ================

  Ordered sequences          Named access            Algebraic types
  +--------+--------+     +------+------+          +--------+--------+
  | Array  | Packed |     | Dict | Set  |          | Tagged | Record |
  | (R/W)  | (R/O)  |     | (R/W)| (R/W)|          | (sum)  | (prod) |
  +--------+--------+     +------+------+          +--------+--------+
       \       |               |      |                 |        |
        \      |               |      |                 |        |
    Lazy Sequences         Namespaces              Algebraic Data Types
    (Array + Thunk)        (Dict stack)           (Tagged + Record)
```

**Key compositions:**
- Array + Thunk = Lazy sequences (infinite, memoized)
- Tagged + Record = Algebraic data types (Rust enum-like)
- Record + Lazy = Streaming record pipelines
- Dict + for-all = Iteration (arbitrary order)
- Record + for-all = Iteration (field declaration order)

## 2. Shared Interface — Polymorphic Operators

Several operators dispatch by type, providing a consistent interface:

| Operator | Array | Packed | String | Dict | Set | Record |
| --- | --- | --- | --- | --- | --- | --- |
| `get` | int -> elem | int -> elem | int -> byte | key -> val | -- | name -> val |
| `length` | count | count | count | entry count | member count | field count |
| `for-all` | elem | elem | byte | key val | member | name val |
| `deep-eq` | recursive | recursive | bytewise | recursive | member-wise | schema + fields |
| `copy` | array -> array | -- | string -> string | dict -> dict | set -> set | -- |
| `put` | int val -> | -- | int byte -> | key val -> | -- | -- (use record-update) |
| `type` | array-type | packed-type | string-type | dict-type | set-type | record-type |

**Design note:** `get` takes different key types depending on the container:
integers for Array/Packed/String, any hashable key for Dict, and Name for
Record. Record's `get` errors on missing field (schema is fixed and known).

### 2.1 Access Control

Four types support ReadOnly/ReadWrite access:

```
[1 2 3]       % read-write array
[1 2 3]#r     % read-only array
(hello)#r     % read-only string
<< /a 1 >>#r  % read-only dict
{{ /a /b }}#r % read-only set
```

Records are always ReadOnly (immutable by design). Tagged values have no
access control (the payload is accessed only via untag/tag-value).

### 2.2 for-all Dispatch

`for-all` adapts its behavior per container type:

| Container | Pushed per iteration | Order                     |
| --------- | -------------------- | ------------------------- |
| Array     | element              | positional (0, 1, 2, ...) |
| Packed    | element              | positional                |
| String    | byte value           | positional                |
| Dict      | key, value           | hash order (arbitrary)    |
| Set       | member               | hash order (arbitrary)    |
| Record    | name, value          | field declaration order   |

Dict and Record both push key/name + value pairs. Set pushes members only.
Array/Packed/String push elements only.

### 2.3 deep-eq Semantics

`deep-eq` recursively compares:

- **Array/Packed**: element-by-element, cross-type (Array can deep-eq Packed)
- **Dict**: same keys, values recursively equal (key order independent)
- **Set**: all members present in both (order-independent)
- **Tagged**: tag name + payload recursively
- **Record**: same schema (field names in same order) + all fields recursively
- **Curry**: both captured value and callable recursively
- **Thunk**: state + proc + result recursively

## 3. Per-Type Deep Dives

### 3.1 Array (70+ operators)

**Creation:**
```
[1 2 3]      % literal
5 array      % empty, length 5
[1 2 3]#r    % read-only
mark 1 2 3 ] % from mark
```

**Access and mutation:**
```
arr 0 get                  % first element
arr 2 42 put               % set element 2
arr 1 3 get-interval       % index 1, count 3 -> 3-element sub-array
arr 0 [10 20] put-interval % replace range
```

**Higher-order operations (18 operators):**
```
[1 2 3] { 2 mul } map              % => [2 4 6]
[1 2 3 4] { 2 mod 0 eq } filter    % => [2 4]
[1 2 3] 0 { add } reduce           % => 6
[1 2 3] { 10 gt } any              % => false
[1 2 3] { dup mul } map-indexed    % index available
[1 2 3 4] { 2 mod 0 eq } partition % => [2 4] [1 3]
[3 1 2] { } sort-by                % => [1 2 3]
```

**Aggregation (7 operators):**
```
[1 2 3] sum                    % => 6
[1 2 3] product                % => 6
[1.0 1e10 1.0 -1e10] kahan-sum % => 2.0 (compensated)
[1 2 3] [4 5 6] dot-product    % => 32
[1 1 2 2 2 3] frequencies      % => << 1 2  2 3  3 1 >>
```

**VM layout:** Contiguous Object array in heap. `m_array` (vm_offset_t) +
`m_arrays_length` (length_t) in the 8-byte Object header.

**Cost:** O(1) access by index. O(N) for map/filter/sort (new array each time).
No in-place resize — append allocates a new array.

**When to use:** Ordered data, numeric computation, pipeline transforms, any
situation where positional access matters.

### 3.2 Packed (compressed arrays)

Packed arrays use variable-width encoding (1-7 bytes per element vs Array's
fixed 8 bytes). ~32 PackedType codes cover all Object types.

**Creation:**
```
{ dup mul add }  % procedure literal (auto-packed)
3 4 5 3 packed   % data packed from stack
{ add } bind     % bind resolves operator names inside the proc to direct refs
```

**Key properties:**
- Read-only always
- Same `get`, `length`, `for-all` as Array
- 2.5-4x compression over raw Arrays
- Supports `bind` for name-to-operator resolution

**Encoding highlights:**
- PackedSimple: Null/Mark/True/False in 1 byte (SS as sub-type)
- PackedCommonOp: 8 hot operators in 1 byte (X|SS as slot index)
- Sign-aware byte stripping for Integer/Operator
- Record: X bit as field count width selector

**When to use:** Procedure bodies (automatic), space-critical data storage,
serialization.

### 3.3 String (62 operators)

Strings are mutable byte arrays with text-oriented operations.

**Creation:**
```
(hello world)   % literal
(hello)#r       % read-only
(raw\nstring)   % with escapes
<(raw content)> % raw string (no escapes)
(value is \{x}) % interpolation
```

**Operations:** search, replace, split, join, trim, pad, repeat, regex (5 ops),
printf/scanf formatting, binary pack/unpack.

**VM layout:** `vm_t` (uint8_t) array in heap. `m_string` (vm_offset_t) +
`m_string_length` (length_t) in header.

**When to use:** Text processing, I/O buffers, binary data, formatting.

### 3.4 Dict (18 operators)

Hash table mapping arbitrary keys to values.

**Creation:**
```
<< /x 3 /y 4 >> % literal (fixed capacity = entry count)
<< /x 3 >>#r    % read-only
0 dynamic-dict  % dynamic (grows on demand)
```

**Operations:**
<!-- doctest: skip (operator reference; `dict` is a placeholder receiver) -->
```
dict /x get               % lookup
dict /x 42 put            % insert/update
dict /x known?            % test existence
dict /x undef             % remove
dict /x 0 get-default     % lookup with fallback
dict /x { 1 add } update  % transform value in-place
dict { 2 mul } map-dict   % transform all values
dict { 0 gt } filter-dict % filter by predicate
dict1 dict2 merge         % combine dicts
```

**VM layout:** Hash table with bucket array + 20-byte Entry chain nodes.
wyhash keys, prime bucket counts (table up to 32771).

**Three modes:**
- ReadOnly: no modification after creation
- ReadWriteFixed: pre-sized, no growth
- ReadWriteDynamic: grows on demand (doubles capacity)

**Dict stack:** Trix's scope mechanism. `begin` pushes a dict, `end` pops
it. Name lookup walks the stack top-down. Module system builds on this.

**When to use:** Key-value maps, configuration, namespaces, dynamic data with
unknown keys, Dict-stack scoping.

### 3.5 Set (19 operators)

Unordered collection of unique keys. Shares Dict's hash table implementation
but stores only keys (12-byte entries vs Dict's 20-byte).

**Creation:**
```
{{ /a /b /c }}         % literal
{{ 1 (hello) true }}   % heterogeneous
[1 2 3] set-from-array % from array
```

**Operations:**
<!-- doctest: skip (operator reference; `set` is a placeholder receiver) -->
```
set /a set-member?             % membership test
set /d set-add                 % add member
set /a set-remove              % remove member
set1 set2 set-union            % union
set1 set2 set-intersection     % intersection
set1 set2 set-difference       % difference
set1 set2 symmetric-difference % XOR
set1 set2 subset?              % subset test
set1 set2 disjoint?            % disjoint test
set { pred } set-filter        % filter by predicate
set members                    % => array of members
```

**Also works on Arrays:** `intersect`, `union`, `difference` operate directly
on arrays for lightweight set operations without creating Set objects.

**When to use:** Membership testing, deduplication, set algebra, tag validation.

### 3.6 Tagged (11 operators)

Discriminated union: one Name tag + one Object payload. The "sum type" half
of algebraic data types.

**Creation:**
```
42 /some tag         % Some(42)
null /none tag       % None
```

**Operations:**
<!-- doctest: skip (operator reference; `tagged` is a placeholder receiver) -->
```
tagged untag                                       % => payload tag-name
tagged tag-name                                    % => tag-name
tagged tag-value                                   % => payload
tagged /some tag?                                  % => bool (is this tag?)
tagged << /some { 2 mul } /none { 0 } >> tag-match % dispatch
tagged { 1 add } tag-update                        % functional update
tagged /some 0 tag-value-or                        % unwrap with default
tagged /ok { proc } tag-bind                       % monadic chain
{ proc } try-result                                % => /ok value | /err error-name
```

**Packed encoding:** Shares PackedType::Curry. X bit discriminates: Curry is
Executable (X=1), Tagged is Literal (X=0).

**When to use:** Option/Maybe types, Result/Either types, state machines,
variant data, railway-oriented programming.

### 3.7 Record (16 operators)

Immutable named-field composite. The "product type" half of algebraic data
types. See docs/record.md for full details.

**Creation:**
```
/point [ /x /y ] record-type def
3 4 point                % => record{x:3, y:4}
100 200 [ /w /h ] record % anonymous
```

**Access:**
<!-- doctest: skip (operator reference; `rec` is a placeholder record) -->
```
rec /x get                   % field by name
rec length                   % field count
rec record-schema            % => [/x /y] (name array)
rec record-fields            % => 3 4 (on stack)
rec record-values            % => [3 4] (as array)
rec /x record-known          % => true
```

**Transforms:**
<!-- doctest: skip (operator reference; rec/rec1/rec2 are placeholder records) -->
```
rec /x { 2 mul } record-map-field  % one field
rec { 10 mul } record-map          % all fields
rec1 rec2 record-merge             % combine (r2 overrides)
rec [ /x ] record-select           % project
```

**Dict interop:**
<!-- doctest: skip (operator reference; `rec` is a placeholder record) -->
```
rec record-to-dict           % => << /x 3 /y 4 >>
<< /x 3 /y 4 >> [ /x /y ] record-from-dict
```

**Batch:**
<!-- doctest: skip (operator reference; col1/col2/records are placeholders) -->
```
[ [col1] [col2] ] [ /a /b ] record-zip % columnar -> records
records /field record-group-by         % group by field value
```

**VM layout:** RecordSchema (shared, `length_t` field_count + `uint16_t` pad +
Name[]) and RecordInstance (vm_offset_t schema + Object[] fields). See docs/record.md
section 11 for implementation details.

**When to use:** Structured data with known fields, function parameters,
return values, configuration, domain objects. Prefer over Dict when schema
is fixed and immutability is desired.

### 3.8 Lazy Sequences (48 operators)

Built on 2-element Arrays: `[head, tail-thunk]`. Null represents the empty
sequence. Thunks memoize evaluation.

**Construction (10 operators):**
```
lazy-nil                     % empty
42 { 1 add } lazy-iterate    % 42, 43, 44, ...
1 lazy-from                  % 1, 2, 3, ...
1 11 1 lazy-range            % 1..10 (start end step, end exclusive)
42 lazy-repeat               % 42, 42, 42, ...
42 5 lazy-repeat-n           % 42 x 5
[1 2 3] lazy-cycle           % 1, 2, 3, 1, 2, 3, ...
seed { proc } lazy-unfold     % proc: seed -> [val new-seed] or null
[1 2 3] lazy-seq              % eager array -> lazy sequence
head tail lazy-cons           % prepend
```

**Transformation (16 operators):**
```
lazy { proc } lazy-map
lazy { pred } lazy-filter
lazy { pred } lazy-filter-not
lazy { pred proc } lazy-filter-map
lazy { proc } lazy-flat-map
lazy lazy-flatten
lazy { proc } lazy-map-indexed
lazy init { proc } lazy-scan
lazy n lazy-take
lazy n lazy-drop
lazy { pred } lazy-take-while
lazy { pred } lazy-drop-while
lazy lazy-dedupe          % drop consecutive duplicates
lazy val lazy-intersperse % insert val between elements
lazy n lazy-step-by       % keep every nth element
lazy xf lazy-into         % apply a transducer (see docs/transducers)
```

**Partitioning (3 operators):**
```
lazy n lazy-chunked          % non-overlapping groups of n (as arrays)
lazy n lazy-windowed         % sliding windows of size n (trailing partials)
lazy lazy-pairwise           % consecutive pairs (= lazy-windowed n=2)
```

**Combination (5 operators):**
```
lazy1 lazy2 lazy-zip
lazy1 lazy2 { proc } lazy-zip-with
lazy1 lazy2 lazy-chain
lazy1 lazy2 lazy-interleave
lazy lazy-enumerate
```

**Terminal (14 operators):**
```
lazy lazy-to-array           % materialize
lazy init { proc } lazy-fold % reduce
lazy { proc } lazy-for-each  % side effects
lazy { pred } lazy-any       % short-circuit search
lazy { pred } lazy-all
lazy { pred } lazy-find
lazy { pred } lazy-find-index
lazy n lazy-nth              % element at index n
lazy lazy-count              % element count (finite only)
lazy lazy-sum                % sum of elements (finite only)
lazy lazy-head               % first element
lazy lazy-tail               % rest (lazy)
lazy lazy-empty?             % null check
any lazy-seq?                % true if null or [head, thunk]
```

**When to use:** Infinite sequences, deferred computation, memory-efficient
pipelines over large data, generator patterns.

## 4. Composition Patterns

### 4.1 Tagged + Record = Algebraic Data Types

The combination gives Trix a complete algebraic type system:

```
% Product types (Record)
/circle [ /center /radius ] record-type def
/rect   [ /origin /width /height ] record-type def

% Sum type (Tagged wrapping Records).  The center/origin are plain [x y]
% arrays here; the area procs only read the size fields.
[ 0 0 ] 5.0 circle /circle tag /shape1 exch def
[ 0 0 ] 10 20 rect /rect tag /shape2 exch def

% Pattern dispatch.  tag-match unwraps the tagged value and passes the
% payload record to the matching proc.
/area {
    << /circle { /radius get dup mul 3.14159 mul }
       /rect   { dup /width get exch /height get mul }
    >> tag-match
} def

shape1 area =    % => 78.53975  (pi * 5^2)
shape2 area =    % => 200       (10 * 20)
```

This mirrors Rust's `enum Shape { Circle { center: Point, radius: f64 }, ... }`.

### 4.2 Record + Lazy = Streaming Pipelines

Records as elements in lazy sequences enable SQL-like data processing:

<!-- doctest: skip (illustrative sketch: record-map-field bodies are placeholders) -->
```
/state [ /pos /vel /time ] record-type def
0.0 1.0 0.0 state

% Infinite physics simulation
{ /pos { vel add } record-map-field
  /time { 0.01 add } record-map-field
} lazy-iterate

% Query pipeline
{ /pos get 0.0 gt } lazy-filter        % WHERE pos > 0
{ /pos { 2.0 mul } record-map-field }  % UPDATE pos *= 2
lazy-map
10 lazy-take lazy-to-array              % LIMIT 10
```

### 4.3 Record + Dict = Configuration with Defaults

```
/config [ /host /port /timeout /retries ] record-type def
/defaults (localhost) 8080 30 3 config def

% Override from user dict
/user-settings << /port 9090 /timeout 60 >> def

% Merge: extract matching fields from dict, merge with defaults
user-settings [ /port /timeout ] record-from-dict
defaults exch record-merge
% => {host: localhost, port: 9090, timeout: 60, retries: 3}
```

### 4.4 Set + Dict = Validated Keys

```
% Define valid configuration keys
/valid-keys {{ /host /port /timeout /retries }} def

% Validate user input
<< /host (example.com) /invalid-key 42 >> keys
{ valid-keys exch set-member? not } filter
dup length 0 eq
{ pop (config valid) = }
{ (invalid keys: ) print == }
if-else
```

### 4.5 Array + Set = Deduplication

```
[1 2 2 3 3 3] unique           % => [1 2 3] (preserves order)
[1 2 3] [2 3 4] intersect      % => [2 3] (set ops on arrays)
[1 2 3] [2 3 4] union          % => [1 2 3 4]
[1 2 3] [2 3 4] difference     % => [1]
```

### 4.6 Record + record-group-by = Aggregation

```
/emp [ /name /dept /salary ] record-type def
/employees [
    (Alice)   /eng   100000 emp
    (Bob)     /sales  80000 emp
    (Charlie) /eng   120000 emp
    (Diana)   /sales  90000 emp
] def

% Average salary by department (map-dict passes each dept's record array)
employees /dept record-group-by
{ dup length /cnt exch def
  0 exch { /salary get add } for-all
  cnt div
} map-dict
% => << /eng 110000 /sales 85000 >>
```

### 4.7 Lazy + Tagged = Railway-Oriented Processing

<!-- doctest: skip (illustrative sketch; `inputs`/parse-and-validate are placeholders) -->
```
% Process a stream of inputs, wrapping each in Result
inputs lazy-from
{ { parse-and-validate } try-result } lazy-map

% Filter successes
{ /ok tag? } lazy-filter

% Extract values
{ tag-value } lazy-map

10 lazy-take lazy-to-array
```

## 5. Decision Guide

### Which collection should I use?

```
Is the data ordered by position?
  Yes -> Is it mutable?
    Yes -> Array
    No  -> Packed (or Array#r)
  No  -> Continue

Is it keyed by name/value?
  Yes -> Is the schema fixed?
    Yes -> Record
    No  -> Dict
  No  -> Continue

Is it a set of unique values?
  Yes -> Set (or array + intersect/union/difference for lightweight)

Is it a variant (one of several types)?
  Yes -> Tagged

Is it potentially infinite or lazily computed?
  Yes -> Lazy sequence (Array + Thunk)
```

### Collection choice by scenario

| Scenario                  | Best choice           | Why                                        |
| ------------------------- | --------------------- | ------------------------------------------ |
| Config with known keys    | Record                | immutable, named fields, schema validation |
| Config with unknown keys  | Dict                  | dynamic keys, hash lookup                  |
| Function return values    | Record                | self-documenting field names               |
| Sum type / variant        | Tagged                | pattern dispatch, monadic chaining         |
| Large dataset processing  | Lazy sequence         | memory-efficient, composable               |
| Unique element tracking   | Set                   | O(1) membership, set algebra               |
| Ordered data pipeline     | Array + map/filter    | positional, rich operator set              |
| Text manipulation         | String                | 62 dedicated operators                     |
| Compact serialization     | Packed                | 2.5-4x compression                         |
| Default + override config | Record + record-merge | merge semantics                            |
| Columnar -> row data      | record-zip            | batch construction                         |
| Group by category         | record-group-by       | automatic grouping                         |
| Algebraic data types      | Tagged + Record       | sum + product types                        |

## 6. Implementation Architecture (for Maintainers)

### 6.1 Object Representation

All collections are 8-byte Objects:

```
Object (8 bytes):
  m_aat (1 byte):        type (5 bits) + access (1 bit) + attrib (1 bit) + special (1 bit)
  m_save_level (1 byte): save/restore tracking
  m_length (2 bytes):    element count (Array, Packed, String, Record)
                         OR operator data (Operator) OR stream id (Stream)
  m_offset (4 bytes):    vm_offset_t to heap data
```

The `m_length` field is shared by Array, Packed, String, and Record.
Dict and Set store length in the heap-resident hash table header.

### 6.2 VM Heap Layouts

| Type         | Heap layout                                        | Alignment       |
| ------------ | -------------------------------------------------- | --------------- |
| Array        | Object[N]                                          | 4-byte (Object) |
| Packed       | packed_data_t[variable]                            | 1-byte          |
| String       | vm_t[N+1] (null-terminated)                        | 1-byte          |
| Dict         | Header(16B) + Buckets(4B each) + Entries(20B each) | 4-byte          |
| Set          | Header(16B) + Buckets(4B each) + Entries(12B each) | 4-byte          |
| Tagged       | Object[2] (tag-name, payload)                      | 4-byte          |
| Record       | RecordInstance(vm_offset_t + Object[N])            | 4-byte          |
| RecordSchema | uint32_t + Object[N]                               | 4-byte          |
| Curry        | Object[2] (value, callable)                        | 4-byte          |
| Thunk        | Object[3] (state, proc, result)                    | 4-byte          |

### 6.3 verify_t Type Acceptance

The operand verification system uses 64-bit bitmasks:

```
Bits 0..30:   one bit per Object::Type (31 types)
Bits 32..45:  constraint flags (RW, Exe, NotZero, etc.)
```

Composite verify groups for collections:
```
VerifyArrays     = VerifyArray | VerifyPacked
VerifyIndexable  = VerifyString | VerifyArray | VerifyPacked
VerifyHasLength  = VerifyName | VerifyArray | VerifyPacked | VerifyString |
                   VerifyDict | VerifySet | VerifyRecord
VerifyHasAccess  = VerifyStream | VerifyArray | VerifyPacked | VerifyString |
                   VerifyDict | VerifySet
VerifyKey        = (all types except Null and SourceLoc)
```

### 6.4 Operator Dispatch Patterns

**Type-specific operators** (e.g., `set-add`, `record-update`): verify exact
type, operate directly.

**Polymorphic operators** (e.g., `get`, `for-all`, `length`): branch on type
in a chain of `is_dict()` / `is_record()` / `is_array()` checks. Record is
checked before Dict in `get` to avoid the Dict's more general key handling.

**Control operators** for iteration (`@array-for-all`, `@dict-for-all`,
`@record-for-all`, `@record-map-step`): saved on exec stack with per-type
state. Each iteration pops the control op, processes one element, and re-pushes
itself for the next iteration. Final iteration pops the entire frame.

### 6.5 Packed Encoding Summary

31 PackedType codes (`PackedTypeCount`; slot 31 of the 5-bit space is unused):

| Slot     | Type       | Bytes | Special encoding                                          |
| -------- | ---------- | ----- | --------------------------------------------------------- |
| 0        | CommonOp   | 1     | X\|SS = 3-bit slot (8 hot operators)                      |
| 1-2, 4-9 | Primitives | 2-5   | SS = value byte count                                     |
| 3        | PackedExt  | 2-5   | custom: subcode selects Int128/UInt128/eqref/OpaqueHandle |
| 10       | Simple     | 1     | SS = sub-type (Null/Mark/False/True)                      |
| 11       | Curry      | 2-5   | X=1 Curry, X=0 Tagged                                     |
| 12       | Operator   | 2-3   | sign-aware encoding                                       |
| 14       | Record     | 3-7   | X = fc width, SS = offset bytes                           |
| 15       | Name       | 3-5   | standard                                                  |
| 16-29    | Containers | 2-5   | Short/Long length variants                                |
| 30       | Dict       | 2-5   | standard                                                  |
| 13       | Reserved2  | --    | unused (slot 31 also unused)                              |

### 6.6 Save/Restore Interaction

| Type   | Journaled?                   | What is saved                          |
| ------ | ---------------------------- | -------------------------------------- |
| Array  | Yes                          | per-element Object before overwrite    |
| Packed | No (immutable via bind only) | --                                     |
| String | Partial                      | header only; byte writes NOT journaled |
| Dict   | Yes                          | header + per-entry before overwrite    |
| Set    | Yes                          | header + per-entry before overwrite    |
| Tagged | No (immutable)               | --                                     |
| Record | No (immutable)               | --                                     |
| Curry  | No (immutable)               | --                                     |
| Thunk  | Internal                     | state Object updated on force          |

Immutable types (Tagged, Record, Curry) have zero save/restore overhead.
Array and Dict mutations are fully journaled for rollback.

### 6.7 sm_object_attrib Flags

Each type has a static attribute byte:

| Flag | Meaning | Types with flag |
| --- | --- | --- |
| PushOpDirect | pushed to op stack in proc body | most (not Null, Operator, Name, String, Stream, SourceLoc) |
| IgnoresExecute | pushed even via executable name | numbers, Bool, Mark, Dict, Set, Tagged, Record, Coroutine, PipeBuffer, Cell (+ SourceLoc, Screen handle) |
| HasObjectAccess | R/W access control on Object | Array, Packed, String, Stream |
| HasValueAccess | R/W access control on value | Dict, Set |
| HasObjectLength | length stored in Object header | Name, Array, Packed, String, Record |
| HasValueLength | length stored in heap value | Dict, Set |
| UsesVM | m_offset references VM heap | most composite types |
| UsesExtValue | m_offset references ExtValue | Long, ULong, Address, Double |

Record: `PushOpDirect | IgnoresExecute | HasObjectLength | UsesVM`

## 7. Operator Count Summary

| Category                   | Count    |
| -------------------------- | -------- |
| Array creation             | 5        |
| Array access               | 10       |
| Higher-order (array)       | 18       |
| Aggregation                | 7        |
| Transformation             | 9        |
| Set operations (on arrays) | 3        |
| Set type                   | 19       |
| Dict operations            | 13       |
| Dict querying              | 5        |
| Tagged values              | 11       |
| Record type                | 16       |
| Lazy sequences             | 48       |
| **Total**                  | **~164** |

This exceeds Lua's ~4 table operations and Python's ~15 list methods + itertools
combined. Every operator is built-in with zero imports required.

## 8. Performance Comparison

### 8.1 Memory per Element

| Type   | Bytes/element | Notes                              |
| ------ | ------------- | ---------------------------------- |
| Array  | 8             | fixed Object size                  |
| Packed | 1-7           | variable, ~2.5-4x compression      |
| String | 1             | byte storage                       |
| Dict   | 20            | Entry: next(4) + key(8) + value(8) |
| Set    | 12            | Entry: next(4) + key(8)            |
| Tagged | 16            | 2 Objects: tag + payload           |
| Record | 4 + 8*N       | schema offset + N fields           |

Plus hash bucket overhead for Dict/Set (4 bytes per bucket, prime count).

### 8.2 Lookup Complexity

| Type   | By index  | By name/key      | Existence check   |
| ------ | --------- | ---------------- | ----------------- |
| Array  | O(1)      | --               | --                |
| Packed | O(N) skip | --               | --                |
| String | O(1)      | --               | --                |
| Dict   | --        | O(1) amortized   | known? O(1)       |
| Set    | --        | --               | set-member? O(1)  |
| Record | --        | O(N) linear scan | record-known O(N) |

Record's O(N) field lookup is by design — records are small (typically < 10
fields), and linear scan of Name objects is cache-friendly. For large key
sets, use Dict.

### 8.3 Mutation Cost

| Type   | Element update  | Structural change           |
| ------ | --------------- | --------------------------- |
| Array  | O(1) in-place   | O(N) copy (append, map)     |
| String | O(1) byte put   | O(N) copy (append, replace) |
| Dict   | O(1) hash put   | O(N) pool grow (no rehash)  |
| Set    | O(1) hash add   | O(N) pool grow (no rehash)  |
| Record | O(N) full copy  | O(N) full copy (always)     |
| Tagged | N/A (immutable) | O(1) tag-update via proc    |

Record's O(N) update cost is the trade-off for immutability. For
mutation-heavy workflows, use Dict.
