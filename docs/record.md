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

# Record Type — Technical Reference

## 1. What Record Is

Record is an immutable, named-field composite type. It holds a fixed set of
named fields, each containing an arbitrary Trix value. Once constructed, a
record's fields cannot be modified — updates return a new record.

```
/point [ /x /y ] record-type def
3 4 point           % => record with x=3, y=4
dup /x get =        % => 3 (prints it, leaving the record on the stack)
/x 10 record-update % => new record with x=10, y=4 (original unchanged)
```

Record sits between Dict and Array in Trix's type hierarchy:

| Property        | Array                  | Record                      | Dict                        |
| --------------- | ---------------------- | --------------------------- | --------------------------- |
| Field access    | positional (`0 get`)   | named (`/x get`)            | named (`/x get`)            |
| Mutability      | read-write             | **immutable**               | read-write                  |
| Schema          | none (any length)      | **fixed at creation**       | dynamic (any key)           |
| Memory          | N * 8 bytes            | 4 + N * 8 bytes             | hash table + 20 bytes/entry |
| Construction    | `[ 1 2 3 ]`            | `3 4 point`                 | `<< /x 3 /y 4 >>`           |
| Iteration order | positional             | **field declaration order** | hash order (arbitrary)      |
| Use case        | sequences, collections | **structured data**         | maps, namespaces            |

## 2. Why Record Exists

Stack-based languages have a well-known pain point: **structured data is
awkward**. Without records, a "point" is either two loose values on the stack
(requiring mental simulation of stack positions) or a Dict (requiring hash
table overhead for 2 fields).

Record solves this by providing:

1. **Named access** — `/x get` instead of `0 get`. Self-documenting, order-independent.
2. **Single-value passing** — pass one record instead of juggling N values through
   `exch`/`roll`/`index`.
3. **Schema safety** — accessing a nonexistent field is a hard error, not a silent null.
4. **Immutability** — records can be freely shared, stored, and passed without
   defensive copying. No save/restore journaling needed.
5. **Lightweight** — no hash table, no buckets, no tombstones. Just a flat array
   of Objects with a shared schema.

## 3. Quick Reference

### Construction
```
record-type     name_array -- proc       % define a record constructor
record     v0..vN-1 name_array -- record % construct anonymous record
```

### Access
```
get             record name -- value        % field by name (error on missing)
length          record -- int               % field count
record-schema   record -- name_array        % copy of field names
record-fields   record -- v0..vN-1          % push all values in field order
record-values   record -- array             % field values as array
record-known    record name -- bool         % test if field exists
is-record       any -- bool                 % type predicate
```

### Update
```
record-update   record name value -- record'  % new record with one field replaced
```

### Transforms
```
record-map-field  record name proc -- record'  % apply proc to one field
record-map        record proc -- record'       % apply proc to all fields
record-merge      record1 record2 -- record'   % merge (record2 overrides)
record-select     record name_array -- record' % project to field subset
```

### Dict interop
```
record-to-dict    record -- dict               % convert to read-write dict
record-from-dict  dict name_array -- record    % extract fields from dict
```

### Batch
```
record-zip        value_array name_array -- record_array % columnar to row
record-group-by   record_array name -- dict              % group by field value
```

### Polymorphic
```
for-all         record proc --              % proc receives: name value (per field)
deep-eq         record record -- bool       % structural equality
type            record -- /record-type      % type name
```

## 4. Construction Patterns

### 4.1 Schema-First (record-type)

The primary pattern: define a schema once, reuse it for many instances.

```
/point [ /x /y ] record-type def
```

`record-type` takes an array of literal Name objects and returns an executable
procedure. This procedure, when called, pops N values from the stack and
constructs a Record instance.

```
3 4 point       % pops 4 (y), pops 3 (x), returns record{x:3, y:4}
```

**Stack order:** values are pushed left-to-right matching the field declaration.
`3 4 point` means x=3, y=4. This matches the natural reading:
"the fields are x and y; I push x-value then y-value then call the constructor."

The returned procedure is a normal executable array `{ <schema-holder record> @record-ctor }` (element 0 is a zero-field Record holder whose `m_schema` references the schema, so the GC keeps the schema alive).
It can be passed to `curry`, `map`, `bind`, or stored in any container.

