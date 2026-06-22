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

# Protocols: Technical Reference

Type-dispatched polymorphism for Trix.

---

## 1. What Protocols Are

A protocol defines a set of named methods that types opt into by registering
implementations.  When a method is called, the runtime dispatches to the
correct implementation based on the argument's type -- no conditionals, no
manual type-case chains, no switch statements in user code.

```
% Define a protocol with one method
[/to-str] /Stringify def-protocol

% Register implementations for two types
{ pop (an-int) } /to-str /integer-type def-method
{ pop (a-str) } /to-str /string-type def-method

% Call the method -- dispatches automatically
42 to-str          % => (an-int)
(hello) to-str     % => (a-str)
```

After `def-protocol`, the name `to-str` is bound in protocoldict to an
auto-generated dispatch proc.  It behaves like any other Trix operator:
the user calls it by name, and the runtime handles the type lookup.

### 1.1 Relationship to Existing Type Dispatch

Trix already has `type-case` for inline type dispatch:

```
42 << /integer-type { (int) } /string-type { (str) } >> type-case
```

Protocols extend this into a persistent, extensible registry.  The key
differences:

| Property         | `type-case`               | Protocols                                 |
| ---------------- | ------------------------- | ----------------------------------------- |
| Dispatch dict    | Inline, ephemeral         | Persistent, in protocol registry          |
| Extensibility    | Closed (dict is literal)  | Open (new types can register later)       |
| Method names     | None (anonymous dispatch) | Named, bound in protocoldict              |
| Default fallback | `/default` key in dict    | `def-default-method`                      |
| Discoverability  | None                      | `protocol-methods`, `protocol-satisfies?` |

Protocols are `type-case` promoted to a first-class, named, extensible
mechanism.

### 1.2 Why Protocols Exist

Without protocols, every new type is an island.  If you define a Tagged
variant `/:celsius` and want it to work with a formatting pipeline, you must
modify every formatting function to add a type-case arm.  This creates
coupling: the formatter must know about every possible type.

Protocols invert this dependency.  The formatter defines what it needs
(a `/format` method).  Each type registers its own implementation.  The
formatter and the types never need to know about each other -- only about
the protocol.

This is essential for:

1. **Library interop** -- a data-processing library defines protocols; user
   types opt in without modifying the library.
2. **Tagged ADTs** -- discriminated unions dispatch on their base type
   (`/tagged-type`), then use `tag-match` internally for variant-level
   dispatch.
3. **Record types** -- records opt into protocols alongside primitive types,
   giving structured data the same polymorphic interface as scalars.
4. **Incremental extension** -- add support for a new type without touching
   any existing code.


## 2. Quick Reference

```
def-protocol       method-names protocol-name --
                   % Create protocol, bind dispatch stubs in protocoldict.
                   % method-names: array of literal Names (non-empty).
                   % Error if any method name already claimed or protocol exists.

def-method         proc method-name type-name --
                   % Register implementation for one type on one method.
                   % type-name must be a valid type name (/integer-type, etc.).

def-default-method proc method-name --
                   % Register fallback for types without a specific impl.
                   % Stored under /default in the method's dispatch dict.

extend-protocol    impl-dict type-name protocol-name --
                   % Batch-register: impl-dict maps method-name -> proc.
                   % All keys must be methods of the protocol.
                   % All values must be executable procs.

protocol-satisfies? value protocol-name -- bool
                   % True if value's type has impl (or default) for ALL methods.

protocol-methods   protocol-name -- name-array
                   % Return array of method names defined by the protocol.
```

Type names use the canonical `-type` suffix: `/integer-type`, `/string-type`,
`/boolean-type`, `/real-type`, `/double-type`, `/long-type`, `/ulong-type`,
`/uinteger-type`, `/byte-type`, `/array-type`, `/dict-type`, `/name-type`,
`/null-type`, `/tagged-type`, `/record-type`, `/set-type`, `/coroutine-type`,
`/pipebuffer-type`, `/cell-type`, etc.


## 3. Usage Patterns

### 3.1 Single-Method Protocol

The simplest case: one method, multiple type implementations.

