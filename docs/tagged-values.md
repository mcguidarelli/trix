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

# Tagged Values in Trix

Tagged values are discriminated pairs: a Name tag and an arbitrary payload,
stored as two contiguous Objects in VM memory.  They are the foundation for
algebraic data types, pattern dispatch, railway-oriented error handling,
protocol modeling, and actor-like message processing in Trix.

This document covers the complete Tagged value system: all 11 operators, five
major use case families with worked examples, internal architecture, and
design rationale.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [Tutorial](#3-tutorial)
4. [Discriminated Unions](#4-discriminated-unions)
5. [Exhaustive Case Analysis](#5-exhaustive-case-analysis)
6. [Railway-Oriented Error Handling](#6-railway-oriented-error-handling)
7. [Protocol Modeling](#7-protocol-modeling)
8. [Actor-Like Patterns](#8-actor-like-patterns)
9. [Patterns Cookbook](#9-patterns-cookbook)
10. [Architecture](#10-architecture)
11. [Comparison with Other Languages](#11-comparison-with-other-languages)
12. [Design Decisions](#12-design-decisions)

---

## 1. Overview

A tagged value pairs a **Name** (the tag) with a **payload** (any Trix type).
The Name identifies the variant; the payload carries the data.  Together they
form a discriminated union -- a single value that can be one of several named
alternatives, each carrying its own data.

```
42 /some tag          % Some(42)    -- the value 42, tagged as "some"
null /none tag        % None        -- null, tagged as "none"
(timeout) /err tag    % Err(timeout) -- the string "timeout", tagged as "err"
```

Tagged values are always **Literal** (never executable).  They are data, not
procedures.  When the interpreter encounters a tagged value on the execution
stack, it pushes it to the operand stack unchanged.

The tag is always a Name.  Names are immutable, interned strings with cached
hash values.  Comparison is O(1) by offset equality after interning, making
dispatch efficient.

The payload is any Trix type: scalars (Integer, Long, Double, Boolean, Null),
composites (String, Array, Dict, Set), other tagged values (for recursive
types), Curry, Thunk, or ExtValue types (Long, ULong, Double, Address).  There
are no restrictions on payload type.

---

## 2. Quick Reference

### Operators

| Operator       | Stack Effect                   | Description                                             |
| -------------- | ------------------------------ | ------------------------------------------------------- |
| `tag`          | `value name -- tagged`         | Create a tagged value                                   |
| `untag`        | `tagged -- value name`         | Decompose into payload and tag                          |
| `tag-name`     | `tagged -- name`               | Extract the tag name                                    |
| `tag-value`    | `tagged -- value`              | Extract the payload                                     |
| `tag?`         | `tagged name -- bool`          | Test if tag matches name                                |
| `is-tagged`    | `any -- bool`                  | Type predicate                                          |
| `tag-match`    | `tagged dict -- ...`           | Dispatch on tag name                                    |
| `tag-update`   | `tagged proc -- tagged'`       | Apply proc to payload, preserve tag                     |
| `tag-value-or` | `tagged name default -- value` | Payload if tag matches, else default                    |
| `tag-bind`     | `tagged name proc -- tagged`   | Monadic bind: unwrap + exec if match, else pass through |
| `try-result`   | `proc -- tagged`               | Execute proc; `/ok value` or `/err error-name`          |

### Type Properties

- Always Literal.  `make-executable` raises `type-check`.
- `type` returns `/tagged-type`.
- `is-tagged` returns `true`; all other `is-*` predicates return `false`.
- `deep-eq` compares both tag name and payload recursively.
- `eq` compares by identity (same VM offset), not structural equality.
- Packed encoding shares `PackedType::Curry` with X-bit = 0.
- Participates in `save`/`restore` and `snap-shot`/`thaw`.

---

## 3. Tutorial

### 3.1 Creating Tagged Values

`tag` pops a Name and a value, allocates a 2-Object pair in VM memory, and
pushes a tagged value:

```
42 /answer tag              % Integer 42, tagged as /answer
(hello) /greeting tag       % String, tagged as /greeting
[1 2 3] /data tag           % Array, tagged as /data
100l /big tag               % Long (ExtValue), tagged as /big
```

The tag must be a Name.  Other types raise `type-check`:

```
{ 42 (not-a-name) tag } try     % => /type-check
{ 42 5 tag } try                % => /type-check
```

### 3.2 Inspecting Tagged Values

Four operators extract information from a tagged value:

```
42 /answer tag

dup tag-name =        % => /answer  (the tag)
dup tag-value =       % => 42       (the payload)
dup /answer tag? =    % => true     (does the tag match?)
dup /other tag? =     % => false
dup is-tagged =       % => true     (type predicate)

untag                 % => 42 /answer  (both on the stack)
```

**Field-access sugar:** the scanner's `.field` syntax also works on tagged
values via two pseudo-fields -- `/tag` and `/value`:

```
42 /answer tag /t exch def

t .tag                % => /answer  (equivalent to tag-name)
t .value              % => 42       (equivalent to tag-value)
```

The sugar chains naturally through nested composites:

```
42 /ok tag /response exch def
response .value                       % => 42  (tagged's payload)

/point [ /x /y ] record-type def
3 4 point /result tag .value .x       % => 3   (tagged -> record.x)
```

Any other key raises `undefined` ("expected /tag or /value").  The classic
`tag-name` / `tag-value` / `untag` operators remain the preferred form for
deep destructuring or pattern-match-style code; `.tag` / `.value` are the
ergonomic choice when tagged values appear mid-pipeline alongside records
and dicts that use the same dotted syntax.

### 3.3 Pattern Dispatch with tag-match

`tag-match` looks up the tag name in a dict and executes the matching
handler.  The handler receives the payload on the operand stack:

```
42 /ok tag
<< /ok { 2 mul } /err { pop -1 } >> tag-match
% => 84  (payload 42 passed to { 2 mul })
```

If no handler matches, `tag-match` tries the `/default` key.  The default
handler receives the full tagged value (not just the payload):

```
42 /unknown tag
<< /ok { 2 mul } /default { tag-name } >> tag-match
% => /unknown  (default handler got the full tagged, extracted name)
```

If neither a specific handler nor `/default` exists, `tag-match` raises
`undefined`:

```
{ 42 /unknown tag << /ok { } >> tag-match } try
% => /undefined
```

### 3.4 Functional Update with tag-update

`tag-update` applies a procedure to the payload and wraps the result in the
same tag.  The tag name is preserved; only the payload changes:

```
42 /ok tag { 1 add } tag-update
% => /ok 43 (tagged)

(hello) /msg tag { uppercase } tag-update
% => /msg (HELLO) (tagged)
```

This is the "fmap" pattern from functional programming: transform the content
without changing the container.

### 3.5 Unwrap with Default via tag-value-or

`tag-value-or` extracts the payload if the tag matches, otherwise returns a
default value:

```
42 /some tag /some 0 tag-value-or       % => 42 (matched, payload returned)
null /none tag /some 0 tag-value-or     % => 0  (not matched, default returned)
```

This is the standard "unwrap or default" operation for Option types.

### 3.6 Monadic Chaining with tag-bind

`tag-bind` is the monadic bind (flatMap / `>>=`).  If the tag matches the
given name, it unwraps the payload and executes the procedure, which must
return a tagged value.  If the tag does not match, the original tagged value
passes through unchanged:

```
42 /ok tag /ok { 1 add /ok tag } tag-bind
% => /ok 43 (matched: payload unwrapped, proc ran, returned new tagged)

42 /ok tag /err { pop 0 /err tag } tag-bind
% => /ok 42 (not matched: original passes through unchanged)
```

This enables chaining operations where errors short-circuit:

```
{ 10 } try-result
/ok { { 2 mul } try-result } tag-bind     % => /ok 20
/ok { { 1 add } try-result } tag-bind     % => /ok 21
```

### 3.7 Exception-to-Result Bridge with try-result

`try-result` executes a procedure and wraps the outcome as a tagged Result:
`/ok value` on success, `/err error-name` on failure.  On error, the operand
stack is rolled back to its pre-call depth:

```
{ 6 7 mul } try-result       % => /ok 42 (tagged)
{ 1 0 div } try-result       % => /err /div-by-zero (tagged)
```

The proc must leave exactly one value on the operand stack on success.

---

## 4. Discriminated Unions

A discriminated union (algebraic data type, sum type) is a type that can be
one of several named variants, each carrying its own data.  Tagged values
implement this directly: the tag name identifies the variant, the payload
carries the data.

### 4.1 Option Type

The simplest discriminated union: a value that is either present or absent.

```
% Convention: /some carries the value, /none carries null
/make-some { /some tag } def
/make-none { null /none tag } def

% Check and extract
/is-some { /some tag? } def
/is-none { /none tag? } def

% Unwrap with default
/unwrap-or { /some exch tag-value-or } def

% Usage
42 make-some             % => /some 42
make-none                % => /none null
42 make-some 0 unwrap-or % => 42
make-none 0 unwrap-or    % => 0
```

### 4.2 Result Type

Either a success value or an error:

```
/make-ok { /ok tag } def
/make-err { /err tag } def

% Dispatch
/handle-result {
    << /ok { (Success: ) print = }
       /err { (Error: ) print = }
    >> tag-match
} def

42 make-ok handle-result        % prints: Success: 42
/timeout make-err handle-result % prints: Error: timeout
```

### 4.3 Shape Type (Multi-Variant with Different Payloads)

A shape can be a circle (radius), rectangle (width x height), or triangle
(base x height):

```
% Constructors — payload is a dict for multi-field variants.
% Use locals (|name ...|) to bind each field, then build the dict directly.
/make-circle { |radius| << /radius radius >> /circle tag } def
/make-rect { |width height| << /width width /height height >> /rect tag } def
/make-tri { |base height| << /base base /height height >> /tri tag } def

% Area computation via tag-match
/area {
    << /circle { dup /radius get dup mul 3.14159 mul }
       /rect   { dup /width get exch /height get mul }
       /tri    { dup /base get exch /height get mul 2 div }
    >> tag-match
} def

5.0r make-circle area         % => ~78.54  (Real radius; circle area is float)
4 3 make-rect area            % => 12
6 4 make-tri area             % => 12
```

### 4.4 Expression Trees (Recursive Tagged Structures)

Tagged values can contain other tagged values as payloads, enabling recursive
data structures like expression trees:

```
% AST: Num(n) | Add(left, right) | Mul(left, right) | Neg(expr)
/make-num { /num tag } def
/make-add { |l r| [ l r ] /add tag } def
/make-mul { |l r| [ l r ] /mul tag } def
/make-neg { /neg tag } def

% Evaluator
/eval-expr {
    << /num { }
       /add { dup 0 get eval-expr exch 1 get eval-expr add }
       /mul { dup 0 get eval-expr exch 1 get eval-expr mul }
       /neg { eval-expr 0 exch sub }
    >> tag-match
} def

% Build: (3 + 4) * -(2)
3 make-num 4 make-num make-add
2 make-num make-neg
make-mul
eval-expr       % => -14  ((3+4) * -2)
```

### 4.5 Linked Lists (Recursive Union)

A linked list is either empty (`/nil`) or a cons cell (`/cons [head, tail]`):

```
/nil { null /nil tag } def
/cons { |h t| [ h t ] /cons tag } def

% Build: [1, 2, 3]
3 nil cons 2 exch cons 1 exch cons

% Sum a list (tail-recursive). The /cons handler receives the [head tail]
% payload array; recurse with acc below, the tail on top (to match |acc lst|).
/list-sum { |acc lst|
    lst << /nil  { pop acc }
           /cons {
               dup 0 get acc add    % arr (head + acc)
               exch 1 get           % (head + acc) tail
               list-sum             % acc below, lst (tail) on top
           }
        >> tag-match
} def
0 exch list-sum     % => 6
```

---

## 5. Exhaustive Case Analysis

### 5.1 How tag-match Handles Missing Cases

`tag-match` enforces case analysis at runtime through three tiers:

1. **Exact match**: tag name is a key in the dispatch dict.  Handler executes
   with the payload on the stack.

2. **Default fallback**: tag name is not found, but `/default` exists in the
   dict.  Default handler executes with the full tagged value on the stack.

3. **No match**: neither specific handler nor `/default`.  Raises `undefined`
   error with the message `tag-match: no handler for tag /X and no /default`.

This means that omitting `/default` makes `tag-match` act as an exhaustive
check: any variant not explicitly listed produces a hard error.

### 5.2 Closed Unions (Exhaustive by Omission)

When you know all possible variants, omit `/default` to enforce that every
variant is handled:

```
% Shape must be /circle, /rect, or /tri — anything else is an error
/area {
    << /circle { dup /radius get dup mul 3.14159 mul }
       /rect   { dup /width get exch /height get mul }
       /tri    { dup /base get exch /height get mul 2 div }
    >> tag-match
} def

% This works:
5 make-circle area     % => ~78.54

% This raises undefined:
{ 42 /hexagon tag area } try    % => /undefined
```

The absence of `/default` acts as a static-like exhaustiveness guarantee: if
you add a new shape variant, every `tag-match` without a handler for it will
fail at runtime with a clear diagnostic.

### 5.3 Open Unions (Extensible with /default)

When variants may be extended by callers, include `/default`:

```
% Plugin system: core handles known messages, /default for extensions
/handle-message {
    << /ping  { pop (pong) }
       /echo  { }
       /default { tag-name 16 string to-string (unknown message: /) exch concat }
    >> tag-match
} def

(hello) /echo tag handle-message     % => (hello)
null /ping tag handle-message        % => (pong)
42 /custom tag handle-message        % => (unknown message: /custom)
```

### 5.4 Exhaustiveness in Practice

Trix is dynamically typed.  There is no compile-time type checker to verify
that all variants are covered.  Instead, Trix provides runtime enforcement:

| Strategy            | Mechanism                      | Behavior on Missing Case   |
| ------------------- | ------------------------------ | -------------------------- |
| Closed union        | `tag-match` without `/default` | `undefined` error          |
| Open union          | `tag-match` with `/default`    | Default handler runs       |
| Explicit validation | `tag?` before dispatch         | Caller checks              |
| Set membership      | Check `tag-name` against a set | Validation before dispatch |

For critical code, you can validate before dispatching:

```
/valid-shapes {{ /circle /rect /tri }} def

/validate-shape {
    dup tag-name valid-shapes exch set-member? not
    { (invalid shape) false assert } if
} def
```

---

## 6. Railway-Oriented Error Handling

Railway-oriented programming models a computation as a pipeline where each
step either succeeds (stays on the "success track") or fails (switches to the
"error track").  Once on the error track, subsequent steps are skipped.

### 6.1 The Railway Model

```
Success:  [step1] --ok--> [step2] --ok--> [step3] --ok--> result
Error:    [step1] --err--> ................bypass............--> error
```

In Trix, the success track carries `/ok value` and the error track carries
`/err error-name`.  `tag-bind` is the switch that routes values along the
tracks:

- If the value is `/ok`, `tag-bind` unwraps the payload, runs the next step,
  and returns whatever that step produces (either `/ok` or `/err`).
- If the value is `/err`, `tag-bind` passes it through unchanged.

### 6.2 Basic Pipeline

```
% Three steps that can each fail
/parse-int {
    % string -- integer (may raise type-check)
    { ({:d}) mark 0 sscan-fmt pop } try-result
} def

/validate-positive {
    % integer -- integer (may raise range-check)
    { dup 0 le { /range-check throw } if } try-result
} def

/double-it {
    { 2 mul } try-result
} def

% Pipeline: parse -> validate -> double
(42) parse-int
/ok { validate-positive } tag-bind
/ok { double-it } tag-bind
% => /ok 84

% Pipeline with invalid input
(-5) parse-int
/ok { validate-positive } tag-bind
/ok { double-it } tag-bind
% => /err /range-check  (validate failed, double-it was skipped)
```

### 6.3 Comparison: try/if-else vs Railway

Without railway operators, error handling requires nested conditionals:

```
% Without railway: nested try/if-else
/process-traditional {
    { step1 } try
    dup /no-error eq {
        pop
        { step2 } try
        dup /no-error eq {
            pop
            { step3 } try
            dup /no-error eq {
                pop     % success: result on stack
            } {
                handle-error
            } if-else
        } {
            handle-error
        } if-else
    } {
        handle-error
    } if-else
} def

% With railway: flat pipeline
/process-railway {
    { step1 } try-result
    /ok { { step2 } try-result } tag-bind
    /ok { { step3 } try-result } tag-bind
    << /ok { }
       /err { handle-error }
    >> tag-match
} def
```

The railway version is flat regardless of pipeline length.  Adding a fourth
step is one line, not another nesting level.

### 6.4 Extracting Results

At the end of a pipeline, extract the result:

<!-- doctest: skip (synopsis: pipeline/transform/format-output are stand-ins) -->
```
% Option 1: tag-match (handle both cases explicitly)
{ pipeline } try-result
/ok { { transform } try-result } tag-bind
<< /ok { (got: ) print = }
   /err { (failed: ) print = }
>> tag-match

% Option 2: tag-value-or (unwrap with default)
{ pipeline } try-result
/ok { { transform } try-result } tag-bind
/ok 0 tag-value-or          % => result or 0

% Option 3: tag-update (transform the success value)
{ pipeline } try-result
/ok { { transform } try-result } tag-bind
{ format-output } tag-update     % => /ok formatted or /err unchanged
```

### 6.5 Real-World Example: User Registration Pipeline

```
/validate-username {
    % string -- string
    { dup length dup 3 lt exch 20 gt or
        { /range-check throw } if
    } try-result
} def

/validate-email {
    % string -- string
    { dup (@) contains? not
        { /type-check throw } if
    } try-result
} def

/create-user {
    % username email -- /ok << dict >>  (bind fields with locals, tag /ok)
    |username email| << /username username /email email >> /ok tag
} def

% Register: validate both fields, then create
/register {
    % username email -- /ok dict or /err error-name
    /email exch def
    validate-username
    /ok { email create-user } tag-bind
} def

% Success case
(alice) (alice@example.com) register
% => /ok << /username (alice) /email (alice@example.com) >>

% Failure case: username too short
(ab) (ab@example.com) register
% => /err /range-check
```

---

## 7. Protocol Modeling

Protocols are sequences of states with defined transitions.  Tagged values
model each state as a variant, with the payload carrying state-specific data,
and `tag-match` enforcing valid transitions.

### 7.1 Traffic Light (Simple Cyclic Protocol)

```
/next-light {
    << /red    { pop null /green tag }
       /green  { pop null /yellow tag }
       /yellow { pop null /red tag }
    >> tag-match
} def

null /red tag
next-light dup tag-name =     % => /green
next-light dup tag-name =     % => /yellow
next-light dup tag-name =     % => /red  (cycle complete)
```

### 7.2 TCP Connection (Stateful Protocol)

A simplified TCP state machine with data payloads tracking connection state.

The canonical state-machine idiom is: **capture both operands (`state` and
`event`) into locals with a `|state event|` preamble, then dispatch on the
tagged value that drives behavior** -- here the `state` -- with `tag-match`.
Because both operands are bound to locals first, each handler can freely reach
the other one (the `event`) by name regardless of what is on the stack.  This
avoids the `exch`-then-`tag-match` trap, where dispatching on the state leaves
the event buried below the state's payload.

```
% State constructors carry connection data; build payload dicts via locals.
/tcp-closed { null /closed tag } def
/tcp-listen { |port| << /port port >> /listen tag } def
/tcp-syn-sent { |ip port| << /dest-ip ip /dest-port port >> /syn-sent tag } def
/tcp-established { |ip port| << /dest-ip ip /dest-port port /seq 0 >> /established tag } def
/tcp-fin-wait { |seq| << /seq seq >> /fin-wait tag } def

% Transitions: state + event -> new state
/tcp-transition { |state event|
    % Dispatch on the STATE; each arm receives the state's payload (dropped),
    % then dispatches on the EVENT (a tagged value held in the local).
    state
    << /closed {
            pop  event
            << /passive-open { pop 8080 tcp-listen }
               /active-open  { pop (10.0.0.1) 80 tcp-syn-sent }
            >> tag-match
       }
       /listen {
            pop  event
            << /syn-received { pop (10.0.0.1) 80 tcp-established }
               /close        { pop tcp-closed }
               /default      { pop state }
            >> tag-match
       }
       /syn-sent {
            pop  event
            << /syn-ack { pop state tag-value dup /dest-ip get exch /dest-port get tcp-established }
               /timeout { pop tcp-closed }
            >> tag-match
       }
       /established {
            pop  event
            << /close { pop state tag-value /seq get tcp-fin-wait }
               /data  { pop state tag-value /seq get 1 add
                        << exch /seq exch >> /established tag }
               /default { pop state }
            >> tag-match
       }
       /fin-wait {
            pop  event
            << /ack     { pop tcp-closed }
               /timeout { pop tcp-closed }
            >> tag-match
       }
    >> tag-match
} def

% Walk through a connection lifecycle.  Events are tagged values (null payload).
tcp-closed
null /passive-open tag tcp-transition % => /listen << /port 8080 >>
null /syn-received tag tcp-transition % => /established << /dest-ip ... /seq 0 >>
null /data tag tcp-transition         % => /established << /seq 1 >>
null /data tag tcp-transition         % => /established << /seq 2 >>
null /close tag tcp-transition        % => /fin-wait << /seq 2 >>
null /ack tag tcp-transition          % => /closed null
```

### 7.3 Parser State Machine

A tokenizer that processes characters through a state machine.  The `char` is
a byte (string indexing yields bytes), tested with `digit?` / `alpha?`.  Same
idiom as §7.2: a `|state char|` preamble captures both operands, then we
dispatch on the `state` and read `char` by name inside each arm.

```
% States: /start, /in-number << /digits [..] >>, /in-word << /chars [..] >>, /done
/tokenize-char { |state char|
    state
    << /start {
            pop
            char digit?
            { [ char ] << exch /digits exch >> /in-number tag }
            { char alpha?
              { [ char ] << exch /chars exch >> /in-word tag }
              { null /start tag }
              if-else }
            if-else
       }
       /in-number {
            % arm receives the state's payload dict (<< /digits [..] >>)
            char digit?
            { /digits get char append << exch /digits exch >> /in-number tag }
            { pop null /done tag }
            if-else
       }
       /in-word {
            char alpha?
            { /chars get char append << exch /chars exch >> /in-word tag }
            { pop null /done tag }
            if-else
       }
       /done { pop null /done tag }
    >> tag-match
} def

% Feed "42x": '4' '2' accumulate as digits, then 'x' (non-digit) ends the number.
% (string indexing yields bytes, so /digits collects the byte for each '4','2'.)
null /start tag
(4) 0 get tokenize-char        % => /in-number << /digits [ <byte '4'> ] >>
(2) 0 get tokenize-char        % => /in-number << /digits [ <byte '4'> <byte '2'> ] >>
(x) 0 get tokenize-char        % => /done null  (non-digit ends the token)
```

### 7.4 Request/Response Protocol

Model a request-response protocol where each state carries context:

```
% Protocol states
% /idle -- no active request
% /pending << /method /path /timeout >> -- request sent, awaiting response
% /complete << /status /body >> -- response received

/send-request {
    % idle method path -- pending  (bind fields with locals; drop the idle marker)
    |idle method path| << /method method /path path /timeout 30 >> /pending tag
} def

/receive-response {
    % pending status body -- complete  (drop the pending marker)
    |pending status body| << /status status /body body >> /complete tag
} def

/handle-response {
    << /complete {
            dup /status get 200 eq
            { /body get /ok tag }
            { /status get /err tag }
            if-else
       }
       /default { pop (not ready) /err tag }
    >> tag-match
} def
```

---

## 8. Actor-Like Patterns

An actor is an entity that processes messages one at a time, maintains internal
state, and can change its behavior in response to messages.  Trix is
single-threaded, so these patterns model **sequential actors** -- the
computational pattern without concurrency.

### 8.1 Basic Actor: Counter

An actor takes `state message` and returns a new state (and any reply).  The
same idiom as the protocol state machines applies: a `|state message|`
preamble binds both operands, so a handler can read the `state` by name while
`tag-match` dispatches on the `message`.  This actor has a single state shape
(`/counter`), so behavior depends only on the message -- we dispatch on the
message and read the count from `state` directly.

```
% Actor state is a tagged value; messages are tagged values
% State: /counter << /count N >>
% Messages: /increment null, /decrement null, /get null, /reset N

/counter-actor { |state message|
    message
    << /increment { pop
            state tag-value /count get 1 add
            << exch /count exch >> /counter tag   null }
       /decrement { pop
            state tag-value /count get 1 sub
            << exch /count exch >> /counter tag   null }
       /get   { pop  state  state tag-value /count get }   % state' below, count on top
       /reset { << exch /count exch >> /counter tag   null }
    >> tag-match
} def

% Create actor, send messages
<< /count 0 >> /counter tag         % initial state

null /increment tag counter-actor pop   % state: count=1
null /increment tag counter-actor pop   % state: count=2
null /increment tag counter-actor pop   % state: count=3
null /decrement tag counter-actor pop   % state: count=2
null /get tag counter-actor             % => state' (count=2) and 2 on top
```

### 8.2 Behavior-Changing Actor

An actor that changes its message handling based on state.  Here behavior
genuinely depends on the current mode, so we dispatch on the **state** (outer
`tag-match`) and then on the **message** (inner `tag-match`).  The `|state
message|` preamble makes both available in every arm: the inner arm receives
the message's payload, while the state is reachable by name.

```
% Thermostat: behavior depends on current mode
% States: /heating << /target T >>, /cooling << /target T >>, /idle null
% Messages: /temp N, /set-target N, /query null

/thermostat-actor { |state message|
    state
    << /idle {
            pop  message
            << /temp       { pop  state }                           % stay idle
               /set-target { << exch /target exch >> /heating tag } % payload N -> target
               /query      { pop  state }
            >> tag-match
       }
       /heating {
            pop  message
            << /temp { state tag-value /target get  ge      % payload N >= target?
                       { null /idle tag }                   % reached target -> idle
                       { state } if-else }                  % else stay heating
               /set-target { << exch /target exch >> /heating tag }
               /query { pop  state }
            >> tag-match
       }
       /cooling {
            pop  message
            << /temp { state tag-value /target get  le
                       { null /idle tag }
                       { state } if-else }
               /set-target { << exch /target exch >> /cooling tag }
               /query { pop  state }
            >> tag-match
       }
    >> tag-match
} def

% idle --set-target 70--> heating;  temp 65 stays heating;  temp 72 reaches idle
null /idle tag
70 /set-target tag thermostat-actor       % => /heating << /target 70 >>
65 /temp tag thermostat-actor             % 65 >= 70? no  => still /heating
72 /temp tag thermostat-actor             % 72 >= 70? yes => /idle null
```

### 8.3 Message Queue Processing

Process a queue of tagged messages against an actor by folding each message
through it.  `for-all` pushes each element on top of the running state, which
is exactly the `state message` order `thermostat-actor` expects -- no `exch`
is needed.

```
/process-mailbox { |state messages|
    state                                  % seed the running state
    messages { thermostat-actor } for-all  % fold each message in turn
} def

% Start idle, set the target to 70, then feed temperatures.
null /idle tag
[ 70 /set-target tag      % => /heating << /target 70 >>
  65 /temp tag            % 65 < 70 => still /heating
  72 /temp tag            % 72 >= 70 => /idle  (drains to idle)
] process-mailbox
% => /idle null
```

### 8.4 Limitations

Trix is a single-threaded, deterministic interpreter.  The actor patterns
shown here are sequential -- they model the actor's computational behavior
(message dispatch, state evolution, behavior change) without concurrency.

What is **not supported**:
- Concurrent actors running in parallel
- Asynchronous message passing between actors
- Mailbox queuing with backpressure
- Supervision trees

These limitations are by design.  Trix is an embedded scripting language
optimized for deterministic execution.  The host C++ application handles
concurrency; Trix scripts model the logic.  For concurrent actor systems,
the host would create multiple Trix instances (each with its own VM) and
manage message passing at the C++ level.

---

## 9. Patterns Cookbook

### 9.1 Validation Pipeline

Validate multiple fields and collect all errors, not just the first:

```
/validate-field {
    % value validator-proc -- /ok value | /err message
    try-result
} def

/validate-all {
    % value validators -- /ok value | /err [errors]
    /validators exch def
    /val exch def
    /errors [] def

    validators {
        val exch validate-field
        /err tag? { tag-value /errors exch errors exch append def } if
    } for-all

    errors length 0 eq
    { val /ok tag }
    { errors /err tag }
    if-else
} def
```

### 9.2 Option Chaining (Safe Navigation)

Navigate nested structures where any level might be absent:

```
% safe-get: dict key -- /some value | /none null
/safe-get {
    2 dup-n known?
    { get /some tag } { pop pop null /none tag } if-else
} def

% Chain: config -> database -> host
/get-db-host {
    /some { /database safe-get } tag-bind
    /some { /host safe-get } tag-bind
} def

% With data
<< /database << /host (localhost) /port 5432 >> >> /some tag
get-db-host
% => /some (localhost)

% Without nested key
<< /database << /port 5432 >> >> /some tag
get-db-host
% => /none null  (host key missing, short-circuited)

% Without database key
<< /cache << >> >> /some tag
get-db-host
% => /none null  (database key missing, short-circuited at first step)
```

### 9.3 Command Pattern (Undo/Redo)

Commands as tagged values with execute and undo operations.  The state here is
a plain number; the command is the tagged value.  We dispatch on the
**command** -- the `|state command|` preamble re-pushes `state` so the matched
arm has `state value` on the stack for the arithmetic.

```
% Commands carry their operand value (single-field dict: value << exch /value exch >>)
/make-add-cmd { << exch /value exch >> /add-cmd tag } def
/make-mul-cmd { << exch /value exch >> /mul-cmd tag } def

/execute-cmd { |state command|
    state                                  % re-push the running state
    command
    << /add-cmd { /value get add }         % arm gets command payload; state below
       /mul-cmd { /value get mul }
    >> tag-match
} def

/undo-cmd { |state command|
    state
    command
    << /add-cmd { /value get sub }         % inverse of add
       /mul-cmd { /value get div }         % inverse of mul
    >> tag-match
} def

% Execute a sequence
0
10 make-add-cmd execute-cmd     % => 10
3 make-mul-cmd execute-cmd      % => 30
5 make-add-cmd execute-cmd      % => 35

% Undo last two
5 make-add-cmd undo-cmd         % => 30
3 make-mul-cmd undo-cmd         % => 10
```

### 9.4 Event Sourcing

Events as tagged values applied to state.  State is `/cart << /items [...] >>`
where each item is `<< /id N /name S >>`.  The `|state event|` preamble binds
both, then we dispatch on the **event**; the running cart is read from `state`.
Multi-field event payloads are built directly with a `|...|` constructor
(`<< /id id /name name >>`), avoiding the broken `4 -1 roll` dict idiom.

```
% Event constructors (multi-field payloads built via locals)
/item-added-event { |id name| << /id id /name name >> /item-added tag } def
/item-removed-event { |id| << /id id >> /item-removed tag } def
% /cart-cleared carries null

/apply-event { |state event|
    state
    event
    << /item-added {
            % payload = << /id N /name S >> (the new item); append to /items
            /item exch def
            state tag-value /items get  item append
            << exch /items exch >> /cart tag
       }
       /item-removed {
            % payload = << /id N >>; keep items whose id differs
            /id get  /target-id exch def
            state tag-value /items get
            { /id get target-id ne } filter
            << exch /items exch >> /cart tag
       }
       /cart-cleared {
            pop  << /items [ ] >> /cart tag
       }
    >> tag-match
} def

% Replay a sequence of events onto an empty cart
<< /items [ ] >> /cart tag
1 (apple)  item-added-event apply-event    % => /cart << /items [ {1,apple} ] >>
2 (banana) item-added-event apply-event    % => /cart << /items [ {1,apple} {2,banana} ] >>
1 item-removed-event apply-event           % => /cart << /items [ {2,banana} ] >>
null /cart-cleared tag apply-event         % => /cart << /items [ ] >>
```

### 9.5 Interpreter Pattern (Mini Language)

A simple calculator language with tagged AST nodes:

```
% Nodes: /lit N, /add [l r], /sub [l r], /if-pos [test then else]
/calc-eval {
    << /lit { }
       /add { dup 0 get calc-eval exch 1 get calc-eval add }
       /sub { dup 0 get calc-eval exch 1 get calc-eval sub }
       /if-pos {
            dup 0 get calc-eval
            0 gt
            { 1 get calc-eval }
            { 2 get calc-eval }
            if-else
       }
    >> tag-match
} def

% Build: if (5 - 3) > 0 then (10 + 20) else (0)
[ [ 5 /lit tag 3 /lit tag ] /sub tag
  [ 10 /lit tag 20 /lit tag ] /add tag
  0 /lit tag
] /if-pos tag
calc-eval       % => 30
```

---

## 10. Architecture

### 10.1 Memory Layout

A tagged value is an 8-byte Object with a `vm_offset_t` pointing to a
2-Object pair allocated contiguously in VM memory:

```
Object (8 bytes):
  m_aat:    LiteralAttrib | Type::Tagged   (1 byte)
  m_object_save_level:                      (1 byte)
  m_length: 0                               (2 bytes, unused)
  m_tagged: vm_offset_t                     (4 bytes, offset to pair)

VM Heap (16 bytes at offset):
  storage[0]:  Object  (tag name -- always Literal Name)
  storage[1]:  Object  (payload -- any type)
```

Total cost: 24 bytes per tagged value (8-byte Object + 16-byte VM pair).
If the payload is an ExtValue type (Long, ULong, Double, Address), an
additional 8-byte ExtValue is allocated on the VM heap for the 64-bit value.

### 10.2 Allocation

Tagged values are allocated via `vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Tagged)`, which bumps the
VM heap pointer by 16 bytes.  The allocation is O(1) with no fragmentation.
The returned offset is stored in the Object's `m_tagged` field.

```cpp
auto *storage = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Tagged);
storage[0] = name_obj;      // tag name
storage[1] = value_obj;     // payload
// make_tagged(trx, name, payload) performs the alloc above + tags it:
*++trx->m_op_ptr = Object::make_tagged(trx, name_obj, value_obj);
```

### 10.3 Cloning and ExtValue Handling

When an operator extracts a payload from a tagged value, it calls
`make_clone()` on the stored Object.  For scalar types (Integer, Real,
Boolean, etc.), cloning is a simple copy.  For ExtValue types, cloning
allocates a new ExtValue on the VM heap and copies the 64-bit value:

```cpp
auto *pair = trx->offset_to_ptr<Object>(tagged.tagged_offset());
auto value = pair[1].make_clone(trx);   // clones ExtValue if needed
```

This ensures each extracted value has independent ownership of its ExtValue.
The original remains in the pair; the clone is on the operand stack.

### 10.4 Packed Array Encoding

Tagged values share `PackedType::Curry` in packed arrays.  The X-bit
(executable attribute) in the packed header discriminates them:

| Type   | X-bit | Reason                     |
| ------ | ----- | -------------------------- |
| Curry  | 1     | Curry is always Executable |
| Tagged | 0     | Tagged is always Literal   |

On extraction from a packed array, if the packed type is `Curry` and the
X-bit is 0, the Object type is set to `Tagged` instead of `Curry`.  This
means tagged values in packed arrays (procedure bodies, compiled code) are
encoded without consuming a new PackedType slot.

### 10.5 Save/Restore

Tagged values participate in save/restore transactions:

- The 2-Object pair is allocated in the VM heap at the current save level.
- Each Object in the pair has its own save level, set to `m_curr_save_level`
  at creation time.
- If the Objects are modified after a save point (e.g., via `put` on the pair
  through array operations), the modifications are journaled and rolled back
  on `restore`.
- The VM heap pointer is reset on `restore`, discarding any tagged values
  allocated after the save point.

### 10.6 Snap-shot/Thaw

Tagged values are serialized as part of the VM heap during snap-shot.  The
`m_tagged` offset (a vm_offset_t) is position-independent, so tagged values
survive serialization and deserialization without fixup.  The `/ok` and `/err`
names used by `try-result` are `WellKnownName` entries (`WellKnownName::Ok` /
`WellKnownName::Err`), reached via `wellknown_name(...)`; the well-known name
offset table is part of the serialized VM state (`wellknown_offsets_offset` in the
`SnapShotHeader`), so the names remain available after thaw.

### 10.7 Deep Equality

`deep-eq` compares tagged values by recursively comparing both the tag name
and the payload:

```cpp
auto *a_pair = trx->offset_to_ptr<Object>(a.tagged_offset());
auto *b_pair = trx->offset_to_ptr<Object>(b.tagged_offset());
return deep_equal(trx, a_pair[0], b_pair[0], depth + 1)
    && deep_equal(trx, a_pair[1], b_pair[1], depth + 1);
```

Two tagged values are `deep-eq` if and only if their tag names are equal
AND their payloads are `deep-eq`.  This comparison is recursive, so nested
tagged values and composite payloads (arrays, dicts) are compared
structurally.

`eq` compares by identity (same VM offset).  Two tagged values created
separately are never `eq`, even if structurally identical.

### 10.8 PrintFmt

Three format modes:

| Format        | Output         | Example                                     |
| ------------- | -------------- | ------------------------------------------- |
| `s` (default) | `tagged`       | `42 /ok tag =` prints `tagged`              |
| `O` (object)  | `/tag payload` | `({0:O})` renders `42 /ok tag` as `/ok 42i` |
| `#s` (alt)    | `TAGGED`       | `({0:#s})` renders it as `TAGGED`           |

---

## 11. Comparison with Other Languages

### 11.1 Feature Matrix

| Feature | Trix Tagged | Rust enum | Haskell ADT | TypeScript union | Python dataclass |
| --- | --- | --- | --- | --- | --- |
| Variant identification | Name (interned string) | Discriminant (integer) | Constructor | Type guard | Class name |
| Payload type | Any single value | Per-variant struct | Per-constructor types | Per-variant interface | Per-class fields |
| Multi-field payload | Dict or Array | Native struct fields | Native constructor args | Interface fields | Instance attributes |
| Pattern dispatch | `tag-match` (dict) | `match` (exhaustive) | Pattern matching | `switch`/type narrowing | `match` (3.10+) |
| Exhaustiveness | Runtime (`undefined`) | Compile-time | Compile-time | Compile-time (w/ `never`) | Runtime |
| Default/wildcard | `/default` key | `_` pattern | `_` pattern | `default` | `case _` |
| Functional update | `tag-update` (fmap) | Manual | `fmap` (Functor) | Manual | Manual |
| Monadic bind | `tag-bind` | `and_then` | `>>=` | Manual | Manual |
| Unwrap with default | `tag-value-or` | `unwrap_or` | `fromMaybe` | `??` (nullish) | Manual |
| Exception bridge | `try-result` | `catch_unwind` | `try` (IO) | `try/catch` | `try/except` |
| Memory cost | 24 bytes | Enum size | Boxed (heap) | Object + vtable | Object + dict |
| Recursive types | Via nesting | Via `Box` | Native | Via interfaces | Via class refs |

### 11.2 Rust Comparison

Rust's `enum` is the closest analog.  Key differences:

**Trix advantages:**
- No `Box` needed for recursive types (VM heap handles allocation).
- `tag-match` with `/default` enables open unions; Rust `match` requires
  exhaustiveness (which is usually desirable but prevents extensibility).
- `try-result` bridges exceptions to Result in a single operator; Rust
  requires `catch_unwind` (which catches panics, not errors).

**Rust advantages:**
- Compile-time exhaustiveness checking catches missing cases before runtime.
- Multi-field variants are native (`enum Foo { Bar { x: i32, y: f64 } }`);
  Trix requires a Dict payload.
- Pattern destructuring (`let Foo::Bar { x, y } = val`); Trix requires
  explicit `tag-value` + dict access.
- Zero-cost abstractions: no runtime dispatch overhead.

### 11.3 Haskell Comparison

Haskell's algebraic data types are the theoretical gold standard.

**Trix advantages:**
- Mutable payloads via ReadWrite containers (Haskell values are immutable).
- `save`/`restore` provides transactional rollback for tagged value state.
- No monad transformer stacks needed for error handling (`try-result` +
  `tag-bind` is flat).

**Haskell advantages:**
- Type inference eliminates manual tagging.
- Pattern matching is deeply integrated into the language.
- Exhaustiveness is checked by the compiler.
- Type classes (Functor, Monad, etc.) provide generic programming over ADTs.

---

## 12. Design Decisions

### 12.1 Why a Native Type?

Tagged values could be implemented as 2-element arrays with conventions.
A native type provides:

- **Type safety.** `verify_operands(VerifyTagged)` rejects non-tagged values
  at the operator level.  Array-based ADTs would accept any array.
- **Efficient dispatch.** `tag-match` accesses the tag directly from the
  2-Object pair.  Array-based dispatch would require element extraction +
  dict lookup.
- **Packed encoding.** Sharing `PackedType::Curry` with X-bit discrimination
  means no new packed slot is consumed.  Array-based ADTs would require
  encoding as full arrays.
- **Clear semantics.** `is-tagged` returns `true` only for tagged values.
  `type` returns `/tagged-type`.  The type system distinguishes tagged values
  from arrays, curries, and other composite types.

### 12.2 Why Name Tags (Not Strings or Integers)?

Names are immutable, interned strings with cached hash values:

- **O(1) comparison** after interning (offset equality, not string comparison).
- **Immutable** -- tag names cannot be modified after creation.
- **Hashable** -- enables efficient `tag-match` dispatch via dict lookup.
- **Human-readable** -- `/ok`, `/err`, `/some`, `/none` are self-documenting.
- **Consistent** -- the same tag name always resolves to the same Name object.

Integer discriminants (as in Rust) would be faster for switch dispatch but
would sacrifice readability and require a mapping table.  Strings would work
but lack interning and hash caching.

### 12.3 Why Always Literal?

Tagged values must remain Literal (never Executable) because:

- **Packed encoding.** The X-bit in `PackedType::Curry` discriminates Curry
  (X=1, Executable) from Tagged (X=0, Literal).  If Tagged could be
  Executable, this discrimination would break.
- **Semantic clarity.** Tagged values are data containers, not procedures.
  Executing a tagged value would be meaningless.
- **Safety.** Preventing `make-executable` on tagged values ensures they
  always behave as data on the exec stack.

### 12.4 Why No Compile-Time Exhaustiveness?

Trix is dynamically typed.  Adding compile-time exhaustiveness checking would
require:

- A type declaration syntax for defining closed unions.
- A static analysis pass before execution.
- Changes to the scanner and interpreter to support type annotations.

This would fundamentally alter Trix's character as a lightweight, dynamically-
typed scripting language.  The runtime approach (raise `undefined` on
unhandled cases) is consistent with the language's design philosophy: errors
are detected and reported, not prevented by a type system.

### 12.5 Why tag-bind Instead of a Trix Library Function?

`tag-bind` could be implemented in Trix:

```
/tag-bind-trix {
    % tagged name proc -- tagged
    /proc exch def
    /name exch def
    dup name tag?
    { tag-value proc exec }
    { }
    if-else
} def
```

The native implementation is preferred because:

- **Performance.** `tag-bind` is the inner operator in railway chains.  Each
  Trix-level implementation would require 7 operator dispatches per call.
  The C++ implementation is a single function call.
- **Stack efficiency.** The native version does not create temporary
  dicts for local bindings or push/pop additional stack frames.
- **Error propagation.** The native version uses `execute_value()` to
  dispatch the proc, which integrates directly with the interpreter's
  exec stack.  The Trix version would use `exec`, which has different
  error propagation semantics in some edge cases.

### 12.6 Why try-result Rolls Back the Operand Stack

When a procedure fails inside `try-result`, the operand stack may contain
partial results from the failed computation.  `try-result` records the stack
depth before execution and rolls back to that depth on error, freeing any
ExtValues that were allocated.

This is essential for railway-oriented programming: the error path must
leave the stack in a known state.  Without rollback, each `tag-bind` in a
chain would need to handle arbitrary stack pollution from failed steps.