**Schema validation:** `record-type` checks that all elements are literal Names
and that there are no duplicates. Violations raise `type-check` or `range-check`.

### 4.2 Anonymous Records (record)

For one-off records where defining a named constructor is overkill:

```
100 200 [ /width /height ] record
```

This allocates both the schema and instance in one call. The schema is not
shared — each `record` call allocates a fresh schema. For repeated use,
`record-type` is more memory-efficient.

### 4.3 Empty Records

Records with zero fields are allowed:

```
[ ] record-type /unit exch def
unit                    % => empty record
unit length             % => 0
unit unit deep-eq       % => true
```

An empty record carries no data but has distinct identity. It can serve as a
sentinel, a unit type, or a placeholder in algebraic data type definitions.

### 4.4 Curried Constructors

Since `record-type` returns a procedure, it composes with `curry` for partial
application:

```
/point [ /x /y ] record-type def

% Fix y=0 for all points on the x-axis
/x-axis-point 0 //point curry def
5 x-axis-point          % => record{x:5, y:0}

% Note: curry pushes its captured value AFTER the caller's values,
% so the captured value fills the LAST field, not the first.
```

**Important:** `curry` pushes the captured value onto the stack, then executes
the callable. Since the constructor pops values top-to-bottom (last field first),
the curried value becomes the **last** field. To fix the **first** field, use a
wrapper procedure:

```
/point [ /x /y ] record-type def
% Fix x=10 for all points
/at-x10 { 10 exch point } def
20 at-x10               % => record{x:10, y:20}
```

## 5. Field Access and Iteration

### 5.1 Named Access (get and `.field` sugar)

```
/point [ /x /y ] record-type def
3 4 point /x get        % => 3
3 4 point /y get        % => 4

3 4 point .x            % => 3  (scanner sugar; identical emission)
3 4 point .y            % => 4
```

`get` dispatches on the container type. For records, it takes a Name, scans the
schema for a match, and returns the corresponding field value (cloned to the
operand stack).

**`.field` sugar:** the scanner rewrites `receiver.name` to `receiver /name get`
at scan time (see `scanner-syntax.md` Section 8b).  Chains left-to-right:
`order.customer.address.city` becomes `order /customer get /address get /city get`.
Identical runtime semantics to the classic `/name get` form -- pick whichever
reads better at the call site.  Works uniformly on records, dicts with name
keys, and any other container `get` handles.

```
/order << /customer << /name (Alice) /addr << /city (Seattle) >> >> >> def
order .customer .addr .city     % => (Seattle)
```

**Point-free accessors** read well inside procedures:

<!-- doctest: skip (operator reference; `orders` is a placeholder array of records) -->
```
/get-name { .name } def
orders { get-name } map          % extract name from each record
```

**Error behavior:** accessing a field not in the schema raises `undefined`.
This is intentional — a record's schema is fixed and known, so a missing field
is a programming error, not a "maybe it's there" situation.

```
{ 3 4 point /z get } try    % => /undefined
{ 3 4 point .z } try        % => /undefined  (same runtime path)
```

**Performance:** field lookup is a linear scan of Name objects in the schema.
This is fast for typical record sizes (N < 10). For records with many fields,
consider whether a Dict is more appropriate.

### 5.2 Destructuring (record-fields)

```
3 4 point record-fields     % => 3 4
```

Pushes all field values onto the stack in field declaration order (field 0
deepest, field N-1 on top). This is the inverse of construction:

```
/point [ /x /y ] record-type def
7 8 point record-fields point   % => record{x:7, y:8} (round-trip)
```

### 5.3 Schema Inspection (record-schema)

```
/point [ /x /y ] record-type def
3 4 point record-schema     % => [ /x /y ]
```

Returns a **new array** containing copies of the field Names. Modifying the
returned array does not affect the record or its schema.

### 5.4 Iteration (for-all)

```
/point [ /x /y ] record-type def
3 4 point { def } for-all
x       % => 3
y       % => 4
```