```
[/to-str] /Stringify def-protocol

{ pop (an-int) }  /to-str /integer-type def-method
{ pop (a-str) }   /to-str /string-type def-method
{ pop (a-bool) }  /to-str /boolean-type def-method

42 to-str          % => (an-int)
(hello) to-str     % => (a-str)
true to-str        % => (a-bool)
```

### 3.2 Multi-Method Protocol

A protocol with multiple methods.  Each type must implement all methods to
fully satisfy the protocol, though partial implementation is allowed
(only the implemented methods can be called for that type).

```
[/describe /category] /Describable def-protocol

{ pop (integer) } /describe /integer-type def-method
{ pop (string) }  /describe /string-type def-method
{ pop (num) }     /category /integer-type def-method
{ pop (text) }    /category /string-type def-method

42 describe        % => (integer)
(hi) category      % => (text)
```

### 3.3 Default Methods (Fallbacks)

When most types share the same behavior and only a few need specialization:

```
[/show] /Showable def-protocol
{ pop (unknown) } /show def-default-method       % fallback for all types
{ pop (int-val) } /show /integer-type def-method % specific for Integer

42 show            % => (int-val)   -- uses specific implementation
(hi) show          % => (unknown)   -- uses default
true show          % => (unknown)   -- uses default
3.14 show          % => (unknown)   -- uses default
```

With a default method, `protocol-satisfies?` returns true for all types:

<!-- doctest: skip (continues the /Showable example above) -->
```
42 /Showable protocol-satisfies?     % => true  (specific impl)
(hi) /Showable protocol-satisfies?   % => true  (default impl)
3.14 /Showable protocol-satisfies?   % => true  (default impl)
```

### 3.4 Batch Registration with extend-protocol

When a type implements multiple methods of a protocol, `extend-protocol`
avoids repetitive `def-method` calls:

```
[/repr] /Repr def-protocol

<< /repr { pop (real-repr) } >> /real-type /Repr extend-protocol
<< /repr { pop (double-repr) } >> /double-type /Repr extend-protocol

3.14 repr           % => (real-repr)
3.14d repr          % => (double-repr)
```

For protocols with many methods:

```
[/serialize /deserialize /validate] /Storable def-protocol

<<
  /serialize   { 10 string to-string }
  /deserialize { to-number }
  /validate    { dup 0 ge }
>> /integer-type /Storable extend-protocol
```

### 3.5 Override an Implementation

Calling `def-method` again for the same type replaces the previous
implementation.  The dispatch dict entry is overwritten:

<!-- doctest: skip (continues the /Stringify example above) -->
```
{ pop (v1) } /to-str /integer-type def-method
42 to-str              % => (v1)

{ pop (v2) } /to-str /integer-type def-method
42 to-str              % => (v2)
```

### 3.6 Introspection

<!-- doctest: skip (continues the /Stringify and /Describable examples above) -->
```
/Stringify protocol-methods    % => [/to-str]
/Describable protocol-methods  % => [/category /describe]

42 /Stringify protocol-satisfies?     % => true
3.14 /Stringify protocol-satisfies?   % => false (no Real impl)
```

`protocol-methods` returns the method names in unspecified (hash-bucket)
order, not registration order.

### 3.7 Tagged Value Dispatch

Protocol dispatch uses the value's base type.  For Tagged values, the type
is always `/tagged-type`.  To dispatch on tag name, use `tag-match` inside
the handler:

```
[/fmt-tagged] /TagFmt def-protocol

{
    << /ok    { pop (tagged-ok) }
       /error { pop (tagged-err) }
    >> tag-match
} /fmt-tagged /tagged-type def-method

42 /ok tag fmt-tagged        % => (tagged-ok)
(oops) /error tag fmt-tagged % => (tagged-err)
```

This two-level dispatch pattern (type dispatch via protocol, variant dispatch
via `tag-match`) keeps the protocol dispatch path simple while enabling
fine-grained Tagged variant handling.

### 3.8 Multiple Protocols on the Same Type

Types can implement any number of protocols.  Each protocol's methods are
independently dispatched:

<!-- doctest: skip (continues the /Stringify example above) -->
```
[/render] /Renderable def-protocol
{ pop (rendered) } /render /integer-type def-method

42 to-str    % => (an-int)    -- via /Stringify
42 render    % => (rendered)  -- via /Renderable
```

