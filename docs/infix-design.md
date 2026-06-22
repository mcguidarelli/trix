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

# Design: Infix Math Expression Syntax

## Overview

`$( expr )` and `$[ expr ]` provide infix math expression syntax in the Trix
scanner. The scanner parses conventional infix expressions with operator
precedence and emits equivalent postfix tokens. Zero runtime cost -- purely a
scanner-level desugar.

`$( )` preserves strict type semantics (no implicit coercion).
`$[ ]` auto-promotes operands before each binary operator.

## Motivation

Postfix notation is natural for data pipelines but painful for math:

```trix
% Postfix: hard to read
a x dup mul mul b x mul add c add

% Infix: immediately clear
$( a * x * x + b * x + c )
```

The `$( )` form targets the primary readability complaint (math expressions)
without changing Trix's concatenative nature for everything else.

See also `.name` field-access sugar (Section 8b of `scanner-syntax.md`),
a sibling scanner-level rewrite that reuses the same per-stream token
buffer (`m_infix_offset`) to emit multi-token sequences.

## Syntax

### Delimiters

- `$(` opens strict infix mode; `)` closes it.
- `$[` opens auto-promote infix mode; `]` closes it.
- `$` is currently a regular character (RG) in the scanner -- not a delimiter.
  The scanner recognizes `$(` and `$[` as special two-character sequences.
  `[`/`]` are unambiguous as infix delimiters because they are not infix
  operators; the `$` peek-dispatches before `[` could be read as an
  array-literal opener.

### Two Forms

**Strict: `$( expr )`**
Type semantics are unchanged. `$( 3 + 4.0 )` emits `3 4.0 add` which is a
type-check error (integer + real). Users must promote beforehand or use
same-type operands.

**Auto-promote: `$[ expr ]`**
Emits `promote` before each binary operator. `$[ 3 + 4.0 ]` emits
`3 4.0 promote add` which succeeds (both promoted to real, result is real).
The `promote` is a no-op when operand types already match.

## Precedence Table

The Pratt parser uses integer precedence levels; higher values bind tighter.
Binary operators occupy levels 4-13; `( )` grouping, unary operators, and
`**` sit above the binary table (see notes below).

| Prec | Operators            | Associativity | Trix emission               |
| ---- | -------------------- | ------------- | --------------------------- |
| 13   | `**`                 | right         | `pow`                       |
| 12   | `*`, `/`, `%`        | left          | `mul`, `div`, `mod`         |
| 11   | `+`, `-`             | left          | `add`, `sub`                |
| 10   | `<<`, `>>`           | left          | `shift-left`, `shift-right` |
| 9    | `<`, `<=`, `>`, `>=` | left          | `lt`, `le`, `gt`, `ge`      |
| 8    | `==`, `!=`           | left          | `eq`, `ne`                  |
| 7    | `&`, `&&`            | left          | `and`                       |
| 6    | `^`                  | left          | `xor`                       |
| 5    | `\|`, `\|\|`         | left          | `or`                        |
| 4    | `? :`                | right         | `rot`, `select` (ternary)   |

`( )` grouping and unary `!`/`~`/`+` are handled structurally by
`infix_primary` before the precedence-climbing loop, so they bind tightest.
Unary `-` is the exception: it binds **looser** than `**` (it recurses into
the precedence loop at min-prec 13), so `-x ** y` parses as `-(x ** y)` --
the Python/MATLAB convention (e.g. `-2 ** 2 == -4`).  `**` is listed at
13 because it participates in the same precedence loop as the binary
operators but with right-associativity and tighter binding than `*`/`/`/`%`.

### Notes

- `!` and `~` are synonyms (both emit `not`). Use whichever reads better:
  `!` for boolean intent, `~` for bitwise intent.
- `and`, `or`, `not`, `xor` are already polymorphic in Trix: boolean logic
  on booleans, bitwise operations on unsigned integers. The infix operators
  inherit this behavior automatically.
- No short-circuit `&&`/`||` -- not needed since `&`/`|` already do the right
  thing based on operand type.
