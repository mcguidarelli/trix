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

# Trix Scanner Syntax Reference

This document is a complete reference for the Trix lexical syntax — every token form
the scanner recognizes, with exact grammar rules, suffix sequences, escape codes, and
binary encoding tables.

---

## Table of Contents

1. [Source Encoding and Character Classes](#1-source-encoding-and-character-classes)
2. [Whitespace and Comments](#2-whitespace-and-comments)
3. [Numeric Literals](#3-numeric-literals)
4. [Name Tokens](#4-name-tokens)
5. [String Literals](#5-string-literals)
6. [Array Literals](#6-array-literals)
7. [Dict Literals](#7-dict-literals)
8. [Procedure Literals](#8-procedure-literals)
8a. [Infix Expressions](#8a-infix-expressions)
9. [Mark Tokens](#9-mark-tokens)
10. [Binary Tokens (0x80–0xFF)](#10-binary-tokens-0x800xff)
11. [Packed Array Encoding](#11-packed-array-encoding)
12. [Lexeme Summary](#12-lexeme-summary)
13. [Error Conditions](#13-error-conditions)
14. [Formal Grammar](#14-formal-grammar)

---

## 0. Cheat Sheet — All Prefixes and Suffixes

A condensed overview of every sigil the scanner recognizes.  Each
detailed section below covers exact rules.

**Invariant:** `$` always means "global VM"; `$$` always means
"force-local VM"; `#` always introduces a literal-shape suffix.  The
character carries the same semantic in prefix and suffix position.

### Prefixes

| Group    | Form(s)               | Meaning                                          |
| -------- | --------------------- | ------------------------------------------------ |
| Names    | `/foo` `\foo`         | Literal / Executable Name                        |
| Names    | `//foo` `\\foo`       | Immediate-lookup (scan-time resolve)             |
| Names    | `$/foo` `$\foo`       | Name interned in **global VM**                   |
| Names    | `$$/foo` `$$\foo`     | Name **force-interned in local VM**              |
| Sugar    | `.field`              | Field access — desugars to `/field get` (chains) |
| Blocks   | `${ body }`           | Global-alloc scope (scan + run)                  |
| Blocks   | `$${ body }`          | Force-local scope (inverse of `${...}`)          |
| Infix    | `$[ expr ]`           | Promoted (auto-widening) infix                   |
| Infix    | `$( expr )`           | Non-promoting infix (strict type)                |
| Strings  | `(text)`              | Escaped string literal                           |
| Strings  | `<(raw)>`             | Raw string (no escapes, balanced parens)         |
| Strings  | `<48 65>`             | Hex string                                       |
| Numbers  | `0x` `0b` `0o` `R#…#` | Base-prefixed integer (R = 2..36)                |
| Marks    | `[` `<<` `{{` `{`     | Array / Dict / Set / Proc opener                 |
| Comments | `%` `%{`              | Line / Block comment (nestable)                  |

### Suffixes — first slot is mutex: one of `=` / `$` / `$$`

| Host | Grammar | Notes |
| --- | --- | --- |
| Numbers | `i u l ul a r d b q uq` | Integer / UInteger / Long / ULong / Address / Real / Double / Byte / Int128 / UInt128 |
| String | `#[b]` \| `#a[r\|w]` \| `#[=\|$\|$$][l\|x][r\|w]` | `#b` = single Byte; `#a` = byte array; `#l`/`#x` = literal / executable attribute |
| Array | `#[=\|$\|$$][r\|w]` | eq / force-global / force-local + read access |
| Dict | `#[=\|$\|$$][r\|w]` | Same shape as array |
| Set | `#[=\|$\|$$][r\|w]` | Same shape as array |
| Proc | `#[=\|$\|$$][a\|p][e\|l][r\|w]` | `#a`/`#p` = Array / Packed form; `#e`/`#l` = early / late binding |
| Locals | `\|names\|#N` or `\|names\|#+N` | Absolute / relative frame-dict capacity |

### First-slot semantics (`=` / `$` / `$$`)

| Suffix | Storage class | Notes |
| --- | --- | --- |
| *(none)* | Default | Follows enclosing `m_curr_alloc_global` |
| `#=` | eq-temp | Reuses pre-allocated `=array` / `=dict` / `=set` / `=proc` / `=string` scratch |
| `#$` | Force global VM | Survives `save`/`restore`; reclaimed by mark-sweep GC |
| `#$$` | Force local VM | Overrides enclosing `${...}` or `set-global true` |

Any pairwise combination of `=` / `$` / `$$` raises `/syntax-error`.
`#$$$` (three `$` after `#`) also raises. A bare `$$$` prefix is not
special — it parses as an ordinary name.

---

## 1. Source Encoding and Character Classes

Trix source files are single-byte (Latin-1 compatible). Every byte 0x00–0xFF is classified
by a bitmask attribute (`chattr_t`):

| Attribute | Mask   | Characters                                        |
| --------- | ------ | ------------------------------------------------- |
| `WS`      | 0x0001 | Whitespace: HT LF VT FF CR SP                     |
| `CT`      | 0x0002 | Comment terminator: LF FF CR                      |
| `DL`      | 0x0004 | Delimiter: `% ( ) / \ . < > [ ] { }`              |
| `RG`      | 0x0008 | Regular: printable 0x21–0x7E except DL            |
| `LC`      | 0x0010 | Lowercase letter: `a`–`z`                         |
| `UC`      | 0x0020 | Uppercase letter: `A`–`Z`                         |
| `DD`      | 0x0040 | Decimal digit: `0`–`9`                            |
| `OD`      | 0x0080 | Octal digit: `0`–`7`                              |
| `HD`      | 0x0100 | Hex digit: `0`–`9` `A`–`F` `a`–`f`                |
| `IN`      | 0x0200 | Integer start: `+` `-` `0`–`9`                    |
| `FE`      | 0x0400 | Float frac/exp: `.` `E` `e`                       |
| `IS`      | 0x0800 | Integral suffix: `A B I L Q U a b i l q u`        |
| `FS`      | 0x1000 | Float suffix: `D E R d e r`                       |
| `SE`      | 0x2000 | Syntax-error: NUL–US (except HT/LF/VT/FF/CR), DEL |
| `US`      | 0x4000 | Underscore: `_`                                   |
| `SS`      | 0x8000 | String-stop: `(` `)` `\` CR (fast-path exit)      |

**Regular characters** form the body of names and numbers. A **delimiter** always ends a
token and begins the next one. Bytes with `SE` set are invalid and produce a SyntaxError.
Bytes 0x80–0xFF are the binary-token range (Section 10); they carry no attribute bit
(`chattr` is `0x0000`) and are recognized by value (`ch >= 0x80`), not by a flag test.

A character is a **terminator** (valid after any suffix) if it is not a regular character
(i.e., whitespace, a delimiter, a syntax-error byte, a binary-token byte, or EOF).

---

## 2. Whitespace and Comments

**Whitespace:** HT (0x09), LF (0x0A), VT (0x0B), FF (0x0C), CR (0x0D), SP (0x20). Any
run of whitespace between tokens is silently consumed. CRLF is treated as a single line
ending.

**Line comments:** Start with `%` and extend to the next comment-terminator (LF, FF, or CR).
The terminator is NOT consumed; it is recycled as whitespace for the outer loop.

```
% this entire line is a comment
42 % inline comment after a token
```

**Block comments:** `%{` opens a nestable block comment; `%}` closes it. Block comments
may span multiple lines, nest to arbitrary depth, and appear anywhere whitespace is valid.
The scanner tracks a depth counter: each `%{` increments it, each `%}` decrements it.
The comment ends when depth returns to zero. Line endings (LF, CR, CRLF) inside a block
comment are counted for line-number tracking. An unterminated block comment (EOF before
depth reaches zero) raises `SyntaxError`.

A `%}` at top level (not inside a block comment) is harmless — the `%` starts a line
comment and the `}` is consumed as part of it.

```
%{ This is a block comment %}
%{ Multi-line
   block comment %}
1 %{ inline %} 2 add    % yields 3
%{ outer %{ nested %} outer %} 42
```

**Note:** `%{` and `%}` inside strings are literal characters, not comment delimiters.

**Shebang:** If the first two bytes of a file stream are `#!` (at byte offset 0, with no
leading whitespace), the entire line is consumed as a comment. This enables Unix-style
self-executing scripts:

<!-- doctest: skip (shebang only applies at byte 0 of a file stream, not stdin) -->
```
#!/usr/bin/env trix
42 =    % prints 42
```

Strict POSIX: `#!` must be at byte offset 0. A file starting with whitespace before `#!`
does NOT get shebang treatment — the `#!` parses as a name token and raises `Undefined`.
String streams (e.g. the `token` operator) are not affected by shebang processing.

### 2.1 Lexical suffixes are contiguous

Comments are transparent **between tokens**.  They are NOT transparent between a token
and a `#`-suffix or letter-suffix that lexically extends it.  A suffix must follow its
host with no intervening whitespace or comment; otherwise the suffix detaches into a
separate token.

| Host | Contiguous → suffix attaches | Separated → two tokens |
| --- | --- | --- |
| Numeric | `42.0d` (double) | `42.0 d` or `42.0 % c\nd` → real `42.0`, name `d` |
| Radix | `16#FF#` | `16 #FF#` → integer `16`, name `#FF#` |
| String | `(hello)#r` (ReadOnly) | `(hello) #r` → string, then name `#r` |
| Array | `[1 2 3]#=` | `[1 2 3] #=` → array, then name `#=` |
| Dict | `<< /k v >>#r` | `<< /k v >> #r` → dict, then name `#r` |
| Set | `{{ 1 2 3 }}#r` | `{{ 1 2 3 }} #r` → set, then name `#r` |
| Procedure | `{ body }#p` | `{ body } #p` → proc, then name `#p` |
| Locals preamble | `\|a b\|#8` or `\|a b\|#+6` | `\| a b \| #8` → locals default-capacity; the spaced `#8` becomes a body token |

The rule matches the numeric-literal convention (`42.0d` is one token, `42.0 d` is two) and
applies uniformly to every composite form.  When you want a suffix to take effect,
keep it flush with the closing delimiter.

---

## 3. Numeric Literals

### 3.0 Underscore separators

All numeric literal forms (integer, radix, floating-point) accept `_` as a visual
separator between digits. The underscore is stripped before parsing and does not affect
the value.

```
1_000_000       → Integer 1000000
16#DEAD_BEEF#ul → ULong 3735928559
1_000.123_456d  → Double 1000.123456
2#1010_0011#    → Integer 163
```

**Placement rules:** `_` is allowed only between two decimal digits (`0`–`9`). Within the
radix-digits section of a `base#digits#` form, `_` is also allowed between hex-digit
characters (`0`–`9`, `A`–`F`, `a`–`f`). All other placements cause the token to be
treated as a name (not a number, and not an error):

- Leading (`_100`), trailing (`100_`), or doubled (`1__0`) → name
- Adjacent to sign (`+_1`), decimal point (`1_.0`, `1._0`), exponent (`1_e5`, `1e_5`),
  radix `#` (`16_#FF#`, `16#_FF#`), or suffix (`100_u`, `1.0_d`) → name

### 3.1 Integer tokens

```
[+|-] digits [suffix]
```

The number is parsed as an unsigned accumulation, then the sign is applied. A token is
tried as a number first; if it fails (e.g. leading letter after sign) it falls through to
name parsing.

**Suffix** (case-insensitive, at most one):

| Suffix          | Type     | Bit width | Range                        |
| --------------- | -------- | --------- | ---------------------------- |
| `b`             | Byte     | 8         | 0–255 (unsigned)             |
| *(none)* or `i` | Integer  | 32        | −2 147 483 648–2 147 483 647 |
| `u`             | UInteger | 32        | 0–4 294 967 295              |
| `l`             | Long     | 64        | −2⁶³–2⁶³−1                   |
| `ul`            | ULong    | 64        | 0–2⁶⁴−1                      |
| `a`             | Address  | ptr-sized | raw memory pointer           |
| `q`             | Int128   | 128       | −2¹²⁷–2¹²⁷−1                 |
| `uq`            | UInt128  | 128       | 0–2¹²⁸−1                     |

Rules:
- Overflow is detected per type (carry bits checked at 8, 32, 64 bits). Overflow raises
  `NumericalOverflow`.
- Negative values (`-` prefix) are incompatible with `u`, `ul`, and `a` — such tokens
  are parsed as executable names instead, not numbers.
- `b` requires the value to be in 0–255; values outside this range raise `NumericalOverflow`.

Examples:
```
42          → Integer 42
-7          → Integer -7
255b        → Byte 255
100u        → UInteger 100
-5l         → Long -5
99ul        → ULong 99
0x1000#a    → Address 0x1000   (# separator required before suffix in C-prefix form)
```

### 3.2 Radix integers

```
base # digits [# [suffix]]
```

- `base`: decimal integer 2–36. A base outside this range is not numeric — the
  token is parsed as a name (executing it raises `Undefined`), not `SyntaxError`.
- `digits`: characters in the specified base (`0`–`9`, `A`–`Z`/`a`–`z`). Case-insensitive.
- The final `#` is optional; it is only required to attach a type suffix. Without a
  suffix the literal is terminated by the next whitespace, delimiter, or EOF, and
  defaults to Integer (`16#FF` == `16#FF#` == 255).
- Any integral suffix is valid after the second `#`.

Examples:
```
16#FF#          → Integer 255
16#FF#u         → UInteger 255
2#1010#         → Integer 10
8#755#          → Integer 493
36#Z#           → Integer 35
16#FFFFFFFF#u   → UInteger 4294967295
16#DEADBEEF#l   → Long 3735928559
```

### 3.3 Floating-point tokens

```
[+|-] digits [. [digits]] [Ee [+|-] digits] [suffix]
[+|-] . digits           [Ee [+|-] digits] [suffix]
```

A decimal point or exponent makes a token floating-point (without a suffix, Real is
assumed).

**Suffix** (case-insensitive):

| Suffix          | Type   | Width         | Notes                                                        |
| --------------- | ------ | ------------- | ------------------------------------------------------------ |
| *(none)* or `r` | Real   | 32-bit float  | overflow raises `NumericalOverflow`                          |
| `d`             | Double | 64-bit double | larger range (~1.8e308); overflow raises `NumericalOverflow` |

For floating-point, the radix form does not apply.  Use C99 hex float
notation instead: `0x1.0p10` (see Section 3.6).

### 3.4 Special floating-point keywords

Six names provide IEEE-754 special values via `systemdict` (the scanner does not
recognize them as numeric literals):

| Keyword | Type   | Value     |
| ------- | ------ | --------- |
| `inf`   | Real   | +Infinity |
| `nan`   | Real   | quiet NaN |
| `inf#r` | Real   | +Infinity |
| `nan#r` | Real   | quiet NaN |
| `inf#d` | Double | +Infinity |
| `nan#d` | Double | quiet NaN |

These are ordinary names in `systemdict` and can be shadowed by user dicts (they are
not scanner keywords). `inf#r`/`nan#r` are explicit-Real-suffix aliases of the bare
`inf`/`nan`. Negative infinity is produced with `inf neg` or `inf#d neg`.

Arithmetic involving `inf`/`nan` values raises `NumericalInf` or `NumericalNaN` errors
(strict mode, by design). Use `inf?`, `nan?`, `finite?` for classification.

Examples:
```
1.5         → Real 1.5
-3.14       → Real -3.14
1E6         → Real 1000000.0
.5          → Real 0.5
-1.         → Real -1.0
1.0e-5      → Real 0.00001
3.14159d    → Double 3.14159
1.0e308d    → Double 1e308       (D is the Double suffix, not an exponent marker)
inf         → Real +∞
nan         → Real quiet NaN
inf#d        → Double +∞
nan#d        → Double quiet NaN
```

### 3.5 C-prefix integers

C-style prefix notation for hex, octal, and binary integers:

```
0xFF        → Integer 255          (hexadecimal)
0o77        → Integer 63           (octal)
0b1010      → Integer 10           (binary)
0X1A        → Integer 26           (uppercase prefix accepted)
```

The `#` separator is **required** before type suffixes to avoid ambiguity
with hex digits: `0xFFb` parses as hex `FFB` (= 4091), not as byte 255.
Use `0xFF#b` for byte 255.

```
0xFF#b      → Byte 255
0xFF#l      → Long 255
0xFF#u      → UInteger 255
0x1000#a    → Address 0x1000
```

Underscore separators are supported: `0xFF_FF`, `0b1010_0101`.

### 3.6 Hex float literals (C99)

C99-style hexadecimal floating-point with mandatory binary exponent:

```
0x1.0p0     → Real 1.0
0x1.0p8     → Real 256.0
0x1.Fp4     → Real 31.0
0xA.BCp0    → Real 10.734375
0x1.0p-1    → Real 0.5
0xFFp0      → Real 255.0          (no fractional part)
0x0.8p0     → Real 0.5            (fractional only)
```

Default type is Real.  Use `#d` suffix for Double, `#r` for explicit Real:

```
0x1.921FB54442D18p1#d  → Double π (full precision)
0x1.0p0#r              → Real 1.0 (explicit)
```

The `p`/`P` exponent is mandatory — without it, `0xFF` stays an integer.
Case-insensitive: `0X`, `0x`, `P`, `p` all accepted.

---

## 4. Name Tokens

Names consist of **regular characters** only (no whitespace, no delimiters). Maximum
length is **127 characters**; longer tokens raise `LimitCheck`.

### 4.1 Literal names: `/`

```
/name       → literal Name object
$/name      → literal Name object, interned in global VM
$$/name     → literal Name object, force-interned in local VM
            (overrides an enclosing ${...} / set-global true)
```

A bare `/` (slash followed immediately by a delimiter or whitespace) produces the literal
name `/`.  The `$/` prefix routes the Name's interning to the global VM region so that
the Name survives `save` / `restore`.  The `$$/` prefix is the symmetric inverse: it
forces the Name to intern locally even when an enclosing `${...}` block or
`set-global true` directive had flipped the scanner-time allocation flag to global.
See § 12a for the broader Global VM directive family.

### 4.2 Executable names: `\`

```
\name       → executable Name object
$\name      → executable Name object, interned in global VM
$$\name     → executable Name object, force-interned in local VM
```

A bare `\` produces the executable name `\`.  As with `$/` / `$$/`, the `$\` prefix
interns the Name in the global VM region; `$$\` is the force-local inverse.

### 4.3 Immediate lookup: `//` and `\\`

```
//name      → look up name in dictionary stack; push result as Literal
\\name      → look up name in dictionary stack; push result preserving its execute bit
```

- `//name` always pushes a `LiteralValue` (like `load`).
- `\\name` pushes `LiteralValue` if the found object is literal or ignores the execute
  attribute; otherwise pushes `ExecutableValue`.
- If the name is not found, raises `Undefined` error immediately at scan time.

**Inside a `{ }` proc body, `//name` substitutes the resolved VALUE, and that
embedded value follows its own execute attribute when the proc later runs.**
This produces an asymmetry: a proc-bound name (e.g. `//my-proc`, whose value is a
nested proc — and nested procs are data) pushes when the proc runs, so the
factory pattern `/make { //my-proc curry } def` works; but `//some-operator`
embeds an *executable* operator, which **executes** when the proc runs, so
`/make-adder { //add curry } def` underflows instead of currying. To obtain an
operator as data inside a proc, use `/name load` (e.g. `{ /add load curry }`).

### 4.4 Dictionary path form

When the name following `//` or `\\` starts with `:`, it is a **path lookup**:

```
//:rootdict:segment:...:leafname
\\:rootdict:segment:...:leafname
```

- `rootdict` must be one of: `systemdict`, `userdict`, `errordict`, `handlersdict`, `modules`, or any dict entry in systemdict (e.g., `protocoldict`, `pipeline`, `records`).
- Each intermediate `segment` is looked up as a Name in the preceding Dict.
- `leafname` is looked up in the last intermediate Dict found.
- Special root form `:status:key` performs on-demand VM introspection without
  traversing a Dict chain. See the `//:status:` system for available keys.

```
//:systemdict:numbers:real-type:pi     → Real pi
//:userdict:my-constant                → current value of my-constant
//:errordict:error-data                → last error data (set by throw-with)
//:status:vm-used                      → current VM bytes used
```

**Important:** Path lookups via `//` are *immediate* — they resolve at scan time.
Inside `{ }` procedure bodies, `//` captures the value when the procedure is parsed,
not when it is executed. Use executable names or operators (e.g., `last-error-data`)
for values that change at runtime.

#### Runtime and dynamic path lookup

Path names also work with `load` and as executable names at runtime:

```
/:systemdict:numbers:real-type:pi load     → 3.1415927  (runtime lookup)
```

Paths can be constructed dynamically from strings using `to-name`:

```
(:systemdict:numbers:real-type:pi) to-name load    → 3.1415927

% build path at runtime from variable
/key (pi) def
(:systemdict:numbers:real-type:) key concat to-name load   → 3.1415927
```

This works because `load` uses the same path resolution as `//` — any
Name starting with `:` is treated as a path regardless of how it was created.

---

## 5. String Literals

All string forms share the same suffix grammar (Section 5.4). Maximum string length is
`MaxStringLength` (65 535 bytes); exceeding it raises `LimitCheck`.

### 5.1 Text strings: `( ... )`

Delimited by `(` and `)`. Parentheses nest: an unescaped `(` inside a string increments a
nesting counter, and `)` only ends the string when the counter is 0.

**Escape sequences:**

| Escape | Value | Meaning |
| --- | --- | --- |
| `\a` | 0x07 | Bell (alert) |
| `\b` | 0x08 | Backspace |
| `\e` | 0x1B | Escape |
| `\f` | 0x0C | Form Feed |
| `\n` | 0x0A | Line Feed (newline) |
| `\r` | 0x0D | Carriage Return |
| `\t` | 0x09 | Horizontal Tab |
| `\v` | 0x0B | Vertical Tab |
| `\(` | `(` | Literal open-paren |
| `\)` | `)` | Literal close-paren |
| `\\` | `\` | Literal backslash |
| `\^X` | 0x00–0x1F | Control char: X ∈ `@A`–`Z[\]^_`, `^?` → DEL (0x7F) |
| `\0`–`\777` | 0–255 | Octal: 1–3 octal digits, value taken mod 256 (`\400`–`\777` wrap) |
| `\xhh` | 0–255 | Hex: 1–2 hex digits |
| `\uXXXX` | U+0000–U+FFFF | Unicode BMP: exactly 4 hex digits, emits UTF-8 (1–3 bytes) |
| `\UXXXXXXXX` | U+0000–U+10FFFF | Unicode full: exactly 8 hex digits, emits UTF-8 (1–4 bytes) |
| `\{name}` | *(varies)* | Interpolation: scan-time name lookup, value spliced as text |
| `\CR`, `\LF`, `\CRLF` | *(nothing)* | Line continuation — ignored |

**Unescaped line endings** (bare LF or CR inside a string) are normalized to a single LF
(0x0A) in the output.

**Control-character caret notation:** `\^X` where X is in ASCII range `0x3F`–`0x5F`:
- `\^?` → DEL (0x7F, special case for `?` = 0x3F)
- `\^@` → NUL (0x00); `\^A` → SOH (0x01); … `\^Z` → SUB (0x1A); `\^_` → US (0x1F)

**Unicode escape sequences:** `\uXXXX` and `\UXXXXXXXX` encode Unicode codepoints as
UTF-8 byte sequences.  Hex digits are case-insensitive.  Surrogate codepoints
(U+D800–U+DFFF) and values above U+10FFFF are rejected with `SyntaxError`.
The `\u` form covers the Basic Multilingual Plane (U+0000–U+FFFF); the `\U` form
covers the full Unicode range including supplementary planes.  For ASCII codepoints
(U+0000–U+007F), `\u` emits a single byte identical to `\xhh`.  Raw strings
`<(...)>` are unaffected — `\u` and `\U` are literal characters inside raw strings.

```
(\u00E9)         % e-acute: 2 bytes (0xC3, 0xA9)
(\u4E16)         % CJK 'world': 3 bytes (0xE4, 0xB8, 0x96)
(\U0001F600)     % grinning face emoji: 4 bytes (0xF0, 0x9F, 0x98, 0x80)
(caf\u00E9)      % mixed: "cafe" + e-acute = 6 bytes
```

**Interpolation escape sequences:** `\{name}` performs an immediate name lookup at scan
time and splices the value's string representation directly into the string being built.

The name between `{` and `}` is resolved through the dict stack using the same
mechanism as `//name` (immediate lookup).  Dictionary path syntax is supported:
`\{:systemdict:numbers:real-type:pi}` follows the colon-delimited path to resolve
nested dict entries.

Interpolation resolves at **scan time**, not runtime.  The value is baked into the
string when the `(...)` literal is scanned.  This means:

```
/x 42 def
/s (\{x} is the answer) def   % s = "42 is the answer"
/x 99 def
s =                            % prints "42 is the answer" (scan-time value)
```

Inside a procedure, `\{name}` resolves when the procedure is scanned, not when it
is executed:

```
/x 42 def
/f { (\{x}) = } def           % string "42" baked in at scan time
/x 99 def
f                              % prints "42"
```

**All types** can be interpolated.  Values are formatted via `PrintFmt::process_object()`
(same output as the `=` operator).  Simple types produce compact output with no
decoration (no type suffixes, no quoting, no delimiters); compound and tag types
(Array, Dict, Null, Operator) splice as their bare type-name, not a structural dump:

| Type     | Output                   | Example              |
| -------- | ------------------------ | -------------------- |
| Integer  | Decimal digits           | `42`, `-100`         |
| UInteger | Decimal digits           | `42`                 |
| Long     | Decimal digits           | `9999999999`         |
| ULong    | Decimal digits           | `9999999999`         |
| Real     | Decimal notation         | `3.14`               |
| Double   | Decimal notation         | `3.14`               |
| Boolean  | `true` or `false`        | `true`               |
| String   | Raw content (no parens)  | `hello`              |
| Name     | Raw name (no `/` prefix) | `greeting`           |
| Byte     | Single byte character    | `A` (from 65b)       |
| Address  | Trix literal format      | `16#7f8a2c#a`        |
| Array    | Type name (not a dump)   | `array`              |
| Dict     | Type name (not a dump)   | `dict`               |
| Null     | Type name                | `null`               |
| Operator | Operator name            | `add`                |
| Other    | Type-specific format     | (same as `=` output) |

**Empty interpolation** (`\{}`) raises `SyntaxError`.

**Undefined name** raises `Undefined` at scan time.

**Implementation:** The interpolation writes directly into the string build buffer
via `PrintFmt::process_object()` with no intermediate allocation.  The name is read
into a stack-local buffer bounded by `MaxNameLength` (127 bytes).

```
/name (World) def
/n 42 def
(Hello \{name}, you are number \{n}!)   % "Hello World, you are number 42!"

/pi 3.14159 def
(pi = \{pi})                            % "pi = 3.14159"

(\{:systemdict:numbers:real-type:pi})   % "3.1415927" (dict path lookup)
```

Interpolation only applies to text strings `(...)`.  It is not available in hex
strings `<...>` or raw strings `<(...)>`.  Literal braces do not need escaping —
`({hello})` produces the string `{hello}` because only `\{` triggers interpolation.

### 5.2 Hex strings: `< ... >`

```
< hex-digits-and-whitespace >
```

Each pair of hex digits (`0`–`9` `A`–`F` `a`–`f`) encodes one byte. Whitespace is
ignored. The number of hex digits must be even; an odd count raises `SyntaxError`.
An invalid (non-hex, non-whitespace) character raises `SyntaxError`.

```
<48 656C 6C6F>   → string "Hello"  (5 bytes)
<>               → empty string
```

Encoding efficiency: 2 ASCII bytes per binary byte (50%).

### 5.3 Raw strings: `<( ... )>`

```
<( raw-string-content )>
```

A raw string where backslash (`\`) has no special meaning -- it is stored as a literal byte.
Parentheses nest: an unescaped `(` increments a nesting counter, and `)` only ends the
string when the counter is 0. After the closing `)`, a `>` is required to complete the
`<(...)>` form; a missing `>` raises `SyntaxError`.

**No escape processing:** All characters between `<(` and `)>` are stored verbatim. `\n` is
two bytes (backslash + `n`), not a newline. `\x41` is four bytes, not `A`. The only
characters with special meaning inside a raw string are `(` and `)` (for nesting) and
bare CR (normalized to LF, same as text strings).

Raw strings are particularly useful for regular expression patterns, which would otherwise
require double-escaping:

```
<(\d+\.\d+)>       --> 8 bytes: \d+\.\d+   (raw)
(\\d+\\.\\d+)      --> 8 bytes: \d+\.\d+   (equivalent escaped form)

<([a-z]+\s*=\s*)>  --> raw regex pattern, readable
([a-z]+\\s*=\\s*)  --> same pattern, double-escaped
```

The suffix grammar (Section 5.4) applies after the closing `>`:

```
<(hello)>#r        --> raw, read-only string
<(temp)>#=         --> raw, temp =string storage
<(A)>#b            --> raw, byte extraction (Byte 65)
```

### 5.4 String suffixes

After the closing delimiter of any string form, an optional `#` introduces a suffix:

```
#[a[r|w] | b | [=|$|$$][l|x][r|w]]
```

**`#a` — Byte array from string**

Converts the string into an Array of Byte objects (one per byte).
Default is read-write; `#ar` makes the array read-only.

```
(hello)#a    → Array [104 101 108 108 111]  (read-write)
(AB)#ar      → Array [65 66]                (read-only)
```

**`#b` — Byte from single-char string**

Extracts the first (and only) byte of the string and returns a `Byte` object. The string
must have exactly length 1; otherwise `SyntaxError`.

```
(A)#b    → Byte 65
<41>#b   → Byte 65
```

**`#=` — Use temporary =string storage**

Copies the string bytes into the pre-allocated `=string` scratch buffer instead of
allocating new VM heap space. The string object then references the =string. Useful for
temporary strings that don't need to outlive the current operation. The =string has a
fixed maximum capacity; exceeding it raises `LimitCheck`.

**`l` / `x` — Literal or Executable attribute**

- `l` (default): String is a `LiteralValue`
- `x`: String is an `ExecutableValue` (the interpreter will evaluate it as a program when executed)

**`r` / `w` — Access mode**

- `r`: ReadOnly access (string contents cannot be modified)
- `w` (default): ReadWrite access

**Suffix order:** `=` must come first (if present), then `l`/`x`, then `r`/`w`. All
letters follow the initial `#` without a second `#`.

**`#$` — Force global VM storage**

Allocates the string bytes in the global VM region.  Survives `save` /
`restore`; reclaimed by mark-sweep GC.  Mutually exclusive with `#=` and
`#$$` (any pair raises `SyntaxError`).  See § 12a "Global VM Scoping" for
the broader directive family.

**`#$$` — Force local VM storage**

Allocates the string bytes in the local VM region, overriding any
enclosing `${...}` block or `set-global true`.  Symmetric inverse of
`#$`.  Outside any global scope, `#$$` is a no-op (strings already
commit locally).  Mutually exclusive with `#=` and `#$`.

**Full suffix grammar:**
```
#a[r|w]                  → Byte array (each byte becomes an Array element)
#b                       → Byte (length-1 string only)
#[=|$|$$][l|x][r|w]      → String with given storage / attributes
```

Valid combinations:
```
(hello)         → literal, read-write string (default)
(hello)#=       → literal, read-write, temp storage
(hello)#x       → executable string
(hello)#r       → literal, read-only string
(hello)#=xr     → executable, read-only, temp storage
(A)#b           → Byte value 65
```

The suffix (if present) must be immediately followed by a terminator character; otherwise
`SyntaxError`.

---

## 6. Array Literals

```
[ elements ]#[=|$|$$][r|w]
```

`[` pushes a Mark onto the operand stack. `]` collects everything above the nearest Mark
into a new Array and pops the Mark.

Arrays can be nested: `[1 [2 3] 4]` produces a 3-element array whose second element is
the 2-element array `[2 3]`.

**Suffix grammar:**
```
#[=|$|$$][r|w]
```

| Suffix   | Meaning                                                            |
| -------- | ------------------------------------------------------------------ |
| *(none)* | ReadWrite; follows enclosing `m_curr_alloc_global` (default local) |
| `#=`     | ReadWrite, temporary =array storage                                |
| `#r`     | ReadOnly                                                           |
| `#w`     | ReadWrite (explicit)                                               |
| `#=r`    | ReadOnly, temporary storage                                        |
| `#=w`    | ReadWrite, temporary storage (explicit)                            |
| `#$`     | ReadWrite, force global VM (survives save/restore; GC-reclaimed)   |
| `#$$`    | ReadWrite, force local VM (overrides enclosing `${...}`)           |

`#=`, `#$`, and `#$$` are mutually exclusive in the first slot; any pair raises `SyntaxError`.

```
[1 2 3]         → read-write array [1, 2, 3]
[1 2 3]#r       → read-only array [1, 2, 3]
[1 2 3]#=       → read-write, temp storage
[]              → empty read-write array
```

---

## 7. Dict Literals

```
<< key0 value0 key1 value1 ... >>#[=|$|$$][r|w]
```

`<<` pushes a Mark. `>>` collects key-value pairs above the Mark into a new Dict and pops
the Mark. Keys are typically Name objects (`/key`); values may be any type.

The number of elements above the mark must be even (one value per key); an odd count
raises `RangeCheck`.

**Suffix grammar:** same as arrays — `#[=|$|$$][r|w]`.

| Suffix | Mode |
| --- | --- |
| *(none)* | ReadWriteFixed; follows enclosing `m_curr_alloc_global` |
| `#r` | ReadOnly |
| `#=` | ReadWriteFixed, temporary =dict storage |
| `#$` | ReadWriteFixed, **dict struct** force global VM (keys are still interned per their own `/foo` vs `$/foo` form) |
| `#$$` | ReadWriteFixed, **dict struct** force local VM (overrides enclosing `${...}`) |

```
<< /x 1 /y 2 >>          → dict {x→1, y→2}, fixed capacity 2, R/W
<< /x 1 /y 2 >>#r        → read-only dict
<< >>                     → empty dict, fixed capacity 4, R/W
```

**Minimum capacity:** `<< >>` (zero entries) creates a dict with a minimum fixed capacity
of 4, so it can accept entries via `put` without requiring `N dict` or `N dynamic-dict`.
Dicts with 1-3 entries also get capacity 4. Dicts with 4+ entries get exact capacity.

**Dynamic dicts:** The `N dynamic-dict` operator creates a dict that expands
automatically when full. However, the hash bucket count is fixed at creation time
based on the initial capacity. Each expansion adds entry slots but does **not** rehash
or add buckets, so the load factor grows proportionally and average lookup degrades
from O(1) toward O(N). Dynamic dicts are intended for dicts that grow infrequently
and are not on hot lookup paths. For performance-critical dicts, use `N dict` with a
capacity that matches the expected final size.

---

## 7a. Set Literals

```
{{ element0 element1 element2 ... }}#[=|$|$$][r|w]
```

`{{` pushes a Mark. `}}` collects elements above the Mark into a new Set and pops
the Mark. Elements may be any hashable type except Null (Inf and NaN are permitted in
sets, unlike dict keys which reject them), and a single set may contain keys of different
types (heterogeneous).
Duplicate keys are silently deduplicated.

**Suffix grammar:** same as dicts -- `#[=|$|$$][r|w]`.

| Suffix   | Mode                                                                         |
| -------- | ---------------------------------------------------------------------------- |
| *(none)* | ReadWriteFixed; follows enclosing `m_curr_alloc_global`                      |
| `#r`     | ReadOnly                                                                     |
| `#w`     | ReadWriteFixed (explicit, same as default)                                   |
| `#=`     | ReadWriteFixed, temporary =set storage                                       |
| `#$`     | ReadWriteFixed, **set struct** force global VM                               |
| `#$$`    | ReadWriteFixed, **set struct** force local VM (overrides enclosing `${...}`) |

<!-- doctest: skip (set-literal syntax illustration with prose annotations) -->
```
{{ 1 2 3 }}               -> integer set, fixed capacity 4, R/W
{{ /a /b /c }}            -> name set, fixed capacity 4, R/W
{{ 1 /a (hello) true }}   -> mixed-type set, fixed capacity 4, R/W
{{ 1 2 3 }}#r             -> read-only set
{{ }}                     -> empty set, fixed capacity 4, R/W
{{ /a /a /b }}            -> set {a, b}, duplicates removed
```

**Minimum capacity:** `{{ }}` (zero elements) creates a set with minimum fixed capacity
of 4. Sets with 1-4 elements also get capacity 4. Sets with 5+ elements get exact capacity.

**Inside procedures:** `{{ }}` compiles correctly into packed procedures. The packed
representation shares `PackedType::Dict`; the Set identity is reconstructed at
unpack time by reading the `SetFlag` bit in the Dict header.

**VM structure:** Sets share Dict's hash table layout with compact 12-byte `SetEntry`
(chain pointer + key Object) versus Dict's 20-byte `Entry` (chain + key Object + value Object).

---

## 8. Procedure Literals

```
{ elements }#[=|$|$$][a|p][e|l][r|w]
```

`{` begins a procedure body. The scanner recurses, collecting all elements until the
matching `}`. The result is an Array or Packed array with the `ExecutableAttrib` set.

Nesting depth is limited to **64** levels (C-stack overflow guard). Exceeding this raises
`SyntaxError`.

### 8.1 Procedure storage forms

| Suffix letter   | Meaning                                                          |
| --------------- | ---------------------------------------------------------------- |
| *(none)* or `p` | **Packed** — compressed representation (default; see Section 11) |
| `a`             | **Array** — ordinary uncompressed `Array` object                 |

### 8.2 Binding

| Suffix letter | Meaning |
| --- | --- |
| *(none)* or `l` | **Late binding** — names are looked up at execution time (default) |
| `e` | **Early binding** — executable names are replaced with their current operator values at compile time (like `bind`) |

### 8.3 Access mode (Array only)

| Suffix letter          | Meaning   |
| ---------------------- | --------- |
| `w` (default for `#a`) | ReadWrite |
| `r`                    | ReadOnly  |

Packed arrays are always ReadOnly; `#pw` is explicitly prohibited and raises
`Unsupported`.

### 8.4 Storage class (first slot)

The first slot of the suffix selects the storage class.  `=`, `$`, and `$$`
are mutually exclusive:

| Suffix   | Meaning                                                                 |
| -------- | ----------------------------------------------------------------------- |
| *(none)* | Follow the enclosing `m_curr_alloc_global` (default: local VM heap)     |
| `=`      | Pre-allocated `=proc` scratch (fixed maximum; exceeds → `LimitCheck`)   |
| `$`      | Force global VM (survives `save`/`restore`; reclaimed by mark-sweep GC) |
| `$$`     | Force local VM (overrides enclosing `${...}` or `set-global true`)      |

Any pairwise combination of `=`, `$`, `$$` raises `SyntaxError`.

### 8.5 Full suffix grammar

```
#[=|$|$$][a|p][e|l][r|w]
```

All letters follow a single `#` in the order above. Suffix must be followed by a
terminator.

Examples:

```
{ dup exch }            → packed, late-binding, read-only (default)
{ dup exch }#a          → array, late-binding, read-write
{ dup exch }#ae         → array, early-binding, read-write
{ dup exch }#e          → packed, early-binding, read-only
{ dup exch }#ar         → array, late-binding, read-only
{ dup exch }#=          → packed, late-binding, temp storage
{ dup exch }#aer        → array, early-binding, read-only
{ dup exch }#$          → packed, late-binding, read-only, global VM
{ dup exch }#$a         → array, late-binding, read-write, global VM
{ dup exch }#$aer       → array, early-binding, read-only, global VM
```

### 8.6 Local variable bindings

A procedure may begin with a **locals preamble** `|name1 name2 ...|` that declares
lexically scoped local variables:

```
{ |a b c| body... }
```

The scanner detects `|` as the first non-whitespace character after `{`, then scans
names (separated by whitespace) until the closing `|`. The body following the closing
`|` is compiled as a nested procedure.

At execution time, the local names are bound as keys in a **frame dict** (per-call
dict, analogous to a C/C++ stack frame; see the Glossary in `trix-reference.md`)
pushed onto the dict stack. Arguments are consumed from the operand stack
(rightmost name bound first). The frame dict is automatically popped when the
procedure returns.

```
{ |x| x x mul } 5 exch exec    % → 25  (x = 5)
{ |a b| a b add } 3 4 rot exec % → 7   (a = 3, b = 4)
{ |n| } 42 exch exec           % → (empty body, n discarded)
```

**Capacity suffix `|names|#N` (absolute):** the frame dict is sized for exactly the
declared name count by default, which leaves no room for working variables defined by
`/x … def` inside the body. Append `#N` immediately after the closing `|` (no whitespace)
to request a frame dict of capacity `N ≥ K`, reserving `N - K` slots for body-local `def`s:

```
{ |a b|#8 /tmp a b add def tmp tmp mul } 3 4 rot exec  % → 49 (tmp = 7, tmp*tmp)
```

**Capacity suffix `|names|#+N` (relative):** `|a b|#+N` requests `N` *additional* slots
beyond the named args — total capacity is `K + N`. This is usually the form authors
actually want: it lets you add or remove a named arg without re-counting the suffix, and
the number you write equals the number of `local-def`s the body needs.

```
{ |a b|#+6 /tmp a b add def tmp tmp mul } 3 4 rot exec  % → 49 (2 named + 6 extras)
```

The two forms can coexist freely in the same file. Both share the same constraint:
total capacity (`N` for absolute, `K + N` for relative) must be `≤ 65535`.

**Empty-header form `||#N` / `||#+N`:** `K = 0` is allowed *only* when a non-zero
capacity suffix is supplied. This declares a scratch frame dict with no stack-popped
bindings — useful when a proc takes no arguments but still wants frame-dict semantics
(allocator pooling, save/restore transparency) for its `/foo def` locals. A bare `||`
with no suffix, `||#0`, or `||#+0` is a `SyntaxError`.

`N` is a decimal integer in both forms. Whitespace *or a comment* between `|` and `#`
disables the suffix (the `#…` becomes regular body content). This is the shared
"lexical suffixes are contiguous" rule from § 2.1.

**Error conditions:**
- Missing closing `|` → `SyntaxError`
- Empty binding without capacity suffix (`{ || body }`, `{ ||#0 body }`, `{ ||#+0 body }`) → `SyntaxError`
- Locals names must consist of regular characters only (same rules as name tokens)
- `#` not followed by a decimal digit (or `+` then digit) → `SyntaxError`
- Absolute `#N` with `N < K` (capacity smaller than declared count) → `SyntaxError`
- Capacity exceeds `65535` (`N > 65535` absolute, or `K + N > 65535` relative) → `LimitCheck`

Local variable bindings are compiled to the `begin-locals` / `@end-locals` operator pair
internally; the preamble is not a separate runtime concept.

### 8.7 Inline arrays, dicts, and sets inside procedures

`[...]`, `<< ... >>`, and `{{ ... }}` can appear anywhere inside `{ }` — they compile
correctly into the procedure body. When the procedure executes, the embedded mark is
processed in the normal way, building the array, dict, or set at execution time.

```
{ [1 2 3] }            → procedure that pushes [1, 2, 3]
{ << /k 7 >> }         → procedure that pushes a dict {k→7}
{ {{ 1 2 3 }} }        → procedure that pushes a set {1, 2, 3}
{ [1 2] [3 4] }        → procedure that pushes two arrays
```

---

## 8a. Infix Expressions

```
$( expr )              strict infix (no implicit type coercion)
$[ expr ]              auto-promote infix (promote before each binary op)
```

`$( )` and `$[ ]` are scanner-level syntactic sugar for infix math expressions.
The scanner parses the expression with a Pratt parser (precedence climbing) and
emits equivalent postfix tokens.  No new runtime operators or types are introduced.

### 8a.1 Delimiter recognition

`$` remains a regular character (RG).  The scanner recognizes the following
prefix forms in `scan_number_or_name()`:

  * `$(` opens a strict infix expression, closed by `)`.
  * `$[` opens a promoted (auto-widening) infix expression, closed by `]`.
    `[` and `]` are not infix operators, so they serve as unambiguous
    delimiters; the `$` peek-dispatches before `[` could be read as an
    array-literal opener.
  * `${` opens a force-global allocation scope block, closed by `}`.
  * `$/` and `$\` open force-global literal / executable name prefixes.
  * `$${` opens a force-local allocation scope block.
  * `$$/` and `$$\` open force-local literal / executable name prefixes.

Otherwise `$` is part of a normal name token.

### 8a.2 Precedence table

From highest to lowest binding:

| Prec | Operators         | Assoc | Postfix emission           |
| ---- | ----------------- | ----- | -------------------------- |
| --   | `( )`             | --    | grouping                   |
| --   | unary `+` `!` `~` | right | no-op, `not`, `not`        |
| 13   | `**`              | right | `pow`                      |
| 12   | `*` `/` `%`       | left  | `mul` `div` `mod`          |
| 11   | `+` `-`           | left  | `add` `sub`                |
| 10   | `<<` `>>`         | left  | `shift-left` `shift-right` |
| 9    | `<` `<=` `>` `>=` | left  | `lt` `le` `gt` `ge`        |
| 8    | `==` `!=`         | left  | `eq` `ne`                  |
| 7    | `&` `&&`          | left  | `and`                      |
| 6    | `^`               | left  | `xor`                      |
| 5    | `\|` `\|\|`       | left  | `or`                       |
| 4    | `? :`             | right | `rot` `select` (ternary)   |

Unary `-` is the exception: it binds **looser** than `**` (it recurses into
the precedence loop at min-prec 13), so `-2 ** 2` parses as `-(2 ** 2) == -4`
(the Python/MATLAB convention), not `(-2) ** 2`.  The other three prefix
operators (`+` `!` `~`) are handled by `infix_primary` and bind tightest.

`!` and `~` are synonyms (both emit `not`).  Use whichever reads better:
`!` for boolean intent, `~` for bitwise intent.

`&&` and `||` are synonyms for `&` and `|` (same precedence, same emission).
Use whichever reads better for the context.

### 8a.3 Function call syntax

```
name(arg1, arg2, ...)
```

Arguments are pushed left-to-right, then the function name is emitted.
Hyphenated names are supported (e.g. `kahan-sum`).  Nullary calls use empty
parens: `clock()`.

```
$( sqrt(9.0) )              → 9.0 sqrt
$( max(3, 7) )              → 3 7 max
$( abs(min(3, -5)) )        → 3 -5 min abs
$( kahan-sum(data) )        → data kahan-sum
```

### 8a.4 Number literals

All Trix number forms are recognized inside `$( )`:

- Type suffixes: `42u`, `100l`, `3.14d`, `65b`
- Radix: `16#FF#`, `2#1010#u`
- Prefix: `0xFF`, `0o77`, `0b1010`
- Underscore separators: `1_000_000`, `16#FF_FF#`
- Scientific notation: `1.5e-3`, `2.5E+2`
- Hex floats: `0x1.0p+10`, `0x1.8p-2`

### 8a.5 Name resolution

Bare identifiers are emitted as executable names (looked up at runtime):

```
$( x + 1 )                  → x 1 add
$( pi * r * r )             → pi r r mul mul
```

### 8a.6 Auto-promote mode

`$[ ]` emits `promote` before each binary operator emission.  `promote` is a
no-op when operand types already match; otherwise it widens both operands to a
common type.

```
$[ 3 + 4.0 ]                → 3 4.0 promote add    (result: 7.0)
$[ 3 + 4 ]                  → 3 4 promote add       (no-op promote)
```

### 8a.7 Parentheses and nesting

Parentheses inside `$( )` are **grouping only** -- not string literals.
`$( )` cannot nest inside itself.  To combine infix with postfix, use multiple
`$( )` blocks:

```
$( a + b ) $( c * d ) add   % two infix blocks, then postfix add
```

### 8a.8 Token buffer

The Pratt parser emits postfix tokens into the `m_infix_scratch` array (a
shared scratch buffer on the Trix instance), then packs them into a per-stream
VM buffer (768 bytes of packed data, allocated lazily on first use, reused for
subsequent expressions on the same stream).  The packed representation uses
1-6 bytes per token instead of 8 bytes per Object (6-byte worst case: header +
optional subcode + 4-byte offset); 768 bytes is the provable worst case for
128 tokens.  The `scanner()` function extracts packed tokens one
at a time via `extract_next_packed()` before resuming normal scanning.  The
buffer's VM offset (`m_infix_offset`) is journaled through save/restore to
prevent stale reuse after VM rollback.

### 8a.9 Error conditions

| Condition                                | Error         |
| ---------------------------------------- | ------------- |
| Empty expression `$( )`                  | `SyntaxError` |
| Missing closing `)`                      | `SyntaxError` |
| Unexpected character in primary position | `SyntaxError` |
| Invalid number literal                   | `SyntaxError` |
| Expression exceeds 128 tokens            | `LimitCheck`  |
| Token buffer allocation fails            | `VMFull`      |

---

## 8b. Field-Access Sugar

<!-- doctest: skip (field-access rewrite illustration using placeholder names) -->
```
receiver.field             rewritten to: receiver /field get
a.b.c.d                    rewritten to: a /b get /c get /d get
```

`.name` is scanner-level syntactic sugar for `/name get`.  `.` is classified
as a delimiter (character-class `DL | FE` -- see Section 1); when the
scanner dispatches `.` it peeks the next character and routes to one of
three paths.

### 8b.1 Dispatch paths

| `.` followed by | Outcome | Route |
| --- | --- | --- |
| Digit (`0`–`9`) | Number literal (e.g. `.5` -> real 0.5) | `scan_number_or_name(trx, '.')` |
| Regular char (name-start) | Field-access sugar | `scan_field_access` |
| EOF / whitespace / delimiter / syntax-error byte / binary-token byte (0x80–0xFF) | `SyntaxError` | direct error return |

### 8b.2 Chain semantics

Inside `scan_field_access`, after emitting one `/name get` pair, the
scanner peeks the next raw byte.  If it is `.`, the dot is consumed and
another field name is scanned.  The loop terminates when the next byte
is no longer `.`, or on an error.  The first `/name` is returned to the
caller; every subsequent token (`get`, `/name2`, `get`, ...) is packed
into the same per-stream `m_infix_offset` buffer used by `$( )`.
Subsequent `scanner()` calls drain the buffer via
`extract_next_packed()` before resuming normal scanning.

### 8b.3 Number-interior dots

A decimal point inside a numeric literal (e.g. `3.14`, `1.5e10`,
`0x1.Fp4`, `3.`) is handled by a secondary loop in
`scan_number_or_name`.  After accumulating the initial regular-char run,
the scanner checks for a `.` only when the token is number-shaped (first
char is a digit, `+`, `-`, or `.` itself).  At most one `.` per token is
absorbed; the next character must be regular, whitespace, delimiter, or
EOF for the dot to be accepted.  If the composite token fails to parse
as a number, it falls through to `Name::make` and becomes a name (e.g.
`3.foo` -> the name `"3.foo"`, matching pre-delimiter behavior).

### 8b.4 Token emission

The emitted name is always `/name` (literal, `LiteralAttrib`).  The
emitted operator is the well-known name `get` (`ExecutableAttrib`),
obtained via `wellknown_name(WellKnownName::Get, ExecutableAttrib)` --
the same mechanism used by the infix parser for `add`, `sub`, etc.
Because `get` is a well-known name, it dispatches polymorphically at
runtime across `Dict`, `Record`, `Array`, `String`, etc.

### 8b.5 Error conditions

| Condition                                    | Error                                                  |
| -------------------------------------------- | ------------------------------------------------------ |
| Lone `.` at EOF                              | `SyntaxError` "expected digit or field name after '.'" |
| `.` followed by a syntax-error byte          | `SyntaxError`                                          |
| Empty field name after chained `.`           | `SyntaxError` "expected field name after '.'"          |
| Field name exceeds 127 chars                 | `LimitCheck`                                           |
| Chain exceeds 64 accesses (= 128 tokens / 2) | `LimitCheck`                                           |
| Buffer allocation fails                      | `VMFull`                                               |

### 8b.6 Grammar

```
FieldAccess     ::= '.' FieldChain
FieldChain      ::= FieldName ( '.' FieldName )*
FieldName       ::= RegularStart RegularChars?
RegularStart    ::= { RG chars except decimal digits 0-9 }
RegularChars    ::= { chars with RG attribute }  { one or more }
```

Rewrite:
```
'.' FieldName   -> /FieldName get
FieldAccess     -> /f1 get /f2 get ... /fn get
```

A `FieldName` may not begin with a decimal digit: `.<digit>` dispatches to the
number scanner at the first dot (so `x.5` is the receiver `x` then real `0.5`, not
field access), and after a chained `.` a leading digit is a `SyntaxError`.

---

## 9. Mark Tokens

`[`, `<<`, and `{{` all produce a `Mark` object (`Type::Mark`). They are pushed as
`LiteralValue` tokens. `]`, `>>`, and `}}` consume all objects above the most recent
Mark to construct an array, dict, or set respectively.

A lone `>` (not followed by `>`) is a `SyntaxError`.
A lone `}` (not followed by `}`) ends a procedure, not a set.
A lone `]`, `>>`, or `}}` when no matching mark exists raises `UnmatchedMark`.

---

## 10. Binary Tokens (0x80–0xFF)

Any byte 0x80–0xFF in the input starts a binary token. The byte encodes the token type;
most types are immediately followed by a value payload of 0–8 bytes. All multi-byte
integer payloads are read in the endianness specified by the token name (`b` = big-endian,
`l` = little-endian, no suffix = native).

### 10.1 Token table

```
0x80  Byte              1 byte payload (uint8)
0x81  Integer_8         1 byte signed  → Integer
0x82  Integer_16        2 bytes native → Integer
0x83  Integer_16b       2 bytes big    → Integer
0x84  Integer_16l       2 bytes little → Integer
0x85  Integer_32        4 bytes native → Integer
0x86  Integer_32b       4 bytes big    → Integer
0x87  Integer_32l       4 bytes little → Integer
0x88  UInteger_8        1 byte         → UInteger
0x89  UInteger_16       2 bytes native → UInteger
0x8A  UInteger_16b      2 bytes big    → UInteger
0x8B  UInteger_16l      2 bytes little → UInteger
0x8C  UInteger_32       4 bytes native → UInteger
0x8D  UInteger_32b      4 bytes big    → UInteger
0x8E  UInteger_32l      4 bytes little → UInteger
0x8F  Long_8            1 byte signed  → Long
0x90  Long_16           2 bytes native → Long
0x91  Long_16b          2 bytes big    → Long
0x92  Long_16l          2 bytes little → Long
0x93  Long_32           4 bytes native → Long
0x94  Long_32b          4 bytes big    → Long
0x95  Long_32l          4 bytes little → Long
0x96  Long_64           8 bytes native → Long
0x97  Long_64b          8 bytes big    → Long
0x98  Long_64l          8 bytes little → Long
0x99  ULong_8           1 byte         → ULong
0x9A  ULong_16          2 bytes native → ULong
0x9B  ULong_16b         2 bytes big    → ULong
0x9C  ULong_16l         2 bytes little → ULong
0x9D  ULong_32          4 bytes native → ULong
0x9E  ULong_32b         4 bytes big    → ULong
0x9F  ULong_32l         4 bytes little → ULong
0xA0  ULong_64          8 bytes native → ULong
0xA1  ULong_64b         8 bytes big    → ULong
0xA2  ULong_64l         8 bytes little → ULong
0xA3  Real              4 bytes native (float32) → Real
0xA4  Real_b            4 bytes big
0xA5  Real_l            4 bytes little
0xA6  Double            8 bytes native (float64) → Double
0xA7  Double_b          8 bytes big
0xA8  Double_l          8 bytes little
0xA9  Fixed             PostScript fixed-point (rarely used)
0xAA  False             no payload → Boolean false
0xAB  True              no payload → Boolean true
0xAC  Mark              no payload → Mark
0xAD  Null              no payload → Null
0xAE  String_8          1 byte length, then N bytes → String
0xAF  String_16         2 bytes native length, then N bytes → String
0xB0  String_16b        2 bytes big-endian length
0xB1  String_16l        2 bytes little-endian length
0xB2  SystemLitName_8   1 byte system-name index → literal system Name
0xB3  SystemLitName_16  2 bytes native index
0xB4  SystemLitName_16b 2 bytes big-endian index
0xB5  SystemLitName_16l 2 bytes little-endian index
0xB6  SystemExecName_8  1 byte index → executable system Name
0xB7  SystemExecName_16 2 bytes native index
0xB8  SystemExecName_16b
0xB9  SystemExecName_16l
0xBA  SystemLitValue_8  1 byte index → value bound to system name, pushed literal
0xBB  SystemLitValue_16 2 bytes native index
0xBC  SystemLitValue_16b
0xBD  SystemLitValue_16l
0xBE  SystemExecValue_8 1 byte index → value bound to system name, pushed executable
0xBF  SystemExecValue_16
0xC0  SystemExecValue_16b
0xC1  SystemExecValue_16l
0xC2  Integer_2         Integer value 2 (constant, no payload)
0xC3  Integer_10        Integer value 10 (constant, no payload)
0xC4  Integer_100       Integer value 100 (constant, no payload)
0xC5  Byte_2            Byte value 2 (constant, no payload)
0xC6  NumberArray       compact array of numeric values (see below)
```

**Constant tokens (no payload):**

```
0xC7  Byte_0            Byte value 0
0xC8  Byte_1            Byte value 1
0xC9  Byte_127          Byte value 127
0xCA  Byte_128          Byte value 128
0xCB  Byte_255          Byte value 255
0xCC  Integer_Min       Integer INT_MIN (−2 147 483 648)
0xCD  Integer_Max       Integer INT_MAX (2 147 483 647)
0xCE  Integer_Neg1      Integer −1
0xCF  Integer_0         Integer 0
0xD0  Integer_1         Integer 1
0xD1  UInteger_Min      UInteger 0
0xD2  UInteger_Max      UInteger UINT_MAX (4 294 967 295)
0xD3  UInteger_1        UInteger 1
0xD4  Long_Min          Long INT64_MIN
0xD5  Long_Max          Long INT64_MAX
0xD6  Long_Neg1         Long −1
0xD7  Long_0            Long 0
0xD8  Long_1            Long 1
0xD9  ULong_Min         ULong 0
0xDA  ULong_Max         ULong UINT64_MAX
0xDB  ULong_1           ULong 1
0xDC  Real_Neg1         Real −1.0
0xDD  Real_0            Real 0.0
0xDE  Real_1            Real 1.0
0xDF  Real_e            Real e (Euler's number ≈ 2.71828)
0xE0  Real_pi           Real π (≈ 3.14159)
0xE1  Real_INF          Real +∞
0xE2  Real_qNAN         Real quiet NaN
0xE3  Double_Neg1       Double −1.0
0xE4  Double_0          Double 0.0
0xE5  Double_1          Double 1.0
0xE6  Double_e          Double e
0xE7  Double_pi         Double π
0xE8  Double_INF        Double +∞
0xE9  Double_qNAN       Double quiet NaN
0xEA  Address_nullptr   Address null pointer (0)
0xEB  Integer_Neg2      Integer value −2
0xEC  Real_2            Real value 2.0
0xED  Double_2          Double value 2.0
```

**Well-known names and 128-bit integers:**

```
0xEE  WellKnownLitName   1-byte WellKnownName index → literal Name
0xEF  WellKnownExecName  1-byte WellKnownName index → executable Name
0xF0  Int128             16 bytes native payload → Int128
0xF1  UInt128            16 bytes native payload → UInt128
```

The well-known-name tokens index a compact table of names the runtime itself
emits (arithmetic operators, `get`, control-flow primitives); `Int128`/`UInt128`
carry only a native-endian form (no big/little variants).

**Reserved (error):**
```
0xF2–0xFF  → Unsupported error (14 reserved slots)
```

### 10.2 Name vs Value tokens

- **Name tokens** (`SystemLitName`, etc.) push a Name object (the key
  itself), analogous to `/name` in text form.
- **Value tokens** (`SystemLitValue`, etc.) push the value *currently
  bound* to that name, analogous to `//name` in text form.
### 10.3 NumberArray token (0xC6)

A compact, typed array of numbers. Format:

```
0xC6  <element_type: uint8>  <count: uint16>  <element_data ...>
```

Bit 7 of the `element_type` byte selects endianness (0 = big-endian, 1 = little-endian);
the count and every multi-byte element are read in that endianness. The low 7 bits are the
element-type code.

`element_type` is a representation code 0–56 (distinct from BinaryToken opcodes):
0–31 = 32-bit fixed-point (scale = code), 32–47 = 16-bit fixed-point (scale = code−32),
48 = Byte, 49 = Integer16, 50 = UInteger16, 51 = Integer32, 52 = UInteger32, 53 = Long,
54 = ULong, 55 = Real, 56 = Double. Codes > 56 raise `Unsupported`. All elements have the
same type; `element_data` contains `count` values packed sequentially.

---

## 11. Packed Array Encoding

Procedures compiled to the packed format use an internal byte stream. This section
describes the encoding for readers of raw packed data.

### 11.1 Header byte layout

Each element in a packed array starts with a single header byte:

```
Bit 7 (X): Executable attribute (1) or Literal attribute (0)
Bits 6–5 (SS): Value size-1: 00=0 bytes, 01=1 byte, 10=2 bytes, 11=3 bytes
Bits 4–0 (TTTTT): PackedType (0–31)
```

For `CommonOp` and `Simple` types the X and SS bits serve as sub-selectors rather than
the normal attribute/size roles.

### 11.2 PackedType values

| Value | Name                        | Description                                                    |
| ----- | --------------------------- | -------------------------------------------------------------- |
| 0     | `CommonOp`                  | Hot operator — X\|SS selects one of 8 operators (see below)    |
| 1     | `Byte`                      | 1-byte Byte value                                              |
| 2     | `Integer`                   | 1–4 byte signed Integer (sign-aware encoding)                  |
| 3     | `PackedExt`                 | 128-bit (Int128/UInt128) via subcode byte; X=0, SS=offset size |
| 4     | `UInteger`                  | 1–4 byte UInteger                                              |
| 5     | `Long`                      | ExtValue-stored 64-bit (offset is 1–4 bytes)                   |
| 6     | `ULong`                     | ExtValue-stored 64-bit (offset is 1–4 bytes)                   |
| 7     | `Address`                   | ExtValue-stored Address                                        |
| 8     | `Real`                      | 4-byte Real                                                    |
| 9     | `Double`                    | ExtValue-stored 64-bit float (offset is 1–4 bytes)             |
| 10    | `Simple`                    | Zero-payload; SS selects: 00=Null, 01=Mark, 10=False, 11=True  |
| 12    | `Operator`                  | Operator by sign-aware index                                   |
| 13    | `Reserved2`                 | Reserved                                                       |
| 14    | `Record`                    | Record; X=fc width (0=1B, 1=2B), SS=offset size                |
| 15    | `Name`                      | Name by index in name table                                    |
| 16    | `ShortLengthArray`          | R/W Array, 1-byte length                                       |
| 17    | `LongLengthArray`           | R/W Array, 2-byte length                                       |
| 18    | `ReadOnlyShortLengthArray`  | R/O Array, 1-byte length                                       |
| 19    | `ReadOnlyLongLengthArray`   | R/O Array, 2-byte length                                       |
| 20    | `ReadOnlyShortLengthPacked` | R/O Packed, 1-byte length                                      |
| 21    | `ReadOnlyLongLengthPacked`  | R/O Packed, 2-byte length                                      |
| 22    | `ShortLengthString`         | R/W String, 1-byte length                                      |
| 23    | `LongLengthString`          | R/W String, 2-byte length                                      |
| 24    | `ReadOnlyShortLengthString` | R/O String, 1-byte length                                      |
| 25    | `ReadOnlyLongLengthString`  | R/O String, 2-byte length                                      |
| 26    | `ShortLengthStream`         | R/W Stream, 1-byte length                                      |
| 27    | `LongLengthStream`          | R/W Stream, 2-byte length                                      |
| 28    | `ReadOnlyShortLengthStream` | R/O Stream, 1-byte length                                      |
| 29    | `ReadOnlyLongLengthStream`  | R/O Stream, 2-byte length                                      |
| 30    | `Dict`                      | Dict                                                           |

Value 11 is `Curry` (partial application). Tagged shares Curry's slot (X=0
for Tagged, X=1 for Curry). Value 14 is `Record` (immutable named-field
composite). Record repurposes the X bit as field count width selector
(X=0: 1-byte, X=1: 2-byte) since records are always literal.

Value 3 is `PackedExt`, a subcode-dispatched slot for fixed-size heap-backed
value types. Beyond the 128-bit integers it also carries eqref-family references
(EqString / EqArray / EqProcArray / EqProcPacked / EqDict / EqSet) and OpaqueHandles.
Encoding: `[header][subcode][offset: SS+1 bytes]`. `X=0`
(always literal); `SS` gives the `vm_offset_t` byte count minus one; the
subcode byte selects the concrete type:

| Subcode    | Object type                                            |
| ---------- | ------------------------------------------------------ |
| 0x00       | Int128                                                 |
| 0x01       | UInt128                                                |
| 0x02–0x05  | EqString (Short/Long × RW/RO)                          |
| 0x06–0x09  | EqArray                                                |
| 0x0A–0x0D  | EqProcArray                                            |
| 0x0E–0x0F  | EqProcPacked                                           |
| 0x10       | EqDict                                                 |
| 0x11       | EqSet                                                  |
| 0x12       | OpaqueHandle (short kind)                              |
| 0x13       | OpaqueHandle (long kind)                               |
| 0x14..0xFF | Reserved for future fixed-size heap-backed value types |

Value 13 (`Reserved2`) remains unused.

Note: Thunk objects cannot be packed (they are runtime-only with mutable state).

### 11.3 CommonOp slot mapping

When `PackedType == CommonOp`, the `X|SS` field (bits 7–5) selects one of 8 operators
with no payload:

| X\|SS | Operator  |
| ----- | --------- |
| 0     | `exch`    |
| 1     | `dup`     |
| 2     | `pop`     |
| 3     | `index`   |
| 4     | `roll`    |
| 5     | `if`      |
| 6     | `if-else` |
| 7     | `eq`      |

### 11.4 Simple sub-type mapping

When `PackedType == Simple`, `SS` selects the value:

| SS  | Object |
| --- | ------ |
| 00  | Null   |
| 01  | Mark   |
| 10  | False  |
| 11  | True   |

---

## 12. Lexeme Summary

The scanner returns `(Lexeme, Object)` pairs:

| Lexeme            | Meaning                                                     |
| ----------------- | ----------------------------------------------------------- |
| `LiteralValue`    | Object should be pushed onto the operand stack as a value   |
| `ExecutableValue` | Object should be executed (name lookup, proc call, etc.)    |
| `EndOfProcedure`  | `}` encountered; signals the end of a `scan_procedure` call |
| `EndOfStream`     | Input exhausted normally                                    |
| `SyntaxError`     | Invalid input; the Object is a String describing the error  |

---

## 12a. Global VM Scoping

Trix runs two heaps: a local VM (a bump arena; the default; rolls back at
`restore`) and a global VM managed by the global allocator with mark-sweep
garbage collection (survives `save` / `restore`; reclaimed by GC).
The scanner exposes three families of directives for opting into
global allocation.

### 12a.1 Per-form directives

| Form          | Effect                                                      |
| ------------- | ----------------------------------------------------------- |
| `$/name`      | Literal Name interned in global VM                          |
| `$\name`      | Executable Name interned in global VM                       |
| `$$/name`     | Literal Name **force-interned in local VM** (inverse)       |
| `$$\name`     | Executable Name **force-interned in local VM** (inverse)    |
| `(str)#$`     | String bytes in global VM                                   |
| `[arr]#$`     | Array storage in global VM                                  |
| `<<dict>>#$`  | Dict **struct** in global VM (keys interned per their form) |
| `{{set}}#$`   | Set struct in global VM                                     |
| `{proc}#$`    | Procedure storage (packed bytes or Array) in global VM      |
| `(str)#$$`    | String bytes **force-local** (overrides enclosing global)   |
| `[arr]#$$`    | Array storage force-local                                   |
| `<<dict>>#$$` | Dict struct force-local                                     |
| `{{set}}#$$`  | Set struct force-local                                      |
| `{proc}#$$`   | Procedure storage force-local                               |

`#$` always means "global VM"; `#$$` always means "force-local VM"; `#=`
selects eq-temporary storage.  All three are mutually exclusive in the
first slot.  Any pair raises `SyntaxError`.

The `$$/foo` / `$$\foo` force-local forms are useful inside a `${...}`
block (or under `set-global true`) when one specific Name should stay
in local VM even though the surrounding scope is otherwise global.
Outside of any global scope they are equivalent to `/foo` / `\foo`.

### 12a.2 Scope block: `${...}` and `$${...}`

```
${ body }            force-global allocation scope
$${ body }           force-local allocation scope (inverse)
```

`${...}` scans `body` with the scanner's global-alloc flag set, then
runs `body` with the runtime flag set.  `$${...}` is the symmetric
inverse: scan + run with the flag forced to `false`.  Either form
saves the prior flag on entry and restores it on exit -- **including
on exception unwind**.  The flag is per-coroutine, so neither form
bleeds into a sibling coroutine.

`$${...}` is useful inside a `${...}` block (or under `set-global
true`) when one specific sub-region should stay local even though the
surrounding scope is otherwise global.  Outside any global scope it
is a no-op.

Within the block:
- Every literal Name interned during scan lands in global.
- Every flag-honoring allocation during runtime (the `int -- container`
  ops `array`, `dict`, `dynamic-dict`, `set`, `string`) lands in global.

```trix
${
    /populate {
        g /alpha 1 put
        g /beta  2 put
    } def
}
```

### 12a.3 Direct flag control

When `${...}` cannot reach (e.g. flag must span a procedure call):

```trix
true set-global             % bool --
\(allocations land in global\)
false set-global

current-global              % -- bool : peek the flag
```

Both ops act on the per-coroutine `m_curr_alloc_global` flag.

### 12a.4 Cross-references

See [`gvm-heap-gc.md`](gvm-heap-gc.md) for the global VM design,
allocator, and GC mechanics.  See [`trix-reference.md`](trix-reference.md)
§ Global VM Allocation for the user-facing reference.

---

## 13. Error Conditions

| Error | When |
| --- | --- |
| `SyntaxError` | Invalid or unbalanced syntax: odd hex digits in `<…>`, missing `>` in `<(…)>`, unmatched `)` / `>` / `}`, invalid suffix letter, NUL or DEL in source, unterminated `(…)` / `<…>` / `<(…)>` / `{…}` / `%{…%}` (EOF inside a string, proc, or comment), procedure nesting depth > 64, unterminated or empty local variable binding `\|...\|`, capacity suffix with `N < K`, or `#`/`#+` not followed by a decimal digit, `\u`/`\U` with insufficient hex digits or surrogate/out-of-range codepoint, mutually-exclusive suffixes both present (e.g. `#=$`, `#$=`, `#$$=`) |
| `LimitCheck` | Name length > 127; string or proc body > MaxStringLength; locals capacity > 65535; =string/=array/=proc capacity exceeded |
| `UnmatchedMark` | A lone `]`, `>>`, or `}}` with no matching `[` / `<<` / `{{` mark on the stack |
| `NumericalOverflow` | Integer/UInteger/Long/ULong/Real overflow; Real overflow to ±∞ |
| `Undefined` | Immediate lookup `//name` or `\\name` not found in dict stack |
| `Unsupported` | Illegal suffix combination (e.g. `#pw`); binary token in the reserved range (0xF2–0xFF) |
| `VMFull` | VM heap exhausted during string or proc construction |

---

## 14. Formal Grammar

This section provides an EBNF-style formal grammar for the complete Trix
lexical syntax.  Conventions:

- `'x'` — literal character or string
- `[x]` — optional
- `{x}` — zero or more repetitions
- `(x | y)` — alternation
- `x - y` — set difference (x but not y)
- `(*..*)` — comment
- Uppercase names are terminal classes from the character attribute table

```ebnf
(* ================================================================ *)
(* Top-level: token stream                                          *)
(* ================================================================ *)

program         = { token } ;

token           = whitespace
                | comment
                | numeric_or_name
                | scope_block
                | delimiter_token
                | field_access
                | binary_token ;

(* ================================================================ *)
(* Field-access sugar: .name -> /name get (see Section 8b)          *)
(* ================================================================ *)

field_access    = '.' field_chain ;
field_chain     = field_name { '.' field_name } ;
field_name      = regular_char { regular_char } ;
                  (* max length: MaxNameLength = 127 *)
                  (* desugar: each '.' field_name emits /field_name get *)

(* ================================================================ *)
(* Character classes (from chattr_t bitmask table, Section 1)       *)
(* ================================================================ *)

WS              = HT | LF | VT | FF | CR | SP ;
CT              = LF | FF | CR ;
DL              = '%' | '(' | ')' | '/' | '\' | '.' | '<' | '>' | '[' | ']'
                | '{' | '}' ;
RG              = (* printable 0x21-0x7E except DL *) ;
DD              = '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7'
                | '8' | '9' ;
OD              = '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' ;
HD              = DD | 'A' | 'B' | 'C' | 'D' | 'E' | 'F'
                     | 'a' | 'b' | 'c' | 'd' | 'e' | 'f' ;
BT              = (* bytes 0x80-0xFF *) ;
SE              = (* NUL-US except WS, plus DEL *) ;

terminator      = WS | DL | BT | EOF ;
regular_char    = RG ;
name_char       = regular_char ;

(* ================================================================ *)
(* Whitespace and comments                                          *)
(* ================================================================ *)

whitespace      = WS { WS } ;

comment         = line_comment | block_comment | shebang ;

line_comment    = '%' { (* any byte except CT and EOF *) } CT ;

block_comment   = '%{' block_body '%}' ;
block_body      = { block_char | block_comment } ;
block_char      = (* any byte except '%' and EOF *)
                | '%' (* not followed by '{' or '}' *) ;

shebang         = '#' '!' { (* any byte except CT and EOF *) } CT ;
                  (* only at byte offset 0 of a file stream *)

(* ================================================================ *)
(* Numeric literals                                                 *)
(* ================================================================ *)

numeric_or_name = regular_token ;
                  (* tried as number first; then infix check;    *)
                  (* falls through to name                       *)

regular_token   = regular_char { regular_char } ;
                  (* max length: MaxNameLength = 127 *)
                  (* if token is '$' and next char is '(':       *)
                  (*   consume '(' and enter infix strict mode   *)
                  (* if token is '$' and next char is '[':       *)
                  (*   consume '[' and enter infix auto-promote  *)

(* ================================================================ *)
(* Infix expressions                                                *)
(* ================================================================ *)

infix_expr      = '$(' infix_body ')'
                | '$[' infix_body ']' ;

(* ================================================================ *)
(* Global-VM scope blocks (see Section 12a)                         *)
(* ================================================================ *)

scope_block     = '${' { token } '}'        (* force-global allocation scope *)
                | '$${' { token } '}' ;     (* force-local allocation scope *)

infix_body      = infix_expression ;

infix_expression = infix_primary { infix_binop infix_expression }
                 | infix_expression '?' infix_expression ':' infix_expression ;
                   (* ternary: right-associative, lowest precedence *)

infix_primary   = '(' infix_expression ')'          (* grouping *)
                | '-' infix_primary                  (* unary neg *)
                | ('!' | '~') infix_primary          (* unary not *)
                | infix_number
                | infix_name '(' infix_arglist ')'   (* function call *)
                | infix_name ;                       (* bare name *)

infix_arglist   = (* empty *)
                | infix_expression { ',' infix_expression } ;

infix_binop     = '**' | '*' | '/' | '%'
                | '+' | '-'
                | '<<' | '>>'
                | '<' | '<=' | '>' | '>='
                | '==' | '!='
                | '&' | '&&' | '^' | '|' | '||' ;

infix_number    = (* any Trix numeric literal: integer, radix,   *)
                  (* prefix 0x/0o/0b, float, hex float, with     *)
                  (* optional suffix and underscore separators   *) ;

infix_name      = (letter | '_') { letter | DD | '_' | infix_hyphen } ;
infix_hyphen    = '-' (* only between two alphabetic characters *) ;

(* --- integers --- *)

integer_literal = [sign] digits [int_suffix] terminator ;

sign            = '+' | '-' ;

digits          = DD { ['_'] DD } ;
                  (* underscore only between two decimal digits *)

int_suffix      = 'b' | 'i' | 'u' | 'ui' | 'l' | 'ul' | 'a' | 'q' | 'uq'
                | 'B' | 'I' | 'U' | 'UI' | 'L' | 'UL' | 'A' | 'Q' | 'UQ' ;
                  (* case-insensitive; at most one suffix; 'ui' = explicit UInteger,
                     'q' = Int128, 'uq' = UInt128 *)

(* --- radix integers --- *)

radix_literal   = base '#' radix_digits '#' [int_suffix] terminator ;

base            = DD { DD } ;
                  (* decimal 2-36; out of range -> token parsed as a name *)

radix_digits    = HD { ['_'] HD } ;
                  (* digits valid for the given base; underscore    *)
                  (* allowed between hex-digit characters           *)

(* --- floating-point --- *)

float_literal   = [sign] float_body [float_suffix] terminator ;

float_body      = digits '.' [digits] [exponent]
                | digits exponent
                | '.' digits [exponent] ;

exponent        = ('E' | 'e') [sign] digits ;

float_suffix    = 'r' | 'd' | 'R' | 'D' ;

(* --- C-prefix integers (0x, 0o, 0b) --- *)

prefix_literal  = '0' prefix_tag prefix_digits ['#' int_suffix] terminator ;

prefix_tag      = 'x' | 'X' | 'o' | 'O' | 'b' | 'B' ;

prefix_digits   = hex_digit { ['_'] hex_digit }   (* 0x: hex digits *)
                | octal_digit { ['_'] octal_digit }  (* 0o: octal digits *)
                | bin_digit { ['_'] bin_digit } ;     (* 0b: binary digits *)
                  (* '#' separator required before suffix to avoid hex ambiguity *)

(* --- hex float literals (C99: 0x1.0p10) --- *)

hex_float       = '0' ('x' | 'X') hex_mantissa ('p' | 'P') [sign] digits
                  ['#' float_suffix] terminator ;

hex_mantissa    = hex_digit { hex_digit } ['.' hex_digit { hex_digit }]
                | '.' hex_digit { hex_digit } ;
                  (* p/P exponent is mandatory; without it, 0xFF stays integer *)

(* ================================================================ *)
(* Name tokens                                                      *)
(* ================================================================ *)

name_token      = literal_name
                | executable_name
                | global_literal_name
                | global_executable_name
                | forcelocal_literal_name
                | forcelocal_executable_name
                | immediate_literal
                | immediate_executable
                | bare_name ;

literal_name    = '/' name_body ;
                  (* bare '/' with no name_body produces name '/' *)

executable_name = '\' name_body ;
                  (* bare '\' produces name '\' *)

global_literal_name     = '$/' name_body ;     (* interned in global VM *)
global_executable_name  = '$\' name_body ;     (* interned in global VM *)
forcelocal_literal_name    = '$$/' name_body ; (* force-interned in local VM *)
forcelocal_executable_name = '$$\' name_body ; (* force-interned in local VM *)

immediate_literal    = '//' imm_body ;
immediate_executable = '\\' imm_body ;

imm_body        = path_lookup | name_body ;

path_lookup     = ':' root_dict ':' { segment ':' } leaf_name ;

root_dict       = 'systemdict' | 'userdict' | 'errordict'
                | 'handlersdict' | 'modules' | 'status'
                | systemdict_subdict ;   (* any dict entry in systemdict *)

segment         = name_body ;
leaf_name       = name_body ;

name_body       = name_char { name_char } ;
                  (* 1 to MaxNameLength characters *)

bare_name       = name_char { name_char } terminator ;
                  (* executable name without prefix; produced when *)
                  (* regular_token is not a valid number            *)

(* ================================================================ *)
(* String literals                                                  *)
(* ================================================================ *)

string_literal  = text_string | hex_string | raw_string ;

(* --- text strings --- *)

text_string     = '(' text_body ')' [string_suffix] ;

text_body       = { text_char } ;

text_char       = escape_seq
                | '(' text_body ')'         (* nested parens *)
                | (* any byte except '\', '(', ')', EOF;
                     bare CR normalized to LF *) ;

escape_seq      = '\' escape_code ;

escape_code     = 'a' | 'b' | 'e' | 'f' | 'n' | 'r' | 't' | 'v'
                | '(' | ')' | '\'
                | '^' caret_char
                | OD [OD [OD]]              (* octal: 1-3 digits, value AND 0xFF (mod 256) *)
                | 'x' HD [HD]               (* hex: 1-2 digits, 0-FF *)
                | 'u' HD HD HD HD           (* Unicode BMP: exactly 4 hex digits *)
                | 'U' HD HD HD HD HD HD HD HD
                                            (* Unicode full: exactly 8 hex digits *)
                | '{' interp_name '}'       (* scan-time interpolation *)
                | CR [LF]                   (* line continuation *)
                | LF ;                      (* line continuation *)

interp_name     = name_char { name_char } ;
                  (* 1 to MaxNameLength bytes; resolved via immediate
                     lookup at scan time; supports dict path syntax
                     e.g. :systemdict:key *)

caret_char      = '?' | '@' | 'A'..'Z' | '[' | '\' | ']' | '^' | '_' ;
                  (* 0x3F-0x5F; '?' maps to DEL, others to 0x00-0x1F *)

(* --- hex strings --- *)

hex_string      = '<' hex_body '>' [string_suffix] ;
                  (* '<' not followed by '<' or '(' *)

hex_body        = { HD | WS } ;
                  (* total HD count must be even; whitespace may appear
                     anywhere, including between the two nibbles of a byte *)

(* --- raw strings --- *)

raw_string      = '<(' raw_body ')>' [string_suffix] ;

raw_body        = { raw_char } ;

raw_char        = '(' raw_body ')'          (* nested parens *)
                | (* any byte except '(', ')', EOF;
                     bare CR normalized to LF,
                     backslash is literal *) ;

(* --- string suffix --- *)

string_suffix   = '#' ( byte_array_suffix
                       | 'b'
                       | ['='] [lit_exec] [access_mode] ) ;
                  (* #a[r|w]: Byte array -- 'a' means ARRAY, not access mode
                     #b:      single Byte (string length must be 1)
                     #[=][l|x][r|w]: string with optional storage/attrib/access *)

byte_array_suffix = 'a' [access_mode] ;
                  (* Converts string bytes into Array of Byte objects.
                     No intermediate string allocated on the VM heap.
                     Result is always literal.  Cannot combine with = b l x. *)

lit_exec        = 'l' | 'x' ;
access_mode     = 'r' | 'w' ;

(* ================================================================ *)
(* Array literals                                                   *)
(* ================================================================ *)

array_literal   = '[' { token } ']' [array_suffix] ;

array_suffix    = '#' [storage_class] [access_mode] ;

(* ================================================================ *)
(* Dictionary literals                                              *)
(* ================================================================ *)

dict_literal    = '<<' { token token } '>>' [dict_suffix] ;
                  (* even number of elements required *)

dict_suffix     = '#' [storage_class] [access_mode] ;

(* ================================================================ *)
(* Procedure literals                                               *)
(* ================================================================ *)

proc_literal    = '{' [locals_preamble] { token } '}' [proc_suffix] ;

locals_preamble = '|' [ name_body { WS name_body } ] '|' [ locals_capacity ] ;
                  (* either >= 1 name, or empty '||' followed by a non-zero capacity
                     suffix; bare '||', '||#0', and '||#+0' are SyntaxError *)

locals_capacity = '#' [ '+' ] decimal_digits ;
                  (* '#N' absolute: total = N (must be >= K)
                     '#+N' relative: total = K + N
                     total must be 1..65535 *)

proc_suffix     = '#' [storage_class] [storage_form] [binding_mode] [access_mode] ;

storage_class   = '=' | '$' | '$$' ;       (* eq-temp, force-global, force-local; mutex *)
storage_form    = 'a' | 'p' ;              (* array or packed; default: p *)
binding_mode    = 'e' | 'l' ;              (* early or late; default: l *)
                  (* access_mode for packed is always ReadOnly;
                     '#pw' raises Unsupported *)

(* ================================================================ *)
(* Mark tokens                                                      *)
(* ================================================================ *)

mark_open       = '[' | '<<' ;             (* push Mark object *)
mark_close_arr  = ']' ;                    (* collect to array *)
mark_close_dict = '>>' ;                   (* collect to dict *)

(* ================================================================ *)
(* Delimiter dispatch                                               *)
(* ================================================================ *)

delimiter_token = '(' text_string
                | ')' (* SyntaxError: unbalanced *)
                | '/' name_token
                | '\' name_token
                | '<' ( '<' mark_open       (* dict begin *)
                      | '(' raw_string
                      | hex_string )
                | '>' ( '>' mark_close_dict
                      | (* SyntaxError: unbalanced *) )
                | '[' mark_open             (* array begin *)
                | ']' mark_close_arr
                | '{' ( '{' mark_open       (* set begin {{ *)
                      | proc_literal )
                | '}' ( '}' mark_close_set  (* set end }} *)
                      | (* EndOfProcedure or SyntaxError *) )
                | '%' comment ;             (* handled before delimiter dispatch *)

(* ================================================================ *)
(* Binary tokens (0x80-0xFF)                                        *)
(* ================================================================ *)

binary_token    = numeric_bt | boolean_bt | null_bt | mark_bt
                | string_bt | name_bt | value_bt
                | numberarray_bt | constant_bt ;

(* --- numeric binary tokens --- *)

numeric_bt      = byte_bt | integer_bt | uinteger_bt | long_bt
                | ulong_bt | real_bt | double_bt | fixed_bt ;

byte_bt         = 0x80 uint8 ;

integer_bt      = 0x81 int8
                | (0x82 | 0x83 | 0x84) int16
                | (0x85 | 0x86 | 0x87) int32 ;

uinteger_bt     = 0x88 uint8
                | (0x89 | 0x8A | 0x8B) uint16
                | (0x8C | 0x8D | 0x8E) uint32 ;

long_bt         = 0x8F int8
                | (0x90 | 0x91 | 0x92) int16
                | (0x93 | 0x94 | 0x95) int32
                | (0x96 | 0x97 | 0x98) int64 ;

ulong_bt        = 0x99 uint8
                | (0x9A | 0x9B | 0x9C) uint16
                | (0x9D | 0x9E | 0x9F) uint32
                | (0xA0 | 0xA1 | 0xA2) uint64 ;

real_bt         = (0xA3 | 0xA4 | 0xA5) float32 ;

double_bt       = (0xA6 | 0xA7 | 0xA8) float64 ;

fixed_bt        = 0xA9 int32 ;             (* PostScript fixed-point *)

(* --- boolean, null, mark --- *)

boolean_bt      = 0xAA                     (* false *)
                | 0xAB ;                   (* true *)

null_bt         = 0xAD ;
mark_bt         = 0xAC ;

(* --- string binary tokens --- *)

string_bt       = 0xAE uint8 bytes
                | 0xAF uint16 bytes
                | 0xB0 uint16_be bytes
                | 0xB1 uint16_le bytes ;
                  (* length prefix, then length raw bytes *)

(* --- name binary tokens --- *)

name_bt         = sysname_bt ;

sysname_bt      = (0xB2 | 0xB3 | 0xB4 | 0xB5) index
                                            (* SystemLitName *)
                | (0xB6 | 0xB7 | 0xB8 | 0xB9) index ;
                                            (* SystemExecName *)

(* --- value binary tokens --- *)

value_bt        = sysvalue_bt ;

sysvalue_bt     = (0xBA | 0xBB | 0xBC | 0xBD) index
                                            (* SystemLitValue *)
                | (0xBE | 0xBF | 0xC0 | 0xC1) index ;
                                            (* SystemExecValue *)

(* --- number array token --- *)

numberarray_bt  = 0xC6 uint16 uint8 bytes ;
                  (* count, element_type, then count * element_size bytes *)

(* --- constant tokens (single byte, no payload) --- *)

constant_bt     = 0xC2 | 0xC3 | 0xC4                    (* Integer 2, 10, 100 *)
                | 0xC5                                   (* Byte 2 *)
                | 0xC7 | 0xC8 | 0xC9 | 0xCA | 0xCB     (* Byte constants *)
                | 0xCC | 0xCD | 0xCE | 0xCF | 0xD0      (* Integer constants *)
                | 0xD1 | 0xD2 | 0xD3                    (* UInteger constants *)
                | 0xD4 | 0xD5 | 0xD6 | 0xD7 | 0xD8     (* Long constants *)
                | 0xD9 | 0xDA | 0xDB                    (* ULong constants *)
                | 0xDC | 0xDD | 0xDE | 0xDF | 0xE0     (* Real constants *)
                | 0xE1 | 0xE2                            (* Real INF, qNaN *)
                | 0xE3 | 0xE4 | 0xE5 | 0xE6 | 0xE7     (* Double constants *)
                | 0xE8 | 0xE9                            (* Double INF, qNaN *)
                | 0xEA                                   (* Address nullptr *)
                | 0xEB                                   (* Integer -2 *)
                | 0xEC                                   (* Real 2.0 *)
                | 0xED ;                                 (* Double 2.0 *)

(* 0xEE-0xEF: well-known-name tokens; 0xF0-0xF1: Int128/UInt128;
   0xF2-0xFF: reserved, raise Unsupported *)

(* ================================================================ *)
(* Binary token payload types                                       *)
(* ================================================================ *)

uint8           = (* 1 byte, unsigned *) ;
int8            = (* 1 byte, signed *) ;
uint16          = (* 2 bytes, native endian *) ;
uint16_be       = (* 2 bytes, big-endian *) ;
uint16_le       = (* 2 bytes, little-endian *) ;
int16           = (* 2 bytes, signed, endianness per token *) ;
int32           = (* 4 bytes, signed, endianness per token *) ;
uint32          = (* 4 bytes, unsigned, endianness per token *) ;
int64           = (* 8 bytes, signed, endianness per token *) ;
uint64          = (* 8 bytes, unsigned, endianness per token *) ;
float32         = (* 4 bytes, IEEE 754 binary32, endianness per token *) ;
float64         = (* 8 bytes, IEEE 754 binary64, endianness per token *) ;
index           = uint8 | uint16 ;
                  (* 8-bit or 16-bit per token variant *)
bytes           = (* N raw bytes, where N is the preceding length *) ;

(* ================================================================ *)
(* Packed array element encoding (internal representation)          *)
(* ================================================================ *)

packed_element  = packed_header { payload_byte } ;

packed_header   = (* 1 byte: X(1) | SS(2) | TTTTT(5) *)
                  (* X: executable attribute                        *)
                  (* SS: size-1 (0-3 extra bytes) or sub-selector   *)
                  (* TTTTT: PackedType (0-31)                       *) ;

(* CommonOp: TTTTT=00000, X|SS selects operator 0-7, no payload *)
(* Simple:   TTTTT=01010, SS selects Null/Mark/False/True, no payload *)
(* Others:   SS = number of extra bytes (0-3), or type-specific *)
```

**Notes on the grammar:**

1. Token dispatch priority: binary token bytes (0x80-0xFF) are recognized
   first.  Among text bytes, whitespace and comments are consumed before
   attempting token recognition.  Regular characters trigger
   `numeric_or_name` (tried as number first, name on failure).  Delimiters
   dispatch to their specific sub-scanners.

2. The `terminator` requirement on numeric and name tokens means that `42abc`
   is a single name token (not `42` followed by `abc`), because `a` is a
   regular character and does not terminate the token.

3. Underscore placement rules are enforced during number parsing, not by the
   grammar.  A token like `1__0` matches `regular_token` but fails number
   parsing and becomes a name.

4. The `locals_preamble` is recognized only when `|` is the first
   non-whitespace character after `{`.  A `|` appearing later in the
   procedure body is a regular character (bitwise-OR operator).

5. String escape sequences are greedy: `\0` followed by an octal digit
   continues accumulating (up to 3 octal digits total).  `\x` requires at
   least one hex digit; `\u` requires exactly 4; `\U` requires exactly 8.

6. The `\u`/`\U` grammar accepts hex digits case-insensitively.  Semantic
   validation (surrogate rejection, range check) occurs after parsing.