### 3.9 Protocol-Guarded Functions

Use `protocol-satisfies?` to build defensive APIs:

<!-- doctest: skip (continues the /Renderable example above) -->
```
/safe-render {
    dup /Renderable protocol-satisfies?
    { render }
    { pop (not renderable) }
    if-else
} def

42 safe-render         % => (rendered)
3.14 safe-render       % => (not renderable)
```


## 4. Real-World Scenarios

### 4.1 Serialization Protocol

```
[/to-json /from-json] /JsonCodec def-protocol

% Integer: {"type":"int","value":42}
{
    dup 10 string to-string
    ({"type":"int","value":) exch concat
    (}) concat
    exch pop
} /to-json /integer-type def-method

% String: {"type":"str","value":"hello"}
{
    ({"type":"str","value":") exch concat
    ("}) concat
} /to-json /string-type def-method

% Boolean: {"type":"bool","value":true}
{
    { (true) } { (false) } if-else
    ({"type":"bool","value":) exch concat
    (}) concat
} /to-json /boolean-type def-method
```

### 4.2 Collection Protocol for Generic Algorithms

```
[/first /rest /empty?] /Collection def-protocol

% Array collection
{ 0 get }                     /first /array-type def-method
{ 1 drop }                    /rest /array-type def-method
{ length 0 eq }               /empty? /array-type def-method

% Generic sum using only protocol methods
/collection-sum {
    0 exch                    % acc coll
    { dup empty? not }        % while not empty
    { dup first               % acc coll first
      3 -1 roll add           % coll acc'
      exch rest               % acc' rest
    } while
    pop                       % acc
} def

[1 2 3 4 5] collection-sum   % => 15
```

### 4.3 Display Protocol for Debugging

```
[/display] /Displayable def-protocol
{ pop (?) } /display def-default-method   % fallback

{ 10 string to-string }           /display /integer-type def-method
{ }                                /display /string-type def-method
{ { (true) } { (false) } if-else } /display /boolean-type def-method
{
    % tag-name consumes the tagged value, so dup first to keep it for tag-value
    dup tag-name 20 string to-string
    (:) concat
    exch tag-value display concat
} /display /tagged-type def-method

42 display             % => (42)
true display           % => (true)
42 /ok tag display     % => (ok:42)
```


## 5. Design Choices

### 5.1 Global Method Name Uniqueness

Method names are globally unique across all protocols.  A second
`def-protocol` that claims an already-registered method name raises
`/protocol`.

**Why**: Ambiguity.  If two protocols both define `/format`, what does
`42 format` do?  Global uniqueness eliminates the question.  The cost is
namespace pressure, but Trix's name system is hierarchical -- users can
prefix method names (`/json-format`, `/html-format`) to avoid collisions.

**Alternative considered**: Scoped methods (require protocol qualification:
`42 /Stringify::to-str`).  Rejected -- the whole point of protocols is that
method calls look like regular operators.  Qualification defeats ergonomics.

### 5.2 Type-Only Dispatch (No Multi-Method)

Dispatch is on a single argument's type.  There is no multi-method dispatch
(dispatching on the types of two or more arguments).

**Why**: Simplicity and predictability.  Single dispatch is a constant-time
dict lookup.  Multi-dispatch requires linearization, ambiguity resolution,
and makes error messages confusing.  For the rare case where you need
double dispatch, nest a `type-case` inside the handler.

### 5.3 Base Type Dispatch for Tagged Values

Tagged values dispatch as `/tagged-type`, not on their tag name.  The
handler uses `tag-match` internally for sub-dispatch.

**Why**: The protocol dispatch path is one dict lookup per method call.
Adding tag-name dispatch would require a two-level lookup (type, then tag)
for every dispatch, penalizing all non-Tagged values.  The `tag-match`
inside the handler is the same cost, but only paid when a Tagged value is
actually dispatched.

### 5.4 Dispatch Proc = { dup <dict> type-case }

Each method's dispatch proc is a 3-element array:

```
{ dup <dispatch-dict> type-case }
```

`dup` preserves the value.  `type-case` consumes the copy and the dict,
looks up the value's type, and executes the handler.  The handler receives
the original value on the operand stack.