- `**` is right-associative: `$( 2.0r ** 3.0r ** 2.0r )` = `$( 2.0r ** (3.0r ** 2.0r) )`
  = 512.0.  `**` maps to `pow`, which is float-only -- an integer `$( 2 ** 3 )`
  raises `/type-check` (use Real or Double literals).

## Function Call Syntax

Any Trix operator can be called as a function inside `$( )`:

```trix
$( sqrt(x) + 1 )           % emits: x sqrt 1 add
$( abs(a - b) )            % emits: a b sub abs
$( pow(base, exp) )        % emits: base exp pow
$( max(a, b) + min(c, d) ) % emits: a b max c d min add
$( kahan-sum(data) )       % emits: data kahan-sum
```

Arguments are pushed left-to-right, then the operator name is emitted.
Multi-argument functions use comma-separated argument lists. Operator names
follow normal Trix naming (hyphens allowed).

Nullary functions (no arguments) use empty parens:

```trix
$( clock() + offset )          % emits: clock offset add
```

## Name Resolution

Inside `$( )`, bare identifiers are emitted as executable names (looked up at
runtime, same as normal Trix name resolution):

```trix
$( x + 1 )          % emits: x 1 add
$( a * b )          % emits: a b mul
$( pi * r * r )     % emits: pi r r mul mul
```

Number literals retain their type suffixes:

```trix
$( x + 1l )         % emits: x 1l add
$( n * 2u )         % emits: n 2u mul
$( t * 0.001d )     % emits: t 0.001d mul
```

Radix prefixes work.  Bitwise `&` requires unsigned operands, so use an
unsigned radix literal (`16#FF#u`); the C-style `0xFF` is an Integer and would
make `&` raise `/type-check`:

```trix
$( flags & 16#FF#u )   % emits: flags 16#FF#u and   (255u; flags must be unsigned)
```

## Interaction with Local Variables

Infix expressions compose naturally with the `{ |vars| body }` local
variable syntax:

```trix
/quadratic { |a b c x|
    $( a * x * x + b * x + c )
} def

/distance { |x1 y1 x2 y2|
    $[ sqrt((x2 - x1) ** 2 + (y2 - y1) ** 2) ]
} def

/lerp { |a b t|
    $[ a + t * (b - a) ]
} def
```

## Nesting

`$( )` can appear anywhere a normal token can: inside `{ }` procs, at top
level, after other tokens. Multiple `$( )` expressions can appear in
sequence:

```trix
$( a + b ) $( c * d ) add     % legal: two infix exprs, then postfix add
```

`$( )` cannot nest inside itself -- parentheses inside `$( )` are grouping
only. To embed a Trix expression that returns a value, use function call
syntax or break into multiple `$( )` blocks.

## Parentheses

Inside `$( )`, parentheses `( )` are **grouping**, not string literals.
String literals cannot appear inside `$( )`. This is not a problem -- infix
mode is for math expressions only. Use normal postfix for string operations.

## What This Does NOT Change

- No new types, no new runtime operators (except `promote` emissions in `$[ ]`)
- No semantic changes to existing operators
- Postfix remains the native form -- `$( )` is sugar
- All existing code works unchanged
- Strict type checking preserved in `$( )`
- Packed arrays, binary tokens, snap-shot/thaw, save/restore all unaffected
- Binary token encoding does not need changes (infix is resolved at scan time)

## Desugaring Examples

### Quadratic formula (strict)

```trix
% Source
$( (-b + sqrt(b ** 2 - 4 * a * c)) / (2 * a) )

% Emitted postfix
b neg b 2 pow 4 a mul c mul sub sqrt add 2 a mul div
```

### Bit manipulation

```trix
% Source
$( (flags >> 4) & 16#0F#u )

% Emitted postfix
flags 4 shift-right 16#0F#u and
```

Note: the shift *count* must be an Integer (`4`, not `4u`) -- `shift-right` is
`uint int -- uint`, so an unsigned count raises `/type-check`.  The masked
value, by contrast, must be unsigned: `0x0F` produces an Integer in Trix, so
use `16#0F#u` (15u) for an explicit unsigned value with bitwise `&`/`and`.

### Mixed infix and postfix