`for-all` on a record pushes `name value` pairs (matching Dict's convention)
and executes the procedure for each field, in field declaration order.

Common patterns:
<!-- doctest: skip (illustrative patterns; `rec` is a placeholder record) -->
```
% Sum all numeric fields
0 rec { exch pop add } for-all

% Print all fields (for-all pushes name value; to-string needs a dst buffer)
rec {
    /val exch def
    32 string to-string print  (: ) print
    val 32 string to-string print  ( ) print
} for-all

% Convert record to dict
<< >> begin
rec { def } for-all
current-dict end
```

### 5.5 Length

```
3 4 point length  % => 2
[ ] record length % => 0
```

Returns the number of fields. This is an O(1) operation — the field count is
stored in the Object header.

### 5.6 Field Values (record-values)

```
3 4 point record-values       % => [3 4]
```

`record-values` extracts all field values into a new array, in schema order.
Combined with `record-schema`, this enables full record decomposition:

<!-- doctest: skip (operator reference; `rec` is a placeholder record) -->
```
rec record-schema             % => [/x /y]
rec record-values             % => [3 4]
```

The returned array is a fresh copy — modifying it does not affect the record.

**Error behavior:** raises `type-check` on non-record input.

### 5.7 Field Existence (record-known)

```
/point [ /x /y ] record-type def
3 4 point /x record-known     % => true
3 4 point /z record-known     % => false
```

`record-known` tests whether a field name exists in the record's schema without
raising an error. This enables safe access patterns:

<!-- doctest: skip (operator reference; `rec` is a placeholder record) -->
```
rec /x record-known { rec /x get } { -1 } if-else
```

Unlike `get` (which raises `undefined` for missing fields), `record-known`
always returns a boolean — useful when the schema may vary.

**Error behavior:** raises `type-check` on non-record input.

## 6. Functional Update

```
/point [ /x /y ] record-type def
3 4 point /x 10 record-update
% => new record{x:10, y:4}
```

`record-update` returns a **new** record with one field replaced. The original
record is unchanged. This is the fundamental operation for "modifying" immutable
data.

**Chained updates:**
```
/point [ /x /y ] record-type def
1 2 point
/x 10 record-update
/y 20 record-update     % => record{x:10, y:20}
```

Each update allocates a new RecordInstance in VM heap (same schema, new field
values). The schema is shared across all instances.

**Cost:** O(N) — all fields are copied. For records with many fields where only
one changes, this is more expensive than Dict's O(1) `put`. But for typical
record sizes (N < 10), the cost is negligible. If frequent single-field mutation
is the primary access pattern, use a Dict.

**Error:** updating a field not in the schema raises `undefined`.

## 7. Composition with Other Types

### 7.1 Records + Tagged = Algebraic Data Types

The combination of Record (product types) and Tagged (sum types) gives Trix a
complete algebraic type system:

```
/point [ /x /y ] record-type def
% Define shape types
/circle [ /center /radius ] record-type def
/rect   [ /origin /width /height ] record-type def

% Construct tagged shapes
0 0 point 5.0 circle /:circle tag     % circle at origin, radius 5
0 0 point 10 20 rect /:rect tag       % rect at origin, 10x20

% Dispatch on shape type
/area {
    % tag-match unwraps the tagged value and passes the payload (the record)
    % to the matching handler, keyed by the exact tag name.
    << /:circle { dup /radius get dup mul 3.14159 mul }
       /:rect   { dup /width get exch /height get mul }
    >> tag-match
} def
```

This pattern mirrors Rust's `enum` with struct variants, Haskell's algebraic
data types, or OCaml's variant types — but expressed in stack-based syntax.

### 7.2 Records + for-all = Serialization

`for-all` enables generic record processing without knowing the schema:

```
/point [ /x /y ] record-type def
% Convert any record to a string representation
/record-to-string {
    /rec exch def
    () ({) concat                   % accumulator string (empty -> "{")
    /first true def
    rec {
        /val exch def /nm exch def
        first not { (, ) concat } if
        /first false def
        nm 16 string to-string concat (: ) concat
        val 16 string to-string concat
    } for-all
    (}) concat
} def

3 4 point record-to-string    % => ({x: 3, y: 4})
```

### 7.3 Records in Packed Arrays

Records can be stored in packed arrays for compact representation:

```
/point [ /x /y ] record-type def
3 4 point 5 6 point 7 8 point 3 packed
% compact packed representation of 3 records
```

Records survive pack/unpack with full fidelity — schema, field count, and all
field values are preserved. The packed encoding uses `PackedType::Record`
(slot 14), which repurposes the header's X bit as a field count width selector
since records are always literal.

### 7.4 Records as Configuration

Records are ideal for configuration and options:

```
/config [ /host /port /timeout /retries ] record-type def

/default-config
    (localhost) 8080 30 3 config def

% Override specific fields
/my-config default-config
    /port 9090 record-update
    /timeout 60 record-update
def
```

The original `default-config` is never modified — each `record-update` returns
a fresh copy. This is safe for concurrent use and prevents accidental mutation.

### 7.5 Records as Return Values

Replace multiple loose return values with a single named record:

```
% Without records: caller must know order
/parse-coord { ... x y } def     % which is x? which is y?

% With records: self-documenting
/coord [ /x /y /z ] record-type def
/parse-coord { ... coord } def
parse-coord /z get               % unambiguous
```

### 7.6 Records as Function Parameters

Replace long parameter lists with a single options record:

<!-- doctest: skip (illustrative sketch: set-color/draw are placeholder drawing ops) -->
```
/draw-opts [ /color /thickness /filled ] record-type def

/draw-circle {
    /opts exch def
    /center exch def
    opts /color get set-color
    opts /thickness get set-line-width
    opts /filled get { fill-circle } { stroke-circle } if-else
    center /x get center /y get draw
} def

% Call with named parameters
0 0 point
(red) 2 true draw-opts
draw-circle
```

## 8. Data Pipelines

The pipeline operators transform records into an active data-processing
primitive. Combined with lazy sequences, they enable SQL-like workflows.

### 8.1 Field-Level Transforms

`record-map-field` applies a proc to one named field:
```
/point [ /x /y ] record-type def
3 4 point /x { 2 mul } record-map-field    % => {x:6, y:4}
```

`record-map` applies a proc to all fields uniformly:
```
3 4 point { 10 mul } record-map            % => {x:30, y:40}
```

### 8.2 Merge and Select

`record-merge` combines two records (record2 overrides record1):
```
/defaults (localhost) 8080 [ /host /port ] record def
/overrides 9090 [ /port ] record def
defaults overrides record-merge             % => {host:localhost, port:9090}
```

`record-select` projects to a subset of fields:
```
(Alice) 30 (NYC) [ /name /age /city ] record
[ /name /age ] record-select                % => {name:Alice, age:30}
```

### 8.3 Dict Interop

Convert between records and dicts:
```
/point [ /x /y ] record-type def
3 4 point record-to-dict                     % => << /x 3 /y 4 >>
<< /x 10 /y 20 >> [ /x /y ] record-from-dict % => record{x:10, y:20}
```

### 8.4 Batch Operations

`record-zip` converts columnar data to records:
```
[ [ (Alice) (Bob) ] [ 30 25 ] ] [ /name /age ] record-zip
% => [ {name:Alice, age:30}  {name:Bob, age:25} ]
```

`record-group-by` groups records by a field value:
<!-- doctest: skip (operator reference; alice-rec/bob-rec/charlie-rec are placeholder records) -->
```
[ alice-rec bob-rec charlie-rec ] /dept record-group-by
% => << /eng [alice bob] /sales [charlie] >>
```

### 8.5 Lazy Record Pipelines

Records compose with lazy sequences for streaming data processing:
<!-- doctest: skip (illustrative sketch: record-map-field body is a placeholder) -->
```
/state [ /pos /vel ] record-type def

% Infinite sequence of physics states
0.0 1.0 state
{ /pos { dup } record-map-field     % placeholder: pos += vel
} lazy-iterate

% Filter + transform pipeline
{ /pos get 0 gt } lazy-filter
{ /pos { 2 mul } record-map-field } lazy-map
10 lazy-take lazy-to-array
```

Records as `lazy-iterate` state objects replace fragile multi-value stack
state with named, self-documenting bundles.

## 9. Performance Characteristics

### 9.1 Memory Layout

Each record consists of two VM heap allocations:

**Schema** (shared, allocated once per `record-type` call):
```
RecordSchema {
    length_t  m_field_count;     // 2 bytes (uint16_t)
    uint16_t  m_pad;             // 2 bytes (align Object array)
    Object    m_names[N];        // N * 8 bytes
}
Total: 4 + (N * 8) bytes
```

**Instance** (one per record value):
```
RecordInstance {
    vm_offset_t  m_schema;       // 4 bytes (pointer to schema)
    Object       m_fields[N];    // N * 8 bytes
}
Total: 4 + (N * 8) bytes
```

RecordSchema uses `length_t` + a `uint16_t` pad and RecordInstance uses
`vm_offset_t` as their 4-byte headers, ensuring 4-byte alignment of the Object
arrays that follow.

**Object header** (on the operand/exec stack):
```
Object (8 bytes):
    m_aat:     LiteralAttrib | ReadOnlyAccess | Type::Record
    m_length:  field count
    m_record:  vm_offset_t to RecordInstance
```

### 9.2 Cost Comparison

| Operation             | Record           | Dict                      |
| --------------------- | ---------------- | ------------------------- |
| Construction          | O(N) copy values | O(N) hash + insert        |
| `get` by name         | O(N) linear scan | O(1) amortized hash       |
| `length`              | O(1) from header | O(1) from header          |
| `for-all`             | O(N) sequential  | O(B+N) bucket scan        |
| `record-update`       | O(N) full copy   | O(1) in-place `put`       |
| Memory per instance   | 4 + N*8 bytes    | 16 + buckets + N*20 bytes |
| Immutability          | guaranteed       | requires `make-readonly`  |
| Save/restore overhead | none             | journaling per mutation   |

**When to use Record:** structured data with known fields, configuration,
return values, parameter bundles, algebraic data types. N is typically < 10.

**When to use Dict:** dynamic key sets, frequent single-field mutation, large
maps, lookup-heavy workloads where O(1) hash access matters.

### 9.3 Packed Encoding Size

A packed record uses:
- 1 byte header
- 1-2 bytes field count (X bit selects width)
- 1-4 bytes vm_offset_t (SS field selects width)
- Total: 3-7 bytes per packed record

This is the same or smaller than a packed Dict entry.

## 10. Design Decisions

### 10.1 Why Immutable?

Mutable records would require save/restore journaling (like Array and Dict),
increasing complexity and memory overhead. Immutability means:

- No journaling — records never appear in the save undo log.
- Safe sharing — multiple references to the same record are safe.
- `make_clone` is a shallow copy — just copy the 8-byte Object header,
  not the heap data.
- Thread-safe by construction.

The trade-off is that `record-update` is O(N) (copies all fields). For
typical record sizes this is negligible.

### 10.2 Why Schema-First?

The schema (field names) is separated from instances and shared across all
records created by the same constructor. This means:

- Field names are stored once, not repeated per instance.
- A 5-field record with 100 instances uses one 44-byte schema and 100
  44-byte instances = 4,444 bytes. Without sharing: 100 * 84 bytes = 8,400.
- Schema identity enables fast `deep-eq`: if two records share the same
  schema_offset, skip the field-name comparison.

### 10.3 Why Procedure, Not Operator?

`record-type` returns an executable procedure (Array), not a dedicated
operator type. This was chosen because:

- No new mechanisms needed — procedures already exist.
- Composes with `curry`, `bind`, `map`, and all procedure-accepting operators.
- No operator table slots consumed per record-type definition.
- The trade-off: `type` on the constructor returns `array-type`, not something
  record-specific. In practice this doesn't matter — you define the constructor,
  you know what it is.

### 10.4 Why X-Bit for Packed Field Count?

Records are always literal, so the X (executable/literal) bit in the packed
header has no meaning. Rather than waste it, Record repurposes it as a field
count width selector:

- X=0: 1-byte field count (0-255 fields)
- X=1: 2-byte field count (0-65535 fields)

This follows the precedent set by PackedSimple (SS as sub-type selector) and
PackedCommonOp (X|SS as 3-bit slot index). Zero wasted bits.

### 10.5 Why ReadOnly Access?

Records set `ReadOnlyAccess` in their `m_aat` byte, but in practice `put` and
other write operators reject a record at the type-check (operand-verify) level:
Record is not a member of any read-write verify mask, so verification raises
`/type-check` before the access check is ever reached. The ReadOnly bit is
belt-and-suspenders; the type system is the actual gate.

### 10.6 Why Linear Scan for get?

Field lookup in `get` is O(N) — a linear scan of the schema's Name array,
comparing by `equal()`. For typical records (N < 10), this is faster than a
hash lookup because:

- No hash computation needed.
- Sequential memory access (cache-friendly).
- Name comparison is a single integer compare (Name offsets).

If records with many fields become common, the schema could be extended with a
perfect-hash table. This is a future optimization, not a current need.

## 11. Implementation Details (for Maintainers)

### 11.1 Object::Type and Verify

```cpp
enum struct Type : type_t {
    ...
    Tagged,     // 10110
    Record      // 10111 — immutable named-field composite
};

static constexpr verify_t VerifyRecord{1ull << +Object::Type::Record};
```

Record is included in `VerifyKey`, `VerifyAny`, `VerifyHasLength`.
Excluded from `VerifyExecutable` (always literal).

Object attribute flags: `PushOpDirect | IgnoresExecute | HasObjectLength | UsesVM | IsDeepeq`.

### 11.2 Object Layout

```cpp
// Factory
static constexpr Object make_record(vm_offset_t offset, length_t field_count) {
    Object object;
    object.m_aat = (LiteralAttrib | ReadOnlyAccess | +Type::Record);
    object.m_object_save_level = Save::BASE;
    object.m_length = field_count;      // reuses m_arrays_length union slot
    object.m_record = offset;           // vm_offset_t to RecordInstance
    return object;
}
```

### 11.3 VM Heap Structs

```cpp
struct RecordSchema {
    length_t m_field_count;  // uint16_t (2 bytes)
    uint16_t m_pad;          // pad to alignof(Object) = 4
    Object m_names[1];  // variable-length: m_names[0..m_field_count-1]

    static vm_size_t alloc_size(length_t n) {
        return offsetof(RecordSchema, m_names) + (n * sizeof(Object));
    }
};

struct RecordInstance {
    vm_offset_t m_schema;
    Object m_fields[1];  // variable-length: m_fields[0..field_count-1]

    static vm_size_t alloc_size(length_t n) {
        return offsetof(RecordInstance, m_fields) + (n * sizeof(Object));
    }
};
```

RecordSchema uses `length_t` + a `uint16_t` pad, and RecordInstance uses
`vm_offset_t`, as the first 4 bytes to ensure 4-byte alignment of the Object array. Allocations use `vm_alloc<RecordSchema>()` and
`vm_alloc<RecordInstance>()` which call `align_vm_ptr<T>()` for proper
alignment.

**Alignment note:** An earlier implementation used `vm_alloc<vm_t>()` with
`reinterpret_cast` to Object arrays, causing UBSan misaligned-access errors.
The struct-based approach guarantees correct alignment through the type system.

### 11.4 Constructor Mechanism

`record-type` allocates the schema in VM heap, then builds an executable
array containing two elements:

```
{ <schema-holder record> @record-ctor }
```

When this procedure executes:
1. Element 0 is a zero-field Record holder whose `m_schema` references the
   schema (a bare uinteger offset would be GC-invisible and get swept; the
   markable Record holder keeps the schema alive).
2. `@record-ctor` (a control operator) reads the holder's `m_schema`, reads
   `field_count` from the schema, pops N values, allocates a RecordInstance,
   and pushes the Record.

`@record-ctor` is a control operator (`SystemName::atRecordCtor`) rather than
a standard operator because it is internal and hidden from the user (prefixed
with `@`).

### 11.5 Packed Encoding

Record uses `PackedType::Record` (enum value 14).

**Encoding (packer):**
```
Header: |X|SS|01110|
         |  |
         |  +-- SS: vm_offset_t byte count - 1 (1-4 bytes, unsigned stripping)
         +---- X: field count width (0 = 1 byte, 1 = 2 bytes)

Stream: [header] [field_count: 1 or 2 bytes, big-endian] [offset: SS+1 bytes, big-endian]
```

The packer handles Record as a special case with a `continue` before the
standard header/length/value output path, since Record's layout differs from
the standard `[header][length][value]` pattern.

**Decoding (unpacker):**
Record has `VSizeZero` in `sm_sizes[]`, so the standard flow reads no value
bytes. The `extract_next_packed` switch case reads field_count and offset
manually from the packed stream.

**Skipping (`skip_packed`):**
Record is special-cased in `skip_packed()` because `packed_sizes()` returns
`{0, 0, false}` for VSizeZero types, which would skip only the header byte.
The special case decodes X and SS from the header to compute the actual byte
count: `1 + fc_size + offset_size`.

### 11.6 SystemName Entries

**Control operators:**
- `atRecordCtor` — `@record-ctor`: internal constructor
- `atRecordForAll` — `@record-for-all`: internal for-all iterator
- `atRecordMapFieldComplete` — `@record-map-field-complete`: map-field completion
- `atRecordMapStep` — `@record-map-step`: record-map iteration step

**Standard operators:**
- `RecordTypeOp` — `record-type`
- `MakeRecord` — `record`
- `RecordSchema` — `record-schema`
- `RecordFields` — `record-fields`
- `RecordUpdate` — `record-update`
- `RecordValues` — `record-values`
- `RecordKnown` — `record-known`
- `IsRecord` — `is-record`
- `RecordMapField` — `record-map-field`
- `RecordMap` — `record-map`
- `RecordMerge` — `record-merge`
- `RecordSelect` — `record-select`
- `RecordToDict` — `record-to-dict`
- `RecordFromDict` — `record-from-dict`
- `RecordZip` — `record-zip`
- `RecordGroupBy` — `record-group-by`

### 11.7 Switch Statement Coverage

Adding a new `Object::Type` requires updating every `switch` on `Type` in
trix.h. Record was added to approximately 20 switch statements:

- `type_sv()` — returns `"record-type"sv`
- `hash()` — hashes `m_offset` (same as Curry/Tagged)
- `equal()` — compares `m_record` offset and `m_length`
- `integer_value()` (two overloads) — falls through to break (not numeric)
- `floating_point_value()` (two overloads) — falls through to break
- `deep_equal()` — structural comparison of schema + fields
- `for_op()` — falls through to error (not numeric)
- `promote_convert()` — falls through to error
- `cast_to_type()` — falls through to error
- `cast_op()` — falls through to error
- `reinterpret_op()` — falls through to error
- `pack_extract_signed/unsigned/double()` — falls through to error
- `clearobject_op()` — falls through to error
- `add_one_integer()` — falls through to error
- `vmsize_op()` — returns the instance size plus the shared schema size:
  `(sizeof(vm_offset_t) + field_count*sizeof(Object)) + (offsetof(RecordSchema, m_names) + field_count*sizeof(Object))`
- `make_packed_data()` — sets `PackedType::Record` and `value = m_record`
- `make_binary_token_string()` — falls through to error (not tokenizable)
- `scan_numeric()` / `scan_value()` (ScanFmt) — falls through to error
- `Number::scan()` — falls through to error
- `sm_object_attrib[]` — entry with correct attribute flags
- `sm_type[]` — maps PackedType::Record to Type::Record
- `sm_sizes[]` — VSizeZero (custom encoding)

### 11.8 PrintFmt

```
%s  => "record"
%#s => "RECORD"
%O  => --record {/x: 3i /y: 4i}--   (fields as /name: value, declaration order)
```

The `%O` form shows each field as `/name: value` in declaration order — the
same as the `==` debug print. Use `record-schema` and `record-fields` for
programmatic inspection.

### 11.9 Save/Restore Interaction

Records are immutable — no save/restore interaction exists:

- No journaling: record heap data is never modified after construction.
- `make_clone()` / `make_copy()`: default behavior (copies 8-byte Object header
  including `m_record` offset). Does NOT deep-copy heap data. This is correct
  because the heap data is immutable and shared.
- Schema allocations are normal VM heap allocations. `restore` reclaims them
  only if the save barrier is above the allocation point (same as any other
  VM allocation).

### 11.10 Resource Budget Impact

| Resource             | Used | Max                              | Record's cost                 |
| -------------------- | ---- | -------------------------------- | ----------------------------- |
| `Object::Type`       | 31   | 32 (5-bit `TypeMask`)            | 1 slot                        |
| `PackedType`         | 31   | 32 (5-bit)                       | 1 slot                        |
| `verify_t` type bits | 31   | 32 (low half of 64-bit verify_t) | 1 (automatic, mirrors `Type`) |
| `verify_t` flag bits | —    | upper 32 bits                    | 0 (unchanged)                 |