**Why**: Reuses the existing `type-case` operator.  No new dispatch
machinery.  The dispatch dict is a mutable Dict that grows as types register.
`type-case` already supports `/default` fallback.

### 5.5 Registration Mutates in Place

`def-method` and `extend-protocol` mutate the dispatch dict.  There is no
immutable-protocol-then-freeze pattern.

**Why**: Protocols are defined incrementally.  In a real system, type
implementations arrive across multiple source files.  Requiring all
registrations at definition time would force everything into one file.


## 6. Implementation Internals

### 6.1 Data Structures

The protocol system uses three levels of Dict:

```
m_protocol_registry_offset -> Dict
  |
  +-- /Stringify -> protocol-dict (Dict)
  |     +-- /to-str -> dispatch-dict (Dict)
  |           +-- /integer-type -> { pop (an-int) }
  |           +-- /string-type  -> { pop (a-str) }
  |           +-- /default      -> { pop (?) }       (if def-default-method)
  |
  +-- /Describable -> protocol-dict (Dict)
        +-- /describe -> dispatch-dict (Dict)
        |     +-- /integer-type -> { pop (integer) }
        |     +-- /string-type  -> { pop (string) }
        +-- /category -> dispatch-dict (Dict)
              +-- /integer-type -> { pop (num) }
              +-- /string-type  -> { pop (text) }
```

- **Protocol registry** (`m_protocol_registry_offset`): Top-level Dict.
  Maps protocol names to protocol dicts.  Member variable on the Trix
  instance.  Initialized at startup.  Included in snap-shot/thaw.

- **Protocol dict**: Maps method names to dispatch dicts.  Created with
  `ReadWriteFixed` mode (fixed capacity = method count).  One per protocol.

- **Dispatch dict**: Maps type names to implementation procs.  Created with
  `ReadWriteDynamic` mode (grows as types register).  One per method.

### 6.2 Method Binding

When `def-protocol` is called, for each method name:

1. Create an empty dispatch dict (Dynamic, default capacity).
2. Store it in the protocol dict under the method name.
3. Build a dispatch proc: `{ dup <dispatch-dict> type-case }`.
4. Bind the method name in protocoldict to the dispatch proc.

The dispatch proc is a 3-element ReadOnly executable array.  It
contains the dispatch dict Object directly (element [1]), so the dict
is reachable from protocoldict.  When `def-method` adds an entry to the
dispatch dict, the change is immediately visible to the dispatch proc
because both reference the same Dict offset.

### 6.3 Method Lookup

`protocol_find_dispatch_dict` performs a linear scan of all protocols in
the registry to find which protocol owns a given method name.  This scan
runs at `def-method` time, not at dispatch time.  At dispatch time, the
lookup is a single `type-case` execution -- one Dict `get` operation,
O(1) amortized.

### 6.4 Memory Cost

Per protocol:

| Component                       | Size                                      |
| ------------------------------- | ----------------------------------------- |
| Protocol dict entry in registry | 20 bytes (Dict entry)                     |
| Protocol dict header            | 40+ bytes (Dict with N buckets)           |
| Per-method dispatch dict        | 40+ bytes (grows with type registrations) |
| Dispatch proc (per method)      | 24 bytes (3 Objects)                      |
| protocoldict entry (per method) | 20 bytes (Dict entry)                     |

A protocol with 3 methods and 5 types registered per method:
~40 + 3*(40 + 5*20 + 24 + 20) = ~40 + 3*184 = ~592 bytes.

### 6.5 Dispatch Cost

Method dispatch is:

1. `dup` -- copy top of operand stack (1 Object copy).
2. Push dispatch dict (literal, 1 Object copy).
3. `type-case` -- get type name, hash lookup in dispatch dict, execute
   handler if found.

Total: ~3 operations + 1 hash lookup.  This is comparable to a virtual
method call in C++ (vtable load + indirect call).

### 6.6 Snap-Shot/Thaw

`m_protocol_registry_offset` is saved in `SnapShotHeader` and restored
on thaw.  All protocol dicts and dispatch dicts live on the VM heap, so
they are captured by the snap-shot automatically.  Protocol state survives
a snap-shot/thaw cycle.

### 6.7 Save/Restore