```trix
% Pipeline with infix condition
[1 2 3 4 5] { |x| $( x * x ) } map { |x| $( x > 10 ) } filter

% Emitted postfix
[1 2 3 4 5] { |x| x x mul } map { |x| x 10 gt } filter
```

### Auto-promote

```trix
% Source (mixed integer and real)
$[ 3 + 4.0 ]

% Emitted postfix
3 4.0 promote add

% Result: 7.0 (both promoted to real)
```

## Scanner Implementation

### Approach

Pratt parser (precedence climbing) implemented as private methods on
Stream in `src/scanner_infix.inl`, alongside the existing scanner. When the
scanner encounters `$` followed by `(` (or `$` followed by `[`):

1. Consume the delimiter (`$(` or `$[`)
2. Record whether auto-promote mode is active (true for `$[`, false for `$(`)
3. Call `scan_infix_expression(close_ch)` -- Pratt parser using precedence
   climbing; `close_ch` is `)` for `$(` and `]` for `$[`.
4. On the matching close, return to normal postfix scanning.

The parser emits `(Lexeme, Object)` pairs into the normal scanner output --
the same token stream the rest of the interpreter consumes. No new Lexeme
types needed.

All infix parsing code lives in `src/scanner_infix.inl`; the per-stream state
it uses (`m_infix_offset` and the packed-token cursor) lives in `src/stream.inl`,
alongside the existing `scan_procedure()`, `scan_array()`, and `scan_dict()`
scanner methods.

### Token recognition

`$` is currently character attribute RG (regular). The scanner's main loop
handles RG characters by accumulating them into a token. To recognize `$(`:

- When the scanner sees `$`, peek at the next character.
- If `(`, enter infix mode (strict).
- If `$` followed by `[`, enter infix mode (auto-promote).
- Otherwise, treat `$` as a regular character (backward compatible).

### Expression parser

Standard Pratt parser:

- `scan_infix_expression(auto_promote, close_ch)` -- entry point
- `infix_expr(..., min_prec, ...)` -- precedence-climbing loop (binary operators)
- `infix_primary(...)` -- literals, names, unary operators, grouping `( )`
- `infix_name_or_call(...)` -- bare name or `name(args...)` function call

Each binary operator emission is:
- Strict mode: emit just the operator name
- Auto-promote mode: emit `promote` then the operator name

### Error handling

Scanner errors inside `$( )` use the existing `trx->error()` mechanism:
- Unexpected character: `"infix: unexpected '{:c}' in expression"`
- Missing close delimiter: `"infix: missing closing '{:c}'"`
- Empty expression: `"infix: empty expression"`
- Malformed token: `"infix: expected number"`, `"infix: invalid number '{}'"`

## Estimated Cost

- Scanner changes: ~200-250 lines (Pratt parser + delimiter recognition)
- No changes to interpreter, dispatch, types, or any operator
- 5 new member variables for infix state (3 in Stream: m_infix_offset, m_infix_packed_size, m_infix_packed_read; 2 in Trix: m_infix_scratch_objs, m_infix_scratch_count)
- Test suite: ~100-150 assertions (precedence, associativity, function calls,
  auto-promote, error paths, interaction with locals and procs)

## Decisions Made

| Question | Decision | Rationale |
| --- | --- | --- |
| Type promotion | Separate forms: `$()` strict, `$[]` auto-promote | Preserves strict-by-default philosophy; convenience available when wanted |
| Strings inside `$()` | Not supported (parens = grouping) | Infix is for math only |
| Bitwise operators | `&`/`\|`/`^`/`~` map to `and`/`or`/`xor`/`not` | Already polymorphic (boolean on bool, bitwise on unsigned int) |
| `!` and `~` | Both supported as synonyms for `not` | Developer picks whichever reads better for context |
| Assignment | No | Pure expressions only; assignment is a statement |
| Ternary `?:` | Yes, non-short-circuit | Emits `rot select`; both branches evaluated; right-associative |
| Comma operator | No | Comma only for function argument lists |
| `&&`/`\|\|` | Synonyms for `&`/`\|` | Same precedence and emission; familiarity for C/Java developers |