Protocol dispatch dicts are mutable Dicts.  `Dict::put` operations are
journaled by the save/restore system.  If `def-method` is called after
a `save`, `restore` will roll back the registration.  This is the correct
behavior for error recovery (a failed initialization should not leave
partial protocol registrations).

### 6.8 Source File

All protocol operators are in `src/ops_protocol.inl` (~350 lines).
Pre-interned names and the registry offset are in `src/member_vars.inl`.
Initialization is in `src/init.inl`.


## 7. Composability

### 7.1 Protocols + Records

Records are the natural "struct" for protocol implementations:

```
/point [/x /y] record-type def

[/magnitude] /Measurable def-protocol
{
    dup /x get dup mul
    exch /y get dup mul
    add sqrt
} /magnitude /record-type def-method

3.0 4.0 point magnitude    % => 5.0
```

(Use Real coordinates: `sqrt` requires a Real/Double, and `3*3 + 4*4` from
Integer inputs would be the Integer `25`, which `sqrt` rejects with
`/type-check`.)

### 7.2 Protocols + Pattern Matching

Use `protocol-satisfies?` as a match predicate:

<!-- doctest: skip (illustrative; placeholder value + undefined protocols/process helpers) -->
```
value [
    { dup /Stringify protocol-satisfies? } { to-str process-string }
    { dup /Renderable protocol-satisfies? } { render process-rendered }
    { pop true } { pop (unknown) }
] match
```

### 7.3 Protocols + Closures

Capture protocol state in closures for callback patterns:

<!-- doctest: skip (continues the /Stringify example above) -->
```
/formatter {
    /prefix exch def
    { prefix exch to-str concat } [/prefix] closure-capture
} def

/fmt (>> ) formatter def
42 fmt            % => (>> an-int)
```

### 7.4 Protocols + Transducers

Protocol methods compose naturally with transducers:

```
[1 2 3 4 5] { to-str } xf-map into
% => [(an-int) (an-int) (an-int) (an-int) (an-int)]
```

### 7.5 Protocols + GenServer

GenServer handlers can dispatch via protocols to process heterogeneous
messages polymorphically:

```
/handle-message {
    dup /Processable protocol-satisfies?
    { process }
    { pop /unhandled }
    if-else
} def
```


## 8. Error Handling

| Error             | Condition                                                                              |
| ----------------- | -------------------------------------------------------------------------------------- |
| `/protocol`       | Duplicate protocol name (`def-protocol`)                                               |
| `/protocol`       | Method name already claimed by another protocol (`def-protocol`)                       |
| `/protocol`       | Method not part of any protocol (`def-method`, `def-default-method`)                   |
| `/protocol`       | Protocol does not exist (`extend-protocol`, `protocol-satisfies?`, `protocol-methods`) |
| `/protocol`       | Method not part of specified protocol (`extend-protocol`)                              |
| `/type-check`     | Invalid type name (`def-method`, `extend-protocol`)                                    |
| `/type-check`     | Non-name in method-names array (`def-protocol`)                                        |
| `/type-check`     | Non-proc value in impl-dict (`extend-protocol`)                                        |
| `/range-check`    | Empty method-names array (`def-protocol`)                                              |
| `/undefined-case` | No implementation for the value's type (dispatch)                                      |

The dispatch error (`/undefined-case`) comes from `type-case`, not from
protocol-specific code.  This is because the dispatch proc is literally
`{ dup <dict> type-case }` -- `type-case` raises the error when no matching
key is found.


## 9. Limitations

- **No multi-method dispatch.** Dispatch is on a single value's type.
  For multi-argument dispatch, use `type-case` or nested `match` inside
  the handler.

- **No protocol inheritance.** Protocols are flat.  A protocol cannot
  "extend" another protocol.  Compose by having types implement multiple
  protocols.

- **Method names consume protocoldict space.** Each method name is a
  protocoldict entry.  The protocoldict is ReadWriteDynamic and grows as
  needed, so capacity is not a concern.

- **No method removal.** Once registered, a method implementation cannot be
  removed (only overwritten with a new one).  Protocol definitions
  themselves cannot be removed.

- **Tagged dispatch is two-level.** There is no automatic dispatch on
  tag names.  Handlers for `/tagged-type` must do their own tag-name
  dispatch via `tag-match`.
