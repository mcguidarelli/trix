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

# Trix Scanner: A Modern Lexical Engine for a Stack-Based Language

## 1. Introduction

A scanner (lexer, tokenizer) is the first stage of any language implementation.
It reads raw bytes and produces typed tokens.  Most scanners are invisible
infrastructure -- correct, boring, unremarkable.

Trix's scanner is a dual-mode lexical engine that processes both human-readable source text
and compact binary bytecode through the same interface.  It offers a suffix
grammar system that gives the programmer control over type width, access mode,
mutability, and storage location -- at the point of construction, not after the
fact.  It supports radix 2-36 numeric literals, three string forms (text, hex,
raw), Unicode UTF-8 escapes, scan-time string interpolation, nested block
comments, underscore separators, local variable bindings, dictionary path
expressions, and a 128-opcode binary token encoding that can represent any
Trix value in 1-9 bytes.

This document explains what the scanner does, how each feature works, why each
design choice was made, and how the pieces fit together.

### 1.1 How the Scanner Fits In

```
Source bytes --> Scanner --> (Lexeme, Object) pairs --> Interpreter
                  |
                  +-- text mode: human-readable tokens
                  +-- binary mode: compact bytecode tokens
```

The scanner's output is a stream of `(Lexeme, Object)` pairs.  The `Lexeme`
tells the interpreter what to do with the object:

| Lexeme            | Meaning                                                |
| ----------------- | ------------------------------------------------------ |
| `LiteralValue`    | Push object onto the operand stack                     |
| `ExecutableValue` | Execute: look up names, call operators, run procedures |
| `EndOfProcedure`  | `}` encountered -- signals end of procedure body       |
| `EndOfStream`     | Input exhausted normally                               |
| `SyntaxError`     | Invalid input -- Object carries the error message      |

Every value in Trix is an 8-byte `Object`.  The scanner produces fully typed,
ready-to-use objects directly -- there is no intermediate AST, no parse tree,
no second pass.  This single-pass design is fundamental to Trix's low overhead.

---

## 2. Character Classification

Before any token is recognized, each input byte is classified using a 16-bit
attribute bitmask.  This drives the entire scanner with single-branch dispatch:

```
Bit  Flag  Meaning                           Characters
---  ----  --------------------------------  -------------------------
 0   WS    Whitespace                        HT LF VT FF CR SP
 1   CT    Comment terminator                LF FF CR
 2   DL    Delimiter                         % ( ) / \ . < > [ ] { }
 3   RG    Regular character                 ! " # $ & ' * + , - 0-9
                                             : ; = ? @ A-Z ^ _ ` a-z | ~
 4   LC    Lowercase letter                  a-z
 5   UC    Uppercase letter                  A-Z
 6   DD    Decimal digit                     0-9
 7   OD    Octal digit                       0-7
 8   HD    Hex digit                         0-9 A-F a-f
 9   IN    Integer start                     + - 0-9
10   FE    Float fraction/exponent           . E e
11   IS    Integral suffix                   A B I L Q U a b i l q u
12   FS    Float suffix                      D E R d e r
13   SE    Syntax error                      NUL-US (except WS) DEL
14   US    Underscore                        _
15   SS    String-stop                       ( ) \ CR
```

Binary-token bytes (0x80-0xFF) carry **no** attribute bit -- their `chattr`
entry is `0x0000`.  They are recognized by value (`ch >= 0x80`) and dispatched
to `scan_binary_token` (see Section 8), not by a flag test.

The attribute table is a compile-time `constexpr` array of 257 entries (one for
EOF at index -1, then 256 byte values).  Every bitmask classification predicate
-- `is_whitespace`, `is_regular`, `is_hexdigit` -- is a single bitmask AND
operation (`is_binary_token` is the exception noted above: a `ch >= 0x80` test).

**Why this matters:** A hand-written `if/else` chain for character classification
would require 5-10 comparisons per byte.  The bitmask approach makes every
predicate O(1) with a single memory access and bitwise AND.  In the
`scan_regular_chars` inner loop -- the hottest path in the scanner -- this
means one table lookup per byte to classify, copy, and advance.

---

## 3. Numeric Literals

Trix supports eight integer types, two float widths, radix 2-36 notation, and
underscore separators -- all in a unified grammar.

### 3.1 Integer Literals

```
Format: [+|-] digits [suffix]

Suffix  Type       Width    Range
------  ---------  -------  ---------------------------
(none)  Integer    32-bit   -2,147,483,648 to 2,147,483,647
i       Integer    32-bit   (explicit, same as none)
b       Byte        8-bit   0 to 255
u       UInteger   32-bit   0 to 4,294,967,295
l       Long       64-bit   -2^63 to 2^63-1
ul      ULong      64-bit   0 to 2^64-1
a       Address    64-bit   pointer-width raw address
q       Int128     128-bit  -2^127 to 2^127-1
uq      UInt128    128-bit  0 to 2^128-1
```

Examples:
```trix
42           % Integer (default)
42i          % Integer (explicit)
255b         % Byte
1000000u     % UInteger
9999999999l  % Long
100ul        % ULong
16384a       % Address
-42          % negative Integer
+42          % explicit positive Integer
```

**Why eight integer types?** Trix's 8-byte tagged value design stores small
types (Byte, Integer, UInteger, Real, Boolean) inline in the 4-byte value slot.
Larger types (Long, ULong, Double, Address, Int128, UInt128) are heap-stored
via an `ExtValue` in VM memory.  The suffix system lets the programmer choose
the right width for the task -- saving memory for small values while providing
full 64-bit (and 128-bit) range when needed.

### 3.2 Radix Notation (Base 2-36)

```
Format: base # digits [# [suffix]]
```

The base is a decimal number from 2 to 36.  Digits use 0-9 and A-Z
(case-insensitive).  The closing `#` is optional; it is only required when
adding a type suffix (e.g. `16#FF#b`).  Without a suffix, the literal ends at
the next whitespace, delimiter, or end of input: `16#FF` and `16#FF#` both
yield 255.

```trix
16#FF#         % 255 (hex)
16#ff#         % 255 (case-insensitive)
2#11111111#    % 255 (binary)
8#377#         % 255 (octal)
36#Z#          % 35 (base-36, Z is the highest single digit)
16#DEAD_BEEF#u % 3,735,928,559u (hex with underscore separator)
2#1010_0101#b  % 165b (binary byte with separator)
```

**C-style prefixes: `0x` / `0o` / `0b`.** For the three most common bases,
Trix also accepts the familiar C-style prefixes alongside the general
`N#digits#` form -- the two are interchangeable for bases 16, 8, and 2
(`0xFF` and `16#FF#` both yield 255):

```trix
0xFF           % 255 (hex)
0XFF           % 255 (prefix is case-insensitive: 0x/0X, 0o/0O, 0b/0B)
0o377          % 255 (octal)
0b1111_1111    % 255 (binary, underscore separators allowed)
0xDE_AD        % 57005 (hex with separator)
```

`N#digits#` remains the only way to write the other 32 bases (`36#Z#`,
`7#66#`, ...); the prefixes simply give the common three the spelling every
C, Rust, Python, and Go programmer already knows.

**Type suffixes need a `#` separator.**  The suffix letters (`b`, `i`, `u`,
`l`, `a`, ...) are themselves valid hex digits, so a prefixed literal takes its
type suffix after a `#`:

```trix
0xFF#b         % Byte 255
0o377#ul       % ULong 255
0xDEAD_BEEF#u  % 3,735,928,559u
0xFFb          % 4091 -- no '#', so 'b' is the hex digit B (value 0xFFB)
```

Without a suffix, a prefixed literal infers the narrowest signed type that
fits, exactly like a decimal literal -- so `0xDEADBEEF` alone overflows
`Integer`; add `#u` for the unsigned reading.

**Bare `0b` is Byte 0**, not an empty binary prefix: the token is the digit
`0` with the Byte suffix `b`.  Write `0b0` for binary zero and `0b1010` for
binary ten.

**Hex floats (C99).**  `0x` also introduces hexadecimal floating-point; the
`p` binary exponent is mandatory:

```trix
0x1.8p1        % 3.0  (Real: 1.5 x 2^1)
0x1.fp-3#d     % 0.2421875 (Double)
```

### 3.3 Floating-Point Literals

```
Format: [+|-] digits [. [digits]] [Ee [+|-] digits] [suffix]
    or: [+|-] . digits [Ee [+|-] digits] [suffix]

Suffix  Type    Width   Range
------  ------  ------  --------------------------
(none)  Real    32-bit  IEEE 754 binary32 (float)
r       Real    32-bit  (explicit, same as none)
d       Double  64-bit  IEEE 754 binary64 (double)
```

Examples:
```trix
3.14         % Real (32-bit float)
3.14r        % Real (explicit)
3.14d        % Double (64-bit)
.5           % Real, leading dot
1e10         % Real, scientific notation
1.5e-3d      % Double, scientific with negative exponent
```

**Special float keywords** (defined in `systemdict`, not scanner keywords):
```trix
inf   % Real +infinity
nan   % Real quiet NaN
inf#r % Real +infinity (explicit-suffix alias of inf)
nan#r % Real quiet NaN (explicit-suffix alias of nan)
inf#d % Double +infinity
nan#d % Double quiet NaN
```

These are ordinary names that can be shadowed by user dicts, not reserved
words.  This keeps the scanner simple while providing convenient access to
IEEE 754 special values.

### 3.4 Underscore Separators

Underscores may appear between digits in any numeric form to improve
readability.  They are stripped before parsing and do not affect the value.

```trix
1_000_000          % 1000000 (Integer)
16#DEAD_BEEF#ul    % hex with separators
1_000.123_456d     % Double with separators
2#1111_0000#b      % binary byte with separators
```

**Placement rules:** An underscore is valid only between two digits (decimal
digits in base-10 forms, hex-valid characters in the radix-digits section of
`N#digits#` forms).  Leading, trailing, or doubled underscores cause the token
to be treated as a name, not a number -- no error, just different
classification.

### 3.5 Overflow Detection

Every numeric type has overflow detection.  An integer literal that exceeds its
type's range raises `NumericalOverflow` at scan time:

```trix
256b                % NumericalOverflow: exceeds Byte range 0-255
2147483648          % NumericalOverflow: exceeds Integer max
-1u                 % parsed as executable name (not a valid unsigned literal)
```

---

## 4. String Literals

Trix provides three string forms, each designed for a specific use case.  All
produce the same type (`String` -- a byte sequence in VM heap memory with a
hidden NUL terminator for C interop).

### 4.1 Text Strings: `( ... )`

The primary string form.  Parentheses nest, backslash escapes are processed,
and bare line endings are normalized to LF.

```trix
(Hello, world!)        % simple string
(nested (parens) ok)   % balanced parens included literally
(line 1\nline 2)       % \n is LF (0x0A)
(tab\there)            % \t is HT (0x09)
(caf\u00E9)            % \u escape: e-acute in UTF-8
```

**Full escape sequence table:**

| Escape                | Value           | Meaning                                                         |
| --------------------- | --------------- | --------------------------------------------------------------- |
| `\a`                  | 0x07            | Bell                                                            |
| `\b`                  | 0x08            | Backspace                                                       |
| `\e`                  | 0x1B            | Escape                                                          |
| `\f`                  | 0x0C            | Form Feed                                                       |
| `\n`                  | 0x0A            | Line Feed                                                       |
| `\r`                  | 0x0D            | Carriage Return                                                 |
| `\t`                  | 0x09            | Horizontal Tab                                                  |
| `\v`                  | 0x0B            | Vertical Tab                                                    |
| `\(`                  | `(`             | Literal open-paren                                              |
| `\)`                  | `)`             | Literal close-paren                                             |
| `\\`                  | `\`             | Literal backslash                                               |
| `\^X`                 | 0x00-0x1F       | Control char (`X` in `@`-`_`; `\^?` is DEL)                     |
| `\0`-`\377`           | 0-255           | Octal: 1-3 digits                                               |
| `\xhh`                | 0-255           | Hex: 1-2 digits                                                 |
| `\uXXXX`              | U+0000-U+FFFF   | Unicode BMP: exactly 4 hex digits, emits 1-3 UTF-8 bytes        |
| `\UXXXXXXXX`          | U+0000-U+10FFFF | Unicode full range: exactly 8 hex digits, emits 1-4 UTF-8 bytes |
| `\{name}`             | *(varies)*      | Interpolation: scan-time name lookup, value spliced as text     |
| `\CR`, `\LF`, `\CRLF` | *(nothing)*     | Line continuation                                               |

**Unicode escape details:** `\u` and `\U` encode a Unicode codepoint as UTF-8
bytes directly into the string.  Hex digits are case-insensitive.  Surrogate
codepoints (U+D800-U+DFFF) and values above U+10FFFF are rejected with
`SyntaxError`.  For ASCII codepoints (U+0000-U+007F), `\u` emits a single byte
identical to `\xhh`.

```trix
(\u00E9)                         % e-acute: 2 bytes (0xC3, 0xA9)
(\u4E16)                         % CJK 'world': 3 bytes (0xE4, 0xB8, 0x96)
(\U0001F600)                     % grinning face emoji: 4 bytes (0xF0, 0x9F, 0x98, 0x80)
(caf\u00E9)                      % mixed: ASCII + Unicode = 6 bytes
(\u0048\u0065\u006C\u006C\u006F) % "Hello" spelled as codepoints
```

**Caret notation:** `\^X` generates control characters using the same
convention as Unix terminals, `stty`, `cat -v`, and Emacs.  The letter
after `^` is the ASCII character 0x40 above the control code it represents
-- `\^A` is 0x41 - 0x40 = 0x01 (SOH), `\^G` is 0x47 - 0x40 = 0x07 (BEL),
and so on.  This mapping covers the full C0 control range (0x00-0x1F) plus
DEL (0x7F via the special case `\^?`).

While `\a`, `\n`, `\t`, and friends cover the most common control characters,
caret notation covers all 33 -- including characters like SOH (0x01), STX
(0x02), ETX (0x03), EOT (0x04), ENQ (0x05), and ACK (0x06) that have no
named escape.  This is particularly useful for serial protocols, terminal
control sequences, and binary data formats where specific C0 characters have
protocol-defined meanings.

```trix
(\^@)       % NUL (0x00) -- string terminator in C
(\^A)       % SOH (0x01) -- start of heading
(\^C)       % ETX (0x03) -- interrupt signal in terminals
(\^D)       % EOT (0x04) -- end of transmission
(\^G)       % BEL (0x07) -- terminal bell (same as \a)
(\^M)       % CR  (0x0D) -- carriage return (same as \r)
(\^Z)       % SUB (0x1A) -- EOF marker on DOS/Windows
(\^[)       % ESC (0x1B) -- start of ANSI escape sequence (same as \e)
(\^?)       % DEL (0x7F) -- delete (special case: ? is 0x3F)
```

**Scan-time string interpolation:** `\{name}` looks up a name through the
dict stack at scan time (using the same mechanism as `//name` immediate
lookup) and splices the value's text representation directly into the string
being built.  Dictionary path syntax is supported.

```trix
/greeting (Hello) def
/n 42 def
(Say \{greeting}, number \{n}!)       % "Say Hello, number 42!"
(\{:systemdict:numbers:real-type:pi}) % "3.1415927" (dict path)
```

Because interpolation resolves at scan time, the value is baked into the
string when the `(...)` literal is scanned -- not when it is later used.
Inside a procedure, the value is captured when the procedure is scanned:

```trix
/x 42 def
/f { (\{x}) = } def    % "42" baked in here
/x 99 def
f                       % prints "42", not "99"
```

Any value type can be interpolated.  The spliced text is what the `=`
operator would print -- i.e., the value's default `PrintFmt` form:

- Numerics (Integer, UInteger, Long, ULong, Int128, UInt128, Real,
  Double, Address) splice as their numeric form (`42`, `3.14`).
- Byte splices as a character (default `%c`): `65b` -> `A`.
- Boolean splices as `true` or `false`.
- String splices as raw content (no surrounding parentheses).
- Name splices as raw text (no `/` prefix).
- Operator splices as its operator name (`add`, `mul`, ...).
- Compound types (Array, Dict, Packed, Set, Stream, Tagged, Record,
  Curry, Thunk, etc.) splice as their type-name string (`array`,
  `dict`, `stream`, ...) -- the default `%s` representation for the
  type, not a structural dump.  Use a dedicated formatter operator
  (`sprint-fmt`, `%O`) if structural output is needed at runtime.

Empty interpolation (`\{}`) raises `SyntaxError`; undefined names raise
`Undefined`.  A buffer overflow during interpolation is reported against
the enclosing string -- the scanner sets an `interp_overflow` flag and
raises `LimitCheck` or `VMFull` when the string scan completes.

The implementation writes directly into the string build buffer with zero
VM allocation -- numeric values use `std::to_chars`, strings and names
copy their existing bytes.  Literal braces need no escaping: `({hello})`
produces `{hello}` because only `\{` triggers interpolation.

### 4.2 Hex Strings: `< ... >`

Pairs of hex digits encode raw bytes.  Whitespace inside the delimiters is
ignored.  Useful for binary data that must be human-readable.

```trix
<48656C6C6F>     % "Hello" (5 bytes)
<48 65 6C 6C 6F> % same, with spacing for readability
<FF 00 80>       % three bytes: 255, 0, 128
<>               % empty string (0 bytes)
```

An odd number of hex digits raises `SyntaxError` -- every byte needs two
digits.  Non-hex, non-whitespace characters also raise `SyntaxError`.

**When to use hex strings:** Binary protocol data, cryptographic hashes,
test fixtures with exact byte sequences.  Anywhere you would write
`bytes.fromhex()` in Python.

### 4.3 Raw Strings: `<( ... )>`

No escape processing.  Backslash is a literal byte.  Parentheses still nest
(for the same reason they nest in text strings -- so you can include balanced
parens without escaping).

```trix
<(\d+\.\d+)>          % regex: \d+\.\d+ (8 bytes, no escaping needed)
<(C:\Users\name)>     % Windows path (13 bytes, backslashes literal)
<(line1\nline2)>      % literal \n (12 bytes, not a newline)
```

**Comparison with other languages' raw strings:**

| Language   | Syntax           | Can contain unescaped `\`? | Can end with `\`? | Nesting?       |
| ---------- | ---------------- | -------------------------- | ----------------- | -------------- |
| Trix       | `<(...)>`        | Yes                        | Yes               | Balanced `()`  |
| Python     | `r"..."`         | Yes                        | No (!)            | No             |
| Rust       | `r#"..."#`       | Yes                        | Yes               | Via `##` count |
| C++        | `R"del(...)del"` | Yes                        | Yes               | Via delimiter  |
| JavaScript | (none)           | --                         | --                | --             |

Trix's raw strings handle the common case cleanly.  The only limitation is
content with unbalanced parentheses -- rare in practice, and addressable with
hex strings for edge cases.

### 4.4 String Suffixes

All three string forms accept an optional suffix after the closing delimiter:

```
Grammar: #[a[r|w] | b | [=|$|$$][l|x][r|w]]

#a    Byte array: convert string bytes into Array of Byte objects
#ar   Byte array, read-only
#aw   Byte array, read-write (explicit default)
#b    Byte extraction: single-character string -> Byte value
#=    Temporary storage: use pre-allocated scratch buffer
#l    Literal attribute (default)
#x    Executable attribute: string becomes a program when executed
#r    ReadOnly access mode
#w    ReadWrite access mode (default)
#$    Force allocation in global VM (survives save/restore; reclaimed by GC)
#$$   Force allocation in local VM (overrides enclosing ${...})
```

`#=`, `#$`, and `#$$` are mutually exclusive (different storage classes);
any pair raises `/syntax-error`.  `#$` and `#$$` are also accepted on
`[arr]`, `<<dict>>`, `{{set}}`, and `{proc}` literals -- they route the
**container's backing store** to the chosen region.  See § 6.10 below
for the companion scope-block syntax `${...}` / `$${...}` and direct-
flag ops, and [`gvm-heap-gc.md`](gvm-heap-gc.md) for the global VM
design.

Examples:
<!-- doctest: skip (suffix reference; `(dup mul)#x` auto-executes against residue) -->
```trix
(Hello)#a          % Byte array: [ 72b 101b 108b 108b 111b ]
(Hello)#ar         % read-only Byte array
(A)#b              % Byte 65 (extracted from single-char string)
(\n)#b             % Byte 10 (LF as a byte value)
(Hello)#r          % read-only string (cannot be modified via put)
(dup mul)#x        % executable string: runs "dup mul" when executed
(temp data)#=      % stored in pre-allocated scratch buffer, not VM heap
(frozen)#=r        % temporary + read-only
```

The `#a` suffix is scanner-level: no intermediate string is allocated on
the VM heap.  The resulting array participates in save/restore journaling
(unlike strings, where `put` writes are not journaled).

**Why string suffixes?** In most languages, immutability is an afterthought --
you create a mutable string, then freeze it (Ruby) or hope nobody mutates it
(Python's implicit immutability).  In Trix, access control is declared at
construction:

```trix
(mutable)            % ReadWrite -- can be modified via put
(immutable)#r        % ReadOnly -- put raises InvalidAccess

% This is enforced by the VM, not by convention:
(frozen)#r 0 65b put  % ERROR: InvalidAccess
```

The `#b` suffix is particularly elegant -- it provides character literal
syntax without a dedicated character type:
```trix
(A)#b     % Byte 65 -- like Rust's b'A' or C's 'A'
(\n)#b    % Byte 10 -- newline as a byte
(\0)#b    % Byte 0  -- NUL as a byte
```

### 4.5 String Limits

- Maximum string length: 65,535 bytes (`MaxStringLength`)
- Strings carry a hidden NUL terminator (not counted in `length`) for C interop
- Exceeding the limit raises `LimitCheck`; exhausting VM heap raises `VMFull`

---

## 5. Name Tokens

Names are Trix's identifiers.  They are interned, immutable, and hashed for
O(1) dict lookup.  The scanner produces four forms:

### 5.1 Literal Names: `/name`

Creates a Name object pushed as a literal value.  Used for defining dict
keys and referring to names as data.

```trix
/x 42 def    % define 'x' as 42
/my-function % push the name 'my-function' as a value
/            % bare slash: the name '/' (a valid name)
```

### 5.2 Executable Names: `\name`

Creates a Name object with the executable attribute.  When the interpreter
encounters an executable name, it looks it up in the dict stack and
executes the result.

```trix
\add               % look up 'add' and execute it (same as bare 'add')
```

In practice, bare names (without any prefix) are parsed as executable names,
so `\name` is rarely used directly.  Its primary role is in contexts where you
need an explicitly executable name object.

### 5.3 Immediate Lookup: `//name` and `\\name`

Double-slash (or double-backslash) triggers immediate lookup at scan time --
the name is resolved in the dict stack when the scanner encounters it,
not when the interpreter executes it.

```trix
//pi                   % resolved to 3.14159... at scan time
//add                  % resolved to the 'add' operator object at scan time

{ //pi mul }           % the value of pi is captured when the proc is compiled
                       % -- not looked up each time the proc runs
```

`//name` pushes the result as a literal value.  `\\name` preserves the found
object's execute attribute.  If the name is not found, `Undefined` is raised
immediately at scan time.

### 5.4 Dictionary Path Expressions

A structured name form that navigates a specific dict chain:

```
Format: //:rootdict:segment:...:leaf
    or: \\:rootdict:segment:...:leaf
```

Root dicts: `systemdict`, `userdict`, `errordict`, `handlersdict`, `modules`,
or any dict-valued entry of `systemdict` (e.g. `numbers`, `pipeline`,
`records`, `protocoldict`).  The special root `:status:key` performs on-demand
VM introspection without traversing a dict, so the `//:status:vm-used` example
below is consistent with these rules.

```trix
//:systemdict:numbers:real-type:pi % pi via nested dicts (3.14159...)
//:systemdict:numbers:real-type:e  % e, alongside pi
//:errordict:command               % the command entry (an operator)
//:status:vm-used                  % on-demand VM introspection (no dict traversal)
```

**Why path expressions?** They provide unambiguous name resolution.  In a
stack-based language with a dynamic dict stack, the same name can resolve
to different values depending on which dicts are active.  Path
expressions bypass the stack entirely, reaching directly into a known root.

### 5.5 Name Limits

- Maximum name length: 127 characters (`MaxNameLength`)
- Names created by the scanner consist of regular characters only (printable
  ASCII 0x21-0x7E excluding delimiters).  The `to-name` operator can create
  names from arbitrary strings, including those containing whitespace,
  delimiters, or non-printable characters -- such names cannot be expressed
  as literal tokens but are valid for programmatic dict operations.
- Names are interned: each unique string is stored once in VM memory with a
  cached wyhash value
- The 127-character limit is generous for a stack-based language where idiom
  favors short names (`add`, `dup`, `forall`).  Real-world identifiers in
  Python rarely exceed 50 characters.  The physical ceiling is 65,535
  (`uint16_t`); 127 is a deliberate policy choice, trivially adjustable.

---

## 6. Composite Literals

Arrays, dicts, sets, and procedures are all constructed with mark-based
syntax and share a unified suffix grammar.

### 6.1 Arrays: `[ ... ]`

`[` pushes a mark; `]` collects everything above it into a new Array.

```trix
[1 2 3]                % Array of three integers
[1 (hello) true 3.14]  % mixed types
[[1 2] [3 4]]          % nested arrays
[]                     % empty array (length 0, fixed-size)
```

**Array suffixes:**
<!-- doctest: skip (grammar/EBNF illustration line, not runnable Trix) -->
```
Grammar: #[=|$|$$][r|w]

[1 2 3]#r         % read-only array (put raises /read-only)
[1 2 3]#=         % temporary storage (uses pre-allocated scratch buffer)
[1 2 3]#=r        % temporary + read-only
```

### 6.2 Dicts: `<< ... >>`

`<<` pushes a mark; `>>` collects key-value pairs into a new Dict.

```trix
<< /x 1 /y 2 >>               % dict with two entries
<< /name (Trix) /version 1 >> % string value
<< >>                         % empty dict (minimum capacity 4)
```

**Dict suffixes:**
<!-- doctest: skip (grammar/EBNF illustration line, not runnable Trix) -->
```
Grammar: #[=|$|$$][r|w]

<< /x 1 >>#r     % ReadOnly: def/put raises /read-only
<< /x 1 >>#=     % temporary storage
```

Dicts without a suffix default to `ReadWriteFixed` -- existing keys can
be updated, but new keys cannot be added.  For growable dicts, use the
`dynamic-dict` operator.

### 6.3 Sets: `{{ ... }}`

`{{` pushes a mark; `}}` collects all elements into a new Set (a Dict with
keys only, no values).

```trix
{{ 1 2 3 }}              % integer set
{{ /a /b /c }}           % name set
{{ 1 (hello) true }}     % mixed types
{{ }}                    % empty set
```

**Set suffixes:**
<!-- doctest: skip (grammar/EBNF illustration line, not runnable Trix) -->
```
Grammar: #[=|$|$$][r|w]

{{ 1 2 3 }}#r     % ReadOnly set
{{ 1 2 3 }}#=     % temporary storage
```

Sets default to `ReadWriteFixed`.  Elements must be hashable (same
constraint as dict keys).

### 6.4 Procedures: `{ ... }`

`{` begins a procedure body; `}` ends it.  The scanner recursively collects all
tokens into a Packed array (default) or regular Array.

```trix
{ dup mul }        % simple procedure: square a number
{ |x| x x mul }    % with local variable binding
{ [1 2 3] forall } % inline array inside procedure
{ << /a 1 >> }     % inline dict inside procedure
```

**Procedure suffixes -- the most expressive in the language:**
```
Grammar: #[=|$|$$][a|p][e|l][r|w]

=     Temporary storage (pre-allocated scratch buffer)
$/$$  Force allocation in global / local VM region
a/p   Array / Packed representation (default: p)
e/l   Early binding / Late binding (default: l)
r/w   ReadOnly / ReadWrite (default: w; Packed is always ReadOnly)
```

Examples:
```trix
{ dup mul }           % Packed, late-bound (default)
{ dup mul }#a         % Array, late-bound
{ dup mul }#e         % Packed, early-bound (operators resolved at compile time)
{ dup mul }#ae        % Array, early-bound
{ dup mul }#ar        % Array, read-only
{ dup mul }#=         % Packed, temporary storage
```

**`#a` is ignored for procedures with a locals preamble.**  A `|...|` or
`||` preamble forces packed form: the locals transform rewrites the body
into a *nested packed proc* (so the frame setup can replay it), which means
the array form would convert only the tiny outer wrapper while the body
stays packed -- no benefit.  Writing `#a` on such a procedure is therefore a
no-op for the body, and the scanner emits a warning to stderr
(`'#a' (array form) ignored on a procedure with a |locals| preamble`).  Use
`#a` only on procedures with no preamble.

**Early vs. late binding:**

Late binding (default) resolves executable names at runtime by searching the
dict stack.  Early binding (`#e`) resolves names at compile time --
replacing them with operator objects when the procedure is scanned.

```trix
% Late binding: 'add' looked up every time the proc runs
/late-add  { 1 2 add } def

% Early binding: 'add' resolved to the operator object at compile time.
% #e captures whatever the name means AT SCAN TIME, so define the early-bound
% proc BEFORE installing any shadow:
/early-add { 1 2 add }#e def

% Now shadow 'add' in the dictionary:
/add { (intercepted!) = } override

early-add =    % => 3          (early-bound: uses the captured operator)
late-add       % => intercepted!  (late-bound: hits the shadow)
```

Early binding is Trix's equivalent of Forth's `IMMEDIATE` or PostScript's
`bind` -- but controlled at the syntax level, not as a separate operation.

**Why early-bind.**  Late binding searches the dictionary stack on every call;
`#e` pays that lookup once, at scan time.  On a hot path this is the single
largest proc-level speedup available -- bigger than `#a` (Section 9.5) and,
unlike `#a`, with no memory cost, because the body stays packed and an operator
reference encodes *smaller* than the name it replaces.  Early-binding the 44 hot
procs of the bundled Z-machine interpreter cut 10.7% off a real-game
walkthrough's wall-clock (full numbers in `interpreter.md` Section 8.2).

`#e` binds **recursively**: it reaches through a `|locals|` preamble's nested
packed body -- the one representation `#a` cannot touch (see the `#a`-on-locals
note above) -- so it is the lever to use on procedures that declare locals.

**Safety.**  `#e` is correct only when each name means the same thing at scan
time as at run time.  A body that relies on an operator being *redefined* later,
or a frame variable whose name shadows an operator (the local does not exist at
scan time, so `#e` freezes the reference to the operator, not the future local),
will misbehave when early-bound.  The opt-in lint
`tests/check_operator_shadows.py` reports operator-named locals so you can avoid
`#e` on those procedures, or rename the local first.

### 6.5 Local Variable Bindings

Procedures can declare local variables in a preamble:

```
Format: { |name1 name2 ...| body }
```

Arguments are consumed from the operand stack (rightmost name bound first) and
made available as entries in a **frame dict** (the per-call dict created by
the `|params|#N` preamble; see the Glossary in `trix-reference.md`).

```trix
{ |x| x x mul } 5 exch exec       % 25 (x = 5)
{ |a b| a b add } 3 4 rot exec    % 7  (a = 3, b = 4)

% Named parameters make stack manipulation explicit:
/distance { |x1 y1 x2 y2|
    x2 x1 sub dup mul
    y2 y1 sub dup mul
    add sqrt
} def
0.0 0.0 3.0 4.0 distance    % 5.0
```

The compiler translates `{ |x y| body }` into a `begin-locals` / `@end-locals`
operator pair that pushes and pops the frame dict on the dict stack.
This is cleaner than Forth's `LOCALS|` syntax and provides true lexical
scoping within the procedure.

**Capacity suffix `|names|#N`:** by default the frame dict is sized exactly to
the declared name count K, leaving no room for working variables defined by
`/tmp ... def` inside the body.  Append `#N` immediately after the closing `|`
(no intervening whitespace) to request a frame dict of capacity `N >= K`,
giving the body `N - K` free slots:

```trix
% Without #N: frame dict has no room for /q
/hypotenuse { |x y|#4
    /q x x mul  y y mul add def    % 'def' into the frame dict
    q sqrt
} def
3.0 4.0 hypotenuse     % 5.0
```

`N` is a decimal integer in the range `K..65535`.  Scan-time errors:
- `N < K` (`SyntaxError`): capacity smaller than declared names.
- `N > 65535` (`LimitCheck`): exceeds `length_t` (uint16) dict-capacity.
- `#` not followed by a digit (`SyntaxError`).
Whitespace between `|` and `#` disables the suffix (the `#...` becomes body
content).

### 6.6 Suffix Grammar Unification

Strings, arrays, dicts, sets, and procedures all share the `#` suffix delimiter
and the `=`/`r`/`w` flags.  This is a deliberate design consistency:

| Type      | Suffix grammar                       | Example                |
| --------- | ------------------------------------ | ---------------------- |
| String    | `#[a[r\|w] \| b \| [=][l\|x][r\|w]]` | `(text)#r`, `(data)#a` |
| Array     | `#[=][r\|w]`                         | `[1 2 3]#r`            |
| Dict      | `#[=][r\|w]`                         | `<< /k v >>#r`         |
| Set       | `#[=][r\|w]`                         | `{{ 1 2 3 }}#r`        |
| Procedure | `#[=][a\|p][e\|l][r\|w]`             | `{ code }#ae`          |

The `=` flag (temporary storage) and `r`/`w` flags (access mode) mean the same
thing everywhere.  Learn the pattern once, apply it to every composite type.

### 6.7 Infix Expressions: `$( )` and `$[ ]`

`$( expr )` and `$[ expr ]` provide infix math expression syntax.  The scanner
parses the expression using a Pratt parser (precedence climbing) and emits
equivalent postfix tokens.  This is purely a scanner-level desugar -- zero
runtime cost, no new types or operators.

```trix
$( a * x * x + b * x + c ) % emits: a x x mul mul b x mul add c add
$[ 3 + 4.0 ]               % emits: 3 4.0 promote add (auto-promote)
$( sqrt(16.0) + 1.0 )      % emits: 16.0 sqrt 1.0 add (function call)
```

`$( )` preserves strict type semantics (no implicit coercion).  `$[ ]` inserts
`promote` before each binary operator, allowing mixed-type arithmetic.

**How it works:**  When the scanner encounters `$` (a regular character) followed
by `(`, it enters strict infix mode.  `$` followed by `[` enters auto-promote
mode (the `$` peek-dispatches before `[` could be read as an array-literal
opener; `[` and `]` are not infix operators, so they serve as unambiguous
delimiters).  The `$` is not a delimiter -- it remains a regular character for
backward compatibility.  The Pratt parser consumes the entire expression up to
the matching close (`)` or `]`), emits the postfix token sequence into a
per-stream VM buffer, and returns them one at a time through subsequent
`scanner()` calls.

**10-level precedence table** (highest to lowest binding):

| Prec | Operators         | Assoc | Emission                   |
| ---- | ----------------- | ----- | -------------------------- |
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

`&&` and `||` are synonyms for `&` and `|` (same precedence, same emission).

**Ternary**: `cond ? true_expr : false_expr` evaluates both branches and
selects based on the condition using the `select` operator (non-short-circuit).
Right-associative: `a ? b : c ? d : e` = `a ? b : (c ? d : e)`.

**Unary operators**: `-` (neg), `+` (no-op), `!` (not), `~` (not).  `!` and `~`
are synonyms.  Unary operators bind looser than `**` (so a negated power
negates the whole power, matching Python / Ruby).

**Function calls**: `name(arg1, arg2, ...)` pushes arguments left-to-right, then
emits the function name.  Nullary calls use empty parens: `clock()`.  Hyphenated
names are supported: `kahan-sum(data)`.

**Number literals**: all Trix number forms work inside `$( )` -- type suffixes,
radix prefixes (`16#FF#`, `0xFF`, `0o77`, `0b1010`), underscore separators,
scientific notation (`1.5e-3`), and hex floats (`0x1.0p+10`).

**Token buffer**: the parser emits postfix tokens into the `m_infix_scratch`
array, then packs them into a per-stream VM buffer (640 bytes of packed data,
allocated lazily on first use).  The packed representation uses 2-5 bytes per
token instead of 8 bytes per Object.  The `m_infix_offset` field is journaled
through the save/restore mechanism to prevent stale buffer reuse after VM
rollback.

**Parentheses** inside `$( )` are **grouping**, not string literals.  `$( )` cannot
nest inside itself.

### 6.8 Field-Access Sugar: `.name`

The `.` character is classified as a **delimiter** (same category as `[`, `(`,
`/`, `{`, etc.), not a regular name character.  The scanner rewrites
`.name` into the two-token sequence `/name get`:

<!-- doctest: skip (field-access desugar illustration; placeholder receiver/a) -->
```trix
receiver.field             % -> receiver /field get
a.b.c.d                    % -> a /b get /c get /d get
```

This is a scanner-level desugar -- zero runtime cost, no new types or
operators.  The `get` operator is polymorphic, so the same sugar works on
records (primary motivation), dicts with name keys, arrays with numeric
keys, and any other collection `get` understands.

**How it works**:  When the scanner dispatches to `scan_delimiter` with
`.`, it peeks the next character:

- **digit** -> delegates to `scan_number_or_name` with `.` as the already-
  consumed first character of a number literal (preserves `.5`, `.002`,
  `-.1`, etc.).
- **regular** (name-start) -> calls `scan_field_access`, which accumulates
  the field name via `scan_regular_chars`, emits `/name` (literal) and
  `get` (well-known executable name), then loops while the next character
  is another `.` to handle chains.
- **anything else** (EOF, delimiter, syntax-error byte) -> `SyntaxError`.

The emitted token pairs are packed into the same per-stream VM buffer that
`scan_infix_expression` uses (`m_infix_offset`).  The first `/name` is
returned directly from `scan_field_access`; subsequent tokens drain via
`scanner()`'s priming path on following calls.

**Number-interior dots**: `scan_number_or_name` has a secondary loop that
consumes a single `.` into a digit-led or `.`-led token when the token-so-
far is number-shaped.  This keeps `3.14`, `1.5e10`, `0x1.Fp4`, and `3.`
(trailing dot = real 3.0) working.  The loop accepts `.` if the next
character is regular, whitespace, delimiter, or EOF -- i.e., any legitimate
token-continuation or token-terminator byte.  A digit-led token with a
dot and non-number content (e.g. `"3.foo"`) falls through to `Name::make`
and becomes a name; to apply field-access sugar to a numeric value, push
the number first and access its field via a later `.name` token.

**Character-attribute encoding**:  `.` has bits `DL | FE`:
- `DL` (delimiter) makes it terminate name scans and dispatch through
  `scan_delimiter` at token start.
- `FE` (frac-or-exp) keeps it valid inside the number scanner's state
  machine (`is_frac_or_exp`, `is_real`, `is_numeric`).

The bits are orthogonal: `scan_regular_chars` tests `is_regular` (no DL
bit, no FE bit) so `.` stops name accumulation; `Number::scan` tests
`is_numeric` (the LC | UC | IN | FE attribute bits, plus the radix marker `#`)
so `.` still flows through number parsing.

---

### 6.9 Global-VM Scope: `${...}`

Trix has two heaps (see [`gvm-heap-gc.md`](gvm-heap-gc.md)).  Ordinary
allocations land in the local VM and roll back at `restore`.  The
scanner exposes three surfaces for opting into the global VM, where
allocations survive `restore` and are reclaimed by garbage collection:

```
${ body }         % scope block: scans body with the global-alloc flag set
$/foo             % literal name interned in global VM
$\foo             % executable name interned in global VM
(str)#$           % global string (see § 4.4)
[arr]#$           % global array storage
<<dict>>#$        % global dict struct
{{set}}#$         % global set struct
{proc}#$          % global procedure storage

% Force-local inverses (useful inside ${...} when one element should stay local):
$${ body }        % scope block: force-local scan + runtime
$$/foo            % literal name force-interned in local VM
$$\foo            % executable name force-interned in local VM
(str)#$$          % force-local string
[arr]#$$          % force-local array storage
<<dict>>#$$       % force-local dict struct
{{set}}#$$        % force-local set struct
{proc}#$$         % force-local procedure storage
```

`${...}` propagates through its body -- every literal Name interned
during scan and every flag-honoring allocation during runtime lands
in global until the matching `}`.  The flag is **saved on entry and
restored on exit**, including on error unwind.  The flag is
**per-coroutine**, so a `${...}` block in one coroutine does not bleed
its policy into a sibling.

`$${...}` is the symmetric inverse: it force-saves the flag false on
entry, so any nested allocation lands in local VM regardless of the
enclosing scope.

`$/foo`, `$\foo`, and `<lit>#$` are per-form directives that don't
affect surrounding scan.  Pick the smallest scope that fits:

<!-- doctest: skip (persist-flag scoping illustration; placeholder global dict g) -->
```trix
% one-off: per-name prefix
g $/persist-key 42 put

% scope: many names + structures
${
    /populate {
        g /alpha 1 put
        g /beta  2 put
    } def
}
```

When `${...}` is not a fit -- e.g. the flag must span a procedure call
or be driven by a runtime Boolean -- the direct-flag ops are the
escape hatch:

```trix
true set-global             % bool --
\(allocations go global\)
false set-global

current-global              % -- bool : peek the flag
```

The runtime `int -- container` ops (`array`, `dict`, `dynamic-dict`,
`set`, `string`) consult the flag and route to global VM when set.
`${ 100 dict }` therefore produces a global dict, `${ 256 string }` a
global string buffer.

`#$` is **container-only**: `<</a 1>>#$` places the dict struct in
global but the literal name `/a` is interned wherever it would have
been (local by default).  For a fully-global dict, use the scope
block: `${ <</a 1>> }` -- both Name and struct land in global.

`#=`, `#$`, and `#$$` are mutually exclusive in the first slot; any
pair (e.g. `#=$`, `#$=`, `#$$=`) raises `/syntax-error`.

See [`trix-reference.md`](trix-reference.md) § Global VM Allocation
for the user-facing op-by-op reference and [`gvm-heap-gc.md`](gvm-heap-gc.md)
for the allocator and GC internals.

---

## 7. Comments

### 7.1 Line Comments

`%` begins a comment that runs to the end of the line (LF, CR, or FF).

```trix
42       % this is a comment
/x 1 def % define x
```

### 7.2 Nested Block Comments: `%{ ... %}`

Block comments can span multiple lines and nest to arbitrary depth.

```trix
%{
    This is a block comment.
    It can span multiple lines.

    %{
        And it nests correctly!
        This inner block is still inside the outer one.
    %}

    We're still in the outer comment here.
%}
```

**Why nesting matters:** Consider commenting out code that already contains
comments:

```trix
%{
    % This is a line comment inside a block comment.
    % Commenting out this section is trivial.
    /x 42 def
    %{ a nested block comment %}
    /y x dup mul def
%}
```

In C/C++ (`/* */`), JavaScript, and Java, block comments do not nest -- you
cannot reliably comment out code that contains block comments.  Python has no
block comment syntax at all.  Trix's nested block comments handle this
correctly at any depth.

### 7.3 Shebang: `#!`

If the first two bytes of a file stream are `#!` (at byte offset 0, no leading
whitespace), the entire line is consumed as a comment.  This enables
Unix-style self-executing scripts:

```bash
#!/usr/bin/env trix
(Hello from a Trix script!) =
```

The shebang is recognized only in file streams, not in string streams or the
REPL.

---

## 8. Binary Token Encoding

This is the scanner's most distinctive feature and the one that separates Trix
from every other scripting language in its class.

### 8.1 The Dual-Mode Scanner

The same `scanner()` function that processes human-readable text also processes
compact binary bytecode.  When the scanner encounters a byte in the range
0x80-0xFF, it dispatches to `scan_binary_token()` instead of the text token
path.  The output is identical: a `(Lexeme, Object)` pair, indistinguishable
from a text-scanned token.

This means:
- A Trix compiler can emit binary tokens directly
- Binary and text tokens can be mixed in the same stream
- The interpreter is completely unaware of the source format
- All token types (numbers, strings, names, operators) have binary encodings

### 8.2 The 128-Opcode Space

The binary token range 0x80-0xFF is divided into these groups:

```
Range       Count  Purpose
----------  -----  -------------------------------------------
0x80-0xA9    42    Numeric types with endianness variants
0xAA-0xAD     4    Boolean, Mark, Null
0xAE-0xC1    20    Strings, names, values (system)
0xC2-0xC5     4    Pre-encoded constants (single-byte tokens!)
0xC6          1    NumberArray
0xC7-0xED    39    Pre-encoded constants (single-byte tokens!)
0xEE-0xEF     2    Well-known names (1-byte index follows)
0xF0-0xF1     2    Int128, UInt128 (16-byte payload follows)
0xF2-0xFF    14    Reserved (raise Unsupported)
```

### 8.3 Numeric Tokens with Endianness Control

Every numeric type has native, big-endian, and little-endian variants:

```
Token  Type         Bytes  Endianness
-----  -----------  -----  ----------
0x80   Byte         1      (N/A)
0x81   Integer_8    1      (N/A)
0x82   Integer_16   2      native
0x83   Integer_16b  2      big-endian
0x84   Integer_16l  2      little-endian
0x85   Integer_32   4      native
0x86   Integer_32b  4      big-endian
0x87   Integer_32l  4      little-endian
...
0x96   Long_64      8      native
0x97   Long_64b     8      big-endian
0x98   Long_64l     8      little-endian
0xA3   Real         4      native
0xA4   Real_b       4      big-endian
0xA5   Real_l       4      little-endian
0xA6   Double       8      native
0xA7   Double_b     8      big-endian
0xA8   Double_l     8      little-endian
```

**128-bit numeric tokens** (outside the endianness-variant range):

```
Token  Type         Bytes  Endianness
-----  -----------  -----  ----------
0xF0   Int128       16     native (no b/l variants)
0xF1   UInt128      16     native (no b/l variants)
```

Format: `[token byte] [16 bytes of native-endian payload]`.  `Int128` and
`UInt128` are out-of-band with the rest of the numeric tokens because the
range 0x80-0xA9 was already fully allocated when 128-bit types were added;
unlike 16/32/64-bit types, only a native-endian form is defined.

**Adaptive width encoding:** Integer types have 8-bit, 16-bit, 32-bit, and
64-bit variants.  A compiler can choose the smallest encoding that fits:

| Value     | Text representation | Smallest binary encoding       | Bytes saved |
| --------- | ------------------- | ------------------------------ | ----------- |
| `42`      | `42` (2 chars)      | `0x81 0x2A` (Integer_8)        | 0           |
| `1000`    | `1000` (4 chars)    | `0x82 0x03E8` (Integer_16)     | 1           |
| `1000000` | `1000000` (7 chars) | `0x85 0x000F4240` (Integer_32) | 2           |
| `3.14d`   | `3.14d` (5 chars)   | `0xA6 [8 bytes]` (Double)      | -4          |

Binary tokens are most beneficial for integer-heavy code and for tokens that
would otherwise require many characters (long names, string literals).

### 8.4 String Tokens

```
Token  Type        Length field  Max length
-----  ----------  -----------  ----------
0xAE   String_8    1 byte       255
0xAF   String_16   2 bytes      65,535 (native endian)
0xB0   String_16b  2 bytes      65,535 (big-endian)
0xB1   String_16l  2 bytes      65,535 (little-endian)
```

Format: `[token byte] [length] [raw string bytes]`

The string bytes are stored verbatim -- no escape processing, no NUL
terminator in the stream.  This is the most compact representation possible.

### 8.5 Name Tokens: Names vs. Values

The binary encoding distinguishes between pushing a **name** (the key itself)
and pushing a name's **value** (the object currently bound to that name):

```
Names (push the Name object):
  0xB2-0xB5   SystemLitName    (8/16-bit index, literal)
  0xB6-0xB9   SystemExecName   (8/16-bit index, executable)

Values (push what the name is bound to):
  0xBA-0xBD   SystemLitValue   (like //name — immediate lookup)
  0xBE-0xC1   SystemExecValue  (like \\name — preserving execute bit)

Well-known names (fixed 1-byte index into the WellKnownName table):
  0xEE        WellKnownLitName  (literal)
  0xEF        WellKnownExecName (executable)
```

The distinction between Name and Value tokens is the binary equivalent of
`/name` vs. `//name`.  A Trix compiler can pre-resolve names at compile time
and emit Value tokens for operators that are known at build time, eliminating
runtime dict lookup entirely.

The 0xEE/0xEF well-known-name tokens are a compact encoding for the small
set of internal names used by core language features (arithmetic operators,
control-flow primitives, common method names like `get`).  Unlike
`SystemLitName`/`SystemExecName`, which use the full `SystemName` index
space (up to 16 bits), well-known names use a single byte -- one token byte
plus one index byte -- for the names the runtime itself needs to emit
(field-access sugar, infix expression lowering, etc.).

### 8.6 The Constant Pool: Single-Byte Tokens

The ranges 0xC2-0xC5, 0xC7-0xED contain 43 pre-encoded constants.
Each is a complete token in a single byte -- no payload, no parsing:

```
Token  Value              Token  Value
-----  -----------------  -----  ------------------
0xC2   Integer 2          0xD4   Long MIN
0xC3   Integer 10         0xD5   Long MAX
0xC4   Integer 100        0xD6   Long -1
0xC5   Byte 2             0xD7   Long 0
0xC7   Byte 0             0xD8   Long 1
0xC8   Byte 1             0xD9   ULong 0
0xC9   Byte 127           0xDA   ULong MAX
0xCA   Byte 128           0xDB   ULong 1
0xCB   Byte 255           0xDC   Real -1.0
0xCC   Integer MIN        0xDD   Real 0.0
0xCD   Integer MAX        0xDE   Real 1.0
0xCE   Integer -1         0xDF   Real e
0xCF   Integer 0          0xE0   Real pi
0xD0   Integer 1          0xE1   Real +infinity
0xD1   UInteger 0         0xE2   Real quiet NaN
0xD2   UInteger MAX       0xEB   Integer -2
0xD3   UInteger 1         0xEC   Real 2.0
                           0xED   Double 2.0
0xE3   Double -1.0        0xE8   Double +infinity
0xE4   Double 0.0         0xE9   Double quiet NaN
0xE5   Double 1.0         0xEA   Address nullptr
0xE6   Double e
0xE7   Double pi
```

**Why this matters:** Consider a common Trix pattern:

```trix
0 1 10 { dup mul } for
```

In text, this is 23 bytes.  In binary tokens:

```
0xCF                 % Integer 0  (1 byte, constant token)
0xD0                 % Integer 1  (1 byte, constant token)
0xC3                 % Integer 10 (1 byte, constant token)
0xB6 [packed-idx]    % SystemExecName for packed proc (2 bytes)
                     %   packed body: dup=CommonOp(1), mul=PosOp(2) = 3 bytes
                     %   + packed header/length = ~2 bytes overhead
0xB6 [for-index]     % SystemExecName 'for' (2 bytes)
                     %                       Total: ~11 bytes
```

That is 11 bytes vs. 23 bytes of text -- a **2.1x reduction** for this small
example.  The savings grow with program size because constant tokens (single
byte for `0`, `1`, `true`, `false`, `null`, `pi`, etc.) and packed procedures
compress the most common patterns.  The constant pool covers the values that
appear most frequently in real code: 0, 1, 2, -1, -2, 10, 100, MIN, MAX,
pi, e, infinity, NaN.

### 8.7 NumberArray Token (0xC6)

A specialized token for typed arrays of homogeneous numbers:

```
Format: 0xC6 [element_type: uint8] [count: uint16] [element data...]
```

All elements share the same type (encoded as a 0-56 representation code, NOT
the BinaryToken opcode).  Bit 7 of `element_type` selects endianness for the
count and all elements (0 = big-endian, 1 = little-endian).  This is the most
compact representation for numeric tables, lookup arrays, and signal data.

### 8.8 Worked Example: Space Efficiency

Consider this Trix program:

```trix
/factorial { |n|
    n 1 le { 1 } { n n 1 sub factorial mul } if-else
} def
```

**Text representation:** 71 bytes (human-readable, with whitespace).

**Binary token representation** (approximate):
<!-- doctest: skip (binary token-encoding byte breakdown, not runnable Trix) -->
```
0xB2 [idx]           % SystemLitName 'factorial' (2 bytes)
                     % procedure body as packed array:
                     %   begin-locals, 'n', end-locals
                     %   n, 1, le, { 1 }, { n, n, 1, sub, factorial, mul }, if-else
0xB6 [def-idx]       % SystemExecName 'def' (2 bytes)
```

The packed procedure body internally compresses operators, small integers, and
common operations (dup, pop, if-else are `CommonOp` single-byte packed types).
A realistic estimate for the full binary form: ~25-30 bytes -- a 2-3x reduction.

---

## 9. Packed Array Encoding

While binary tokens handle the stream-to-object conversion, packed arrays
handle the in-memory representation of compiled procedures.  Together, they
form a two-level compression system.

### 9.1 The Packed Header Byte

Every element in a packed array is prefixed by a single header byte:

```
  7   6   5   4   3   2   1   0
+---+---+---+---+---+---+---+---+
| X |  SS   |     TTTTT         |
+---+---+---+---+---+---+---+---+
  |   \   /   \               /
  |    \ /     -------+-------
  |     |             |
  |     |             +---  PackedType (5 bits, 0-31)
  |     +---  Size-1 field (2 bits, 0-3 extra bytes)
  +---  Executable attribute (1 = executable, 0 = literal)
```

The `SS` field encodes 0-3 bytes of additional data after the header.  For
types that need more, the packed type itself indicates the length format
(ShortLength = 1-byte length, LongLength = 2-byte length).

### 9.2 The 31 Packed Types

| Type | ID | Description | Size encoding |
| --- | --- | --- | --- |
| `CommonOp` | 0 | 8 hot operators | X\|SS = 3-bit slot (no extra bytes) |
| `Byte` | 1 | 0-255 | SS = 1 extra byte |
| `Integer` | 2 | signed int (sign-aware) | SS = 1-3 extra bytes (variable) |
| `PackedExt` | 3 | 128-bit (Int128/UInt128) via subcode | X=0, SS=offset size; 1-byte subcode + 1-4 offset bytes |
| `UInteger` | 4 | unsigned int | SS = 1-3 extra bytes (variable) |
| `Long` | 5 | 64-bit signed | SS = 1-4 byte vm_offset_t to an ExtValue cell (the 64-bit value is heap-stored, not inline) |
| `ULong` | 6 | 64-bit unsigned | SS = 1-4 byte vm_offset_t to an ExtValue cell (the 64-bit value is heap-stored, not inline) |
| `Address` | 7 | pointer-width | ExtValue in VM heap |
| `Real` | 8 | 32-bit float | 4 extra bytes |
| `Double` | 9 | 64-bit float | vm_offset_t (1-4 bytes) to an 8-byte ExtValue cell |
| `Simple` | 10 | Null/Mark/False/True | SS = sub-type (no extra bytes) |
| `Curry` | 11 | curried function | encoded inline |
| `Operator` | 12 | operator (sign-aware index) | SS = 1-2 bytes |
| `Reserved2` | 13 | reserved | -- |
| `Record` | 14 | immutable record | X=fc width + SS=offset bytes |
| `Name` | 15 | name by index | SS = 2-4 bytes (minimum 2 for bind compatibility) |
| `ShortLengthArray` | 16 | R/W Array | 1-byte length + elements |
| `LongLengthArray` | 17 | R/W Array | 2-byte length + elements |
| `ReadOnlyShortLengthArray` | 18 | R/O Array | 1-byte length + elements |
| `ReadOnlyLongLengthArray` | 19 | R/O Array | 2-byte length + elements |
| `ReadOnlyShortLengthPacked` | 20 | R/O Packed | 1-byte length + elements |
| `ReadOnlyLongLengthPacked` | 21 | R/O Packed | 2-byte length + elements |
| `ShortLengthString` | 22 | R/W String | 1-byte length + bytes |
| `LongLengthString` | 23 | R/W String | 2-byte length + bytes |
| `ReadOnlyShortLengthString` | 24 | R/O String | 1-byte length + bytes |
| `ReadOnlyLongLengthString` | 25 | R/O String | 2-byte length + bytes |
| `ShortLengthStream` | 26 | R/W Stream | 1-byte length + bytes |
| `LongLengthStream` | 27 | R/W Stream | 2-byte length + bytes |
| `ReadOnlyShortLengthStream` | 28 | R/O Stream | 1-byte length + bytes |
| `ReadOnlyLongLengthStream` | 29 | R/O Stream | 2-byte length + bytes |
| `Dict` | 30 | dict | encoded entries |

### 9.3 CommonOp: Single-Byte Operators

The eight most frequently used operators are encoded as a single byte with no
extra data.  The X and SS bits of the header double as a 3-bit slot index:

| Slot | Operator  | Frequency rationale              |
| ---- | --------- | -------------------------------- |
| 0    | `exch`    | Stack manipulation (fundamental) |
| 1    | `dup`     | Stack manipulation (fundamental) |
| 2    | `pop`     | Stack manipulation (fundamental) |
| 3    | `index`   | Stack access                     |
| 4    | `roll`    | Stack rotation                   |
| 5    | `if`      | Control flow                     |
| 6    | `if-else` | Control flow                     |
| 7    | `eq`      | Comparison                       |

These eight operators account for a large fraction of tokens in typical Trix
code.  Encoding each as a single byte (rather than a name lookup) is a
significant space and time optimization.

### 9.4 Variable-Width Integer Encoding

`Integer`, `UInteger`, and `Operator` use the SS field for adaptive width:

```
SS = 00: 1 extra byte  (values -128..127 signed, 0..255 unsigned)
SS = 01: 2 extra bytes (values -32768..32767 signed, 0..65535 unsigned)
SS = 10: 3 extra bytes (values -8388608..8388607 signed, 0..16777215 unsigned)
SS = 11: 4 extra bytes (values -2147483648..2147483647 signed, 0..4294967295 unsigned)
```

`Integer` and `Operator` use sign-aware byte stripping: both leading 0x00 bytes
(positive values) and leading 0xFF bytes (negative values) are stripped,
preserving the sign bit in the most-significant stored byte.  On unpack, the
MSB's high bit determines whether to sign-extend with 0xFF or zero-extend
with 0x00.

A small integer like `5` takes 2 bytes total (header + 1 data byte).  `-1`
also takes 2 bytes (header + 0xFF).  The maximum 4-byte payload (SS=11) covers
the full 32-bit Integer/UInteger range.  Integer/UInteger never promote to
`Long`/`ULong` on overflow of a smaller payload; only an explicit `l`/`ul`
literal produces `Long`/`ULong`.

### 9.5 Record Encoding

Records use a custom packed encoding that repurposes the X bit as a field
count width selector (records are always literal, so X has no executable
meaning):

```
Header: |X|SS|01110|   (TTTTT = 14 = Record)
         |  |
         |  +-- SS: vm_offset_t byte count - 1 (1-4 bytes)
         +---- X: field count width (0 = 1 byte, 1 = 2 bytes)

Layout: [header: 1 byte] [field_count: 1 or 2 bytes] [offset: SS+1 bytes]
```

Field count range: X=0 supports 0-255 fields (1 byte), X=1 supports
0-65535 fields (2 bytes, big-endian).  The offset is the vm_offset_t
pointing to the record instance in VM heap.

Example: a 2-field record with offset 0x1234 encodes as:
```
Header:      0|01|01110 = 0x2E  (X=0, SS=01 means 2-byte offset, type=Record)
Field count: 0x02               (2 fields)
Offset:      0x12 0x34          (big-endian vm_offset_t)
Total: 4 bytes
```

### 9.6 Worked Example: Procedure Compression

Consider the procedure `{ dup mul add }`:

**As an Array (uncompressed):** 3 Objects x 8 bytes = 24 bytes, plus the Array
header.

**As a Packed array:**
```
Header byte for dup:  0x01  (CommonOp, slot 1)     = 1 byte
Header byte for mul:  [Operator, SS=1, mul-idx]      = 2 bytes
Header byte for add:  [Operator, SS=1, add-idx]      = 2 bytes
                                              Total = 5 bytes
```

`dup` is a CommonOp (slot 1) -- exactly 1 byte with no payload.  `mul` and
`add` are not in the CommonOp table, so they use `Operator` with a 1-byte
index (2 bytes each).

**Compression ratio:** 5 bytes vs 24 bytes = **4.8x compression**.

For a more complex example, `/fact { |n| n 0 gt { n n 1 sub fact mul } { 1 } if-else } def`
(recursion is by name -- Trix has no `recurse` operator):
- `if-else` is CommonOp slot 6: 1 byte
- Small integers (0, 1) are Integer with SS=01: 2 bytes each
- `gt`, `sub`, `mul` are Operator: 2 bytes each
- `n` and the recursive call `fact` are Name: 2 bytes per occurrence
- Nested procedures: header + packed contents

The packed form is typically 3-5x smaller than the uncompressed Array form.
This matters for embedded deployments where VM memory is at a premium.

**Speed tradeoff -- the array form is faster in hot loops.**  Packed
compactness is not free: a packed body is re-decoded element by element
on *every* execution, so a proc run N times in a loop pays the decode N
times (see `interpreter.md` Section 8.1).  The array form (`#a`) stores
the already-decoded `Object[]` and skips that work.  On a dispatch-bound
loop the difference is roughly 15% of wall-clock (measured: a 100M-op
`{ 1 2 add pop }` microbenchmark runs 2.21 s packed vs 1.89 s with `#a`).
The rule of thumb: keep the default packed form for code that runs a few
times or where memory is tight; reach for `#a` on a small body that
dominates a hot loop, accepting the ~8-16x larger footprint for that one
proc.

**The bigger lever is usually `#e` (early binding), not `#a`.**  `#a` removes
the per-iteration *decode*; `#e` (Section 6.4) removes the per-call *name
lookup*.  `#e` is larger on the same microbenchmark (-23.9% vs -14.6%) and --
because the body stays packed -- carries no footprint penalty, so it keeps
paying off when applied across a whole hot path rather than only on one
amortized loop body (early-binding the bundled Z-machine interpreter's hot core
cut 10.7% off a real-game walkthrough; see `interpreter.md` Section 8.2).  The
two are orthogonal and stack as `#ae`.

### 9.6 Simple Sub-Types

The `Simple` packed type uses the SS field as a 2-bit sub-type selector,
encoding four common constants in a single byte with no payload:

| SS  | Value |
| --- | ----- |
| 00  | Null  |
| 01  | Mark  |
| 10  | False |
| 11  | True  |

---

## 10. Error Handling

The scanner raises specific errors for specific conditions:

| Error | Trigger |
| --- | --- |
| `SyntaxError` | Unbalanced close delimiters (`)`, lone `>`, `}`), unterminated `(` or `{`, invalid escape sequence, odd hex digits in `<...>`, missing `>` in `<(...)>`, NUL or DEL in source, unterminated block comment, unterminated or empty local binding `\|...\|`, invalid or out-of-range `\|...\|#N` capacity suffix, procedure nesting depth > 64, `\u`/`\U` with insufficient hex digits or surrogate/out-of-range codepoint, empty `\{}` interpolation, EOF inside `(...)`, `<...>`, `<(...)>`, `{...}`, or `%{...%}` |
| `UnmatchedMark` | A lone `]`, `>>`, or `}}` with no matching mark on the operand stack |
| `TypeError` | Invalid type in a context that requires a specific type |
| `LimitCheck` | Name length > 127, string length > 65,535, infix expression nesting > 64, infix expression token count > 128, field-access chain length > 64, =string/=array/=proc capacity exceeded |
| `NumericalOverflow` | Integer/UInteger/Long/ULong/Real literal exceeds type range; Real overflow to +/-infinity |
| `Undefined` | Immediate lookup (`//name` or `\\name`) not found in dict stack; `\{name}` interpolation of undefined name |
| `Unsupported` | Illegal suffix combination (e.g., `#pw` -- Packed is always ReadOnly); reserved binary token (0xF2-0xFF) |
| `VMFull` | VM heap exhausted during string, procedure, or name construction |

**Scanner errors in procedures:** Errors encountered while scanning a procedure
body (between `{` and `}`) occur at scan time, not at execution time.  This
means `try` cannot catch scanner errors inside `{ }` -- the procedure object
was never created.  To test scanner error handling, use executable strings:

<!-- doctest: skip (the first line is an uncatchable scan-time error, by design) -->
```trix
% This does NOT work (error at scan time, before try runs):
{ (\uD800) } try    % scanner error kills the enclosing compilation

% This works (error deferred to execution via executable string):
{ <((\uD800))>#x exec } try /syntax-error eq   % true
```

---

## 11. Design Decisions

### 11.1 Two Radix Notations: `N#digits#` and `0x`/`0o`/`0b`

Trix offers both radix notations because they serve different needs.
`N#digits#` is the general form -- one pattern for every base from 2 to 36
(`16#FF#`, `36#Z#`, `7#66#`) -- a reach the three C-style prefixes can never
match.  The `0x`/`0o`/`0b` prefixes cover only the three common bases, but
they are the spelling every C, Rust, Python, and Go programmer already types,
and `0x` carries C99 hex-float notation (`0x1.0p10`) for free.  Supporting
both costs the scanner almost nothing -- the prefix forms are recognized by a
leading `0` plus a base letter -- and lets a reader reach for whichever
notation is idiomatic for the value at hand.

### 11.2 Scan-Time String Interpolation

Trix supports scan-time interpolation in text strings via `\{name}`:

```trix
/x 42 def
(x is \{x})                                % -> "x is 42"
(pi = \{:systemdict:numbers:real-type:pi}) % -> "pi = 3.14159..."
```

The interpolated name is looked up at scan time (like `//name`) and its
value is formatted into the string via `PrintFmt::process_object()`.  All
types are interpolatable.  Dictionary paths are supported.  This works
only in text strings `(...)`, not in hex `<...>` or raw `<(...)>` strings.

For runtime formatting, use `sprint-fmt` or `print-fmt` with the stack:
<!-- doctest: skip (sprint-fmt synopsis; placeholder name/age operands) -->
```trix
name age (%s is %d years old) sprint-fmt
```

### 11.3 Why No Regex Literals

The `/` character is the literal-name prefix.  `//` is immediate lookup.
Overloading `/pattern/flags` would require significant lookahead or a
completely different sigil.  Meanwhile, raw strings `<(pattern)>` already
eliminate the double-escaping problem:

```trix
<(\d{3}-\d{4})> regex-match    % no escaping needed
```

String-based patterns are also more flexible -- they can be computed
dynamically, concatenated, and stored in variables.

### 11.4 Why Three String Forms

Each form solves a specific problem:

| Form      | Use case                              | Escapes          | Nesting       |
| --------- | ------------------------------------- | ---------------- | ------------- |
| `(...)`   | General text, code with special chars | Full suite       | Balanced `()` |
| `<...>`   | Binary data, exact byte sequences     | None (hex pairs) | None          |
| `<(...)>` | Regex, paths, embedded code           | None (raw)       | Balanced `()` |

Two forms would force compromises.  One form would be painful.  Three covers
every real-world case with minimal overlap.

### 11.5 Why Immutability at Construction

Most languages add immutability after the fact: Python strings are implicitly
immutable, Ruby has `freeze`, Rust has `mut`.  Trix declares access mode at
construction via the `#r` suffix:

```trix
(mutable string)  % ReadWrite -- can modify via put
(frozen string)#r % ReadOnly  -- put raises InvalidAccess at runtime
[1 2 3]#r         % ReadOnly array
<< /x 1 >>#r      % ReadOnly dict
{{ 1 2 3 }}#r     % ReadOnly set
```

This is enforced by the VM's access mode check, not by convention.  The
suffix applies to strings, arrays, dicts, sets, and procedures uniformly.

### 11.6 Why Nested Block Comments

Most block comment implementations (`/* */` in C, Java, JavaScript) fail on
the most common use case: commenting out code that already contains comments.
Nested `%{ ... %}` handles this correctly at any depth, making it safe to
comment out arbitrary code blocks during development.

---

## 12. Scanner Performance

### 12.1 Character Classification: O(1) Per Byte

Every classification predicate is a single table lookup and bitmask AND.
The 257-entry `sm_chattr` table is `constexpr` -- it exists in read-only memory
and is typically hot in L1 cache.

```cpp
[[nodiscard]] static constexpr bool is_regular(int ch) {
    return ((sm_chattr[ch] & RG) != 0);
}
```

### 12.2 Buffered Fast Paths

The two hottest scanner loops -- `skip_whitespace` and `scan_regular_chars` --
operate directly on the stream's buffer pointers, avoiding per-byte function
call overhead:

```
skip_whitespace:
    while (rptr != rlimit):
        if is_whitespace(*rptr):
            advance rptr, update line tracking
        else:
            return (found non-whitespace)
    return (buffer exhausted, need refill)

scan_regular_chars:
    while (rptr != rlimit):
        if is_regular(*rptr):
            copy to token buffer, advance, check length
        else:
            return (found non-regular)
    return (buffer exhausted, need refill)
```

These loops process the bulk of input bytes.  The buffer-at-a-time design
means the scanner touches each byte exactly once, with no backtracking.

### 12.3 Single-Pass Design

The scanner produces fully typed `Object` values directly -- no intermediate
token struct, no string-to-value conversion pass.  When the scanner finishes
a numeric token, the `Number` class has already parsed it into the correct
type (Integer, Double, etc.) with overflow detection.  When it finishes a
string, the bytes are already in VM memory with the NUL terminator in place.

This single-pass design is a direct consequence of the stack-based execution
model: the interpreter consumes tokens one at a time, in order, with no need
for lookahead beyond the current token.

---

## 13. Quick Reference

### 13.0 All Sigils at a Glance

**Invariant:** `$` always means "global VM" in both prefix and suffix
position; `$$` always means "force-local VM"; `#` always introduces a
literal-shape suffix.  Knowing those three rules collapses most of the
table below to "compose the sigils to mean what they read like".

#### Prefixes — names, sugar, scopes, infix, strings

| Group  | Form                  | Meaning                                                    |
| ------ | --------------------- | ---------------------------------------------------------- |
| Name   | `/foo` `\foo`         | Literal / Executable Name                                  |
| Name   | `//foo` `\\foo`       | Immediate-lookup (scan-time resolve)                       |
| Name   | `$/foo` `$\foo`       | Name interned in **global VM**                             |
| Name   | `$$/foo` `$$\foo`     | Name **force-interned in local VM**                        |
| Sugar  | `.field`              | Field access — desugars to `/field get` (chains: `.a.b.c`) |
| Block  | `${ body }`           | Global-alloc scope                                         |
| Block  | `$${ body }`          | Force-local scope (inverse)                                |
| Infix  | `$[ expr ]`           | Promoted (auto-widening) infix                             |
| Infix  | `$( expr )`           | Non-promoting infix                                        |
| String | `(text)`              | Escaped string literal                                     |
| String | `<(raw)>`             | Raw string                                                 |
| String | `<48 65>`             | Hex string                                                 |
| Number | `0x` `0b` `0o` `R#…#` | Base-prefixed integer (R = 2..36)                          |
| Number | `-`                   | Negative                                                   |

#### Mark / bracket delimiters

| Open | Close | Form              |
| ---- | ----- | ----------------- |
| `[`  | `]`   | Array literal     |
| `<<` | `>>`  | Dict literal      |
| `{{` | `}}`  | Set literal       |
| `{`  | `}`   | Procedure literal |

#### Comments

| Form        | Meaning                  |
| ----------- | ------------------------ |
| `% ...`     | Line comment             |
| `%{ ... %}` | Block comment (nestable) |

#### Suffixes — numbers

| Suffix               | Type                               |
| -------------------- | ---------------------------------- |
| *(none, int-shape)*  | `Integer` (i32)                    |
| *(none, real-shape)* | `Real` (f32)                       |
| `i` `u`              | `Integer` (i32) / `UInteger` (u32) |
| `l` `ul`             | `Long` (i64) / `ULong` (u64)       |
| `a`                  | `Address` (vm_offset_t)            |
| `r` `d`              | `Real` (f32) / `Double` (f64)      |
| `b`                  | `Byte` (u8)                        |
| `q` `uq`             | `Int128` / `UInt128`               |

#### Suffixes — strings — `#[b]` \| `#a[r\|w]` \| `#[=\|$\|$$][l\|x][r\|w]`

| Suffix           | Meaning                            |
| ---------------- | ---------------------------------- |
| `#b`             | Single Byte (length-1 string only) |
| `#a` `#ar` `#aw` | Byte array (no string allocated)   |
| `#=`             | eq-string (pre-allocated scratch)  |
| `#$`             | Force bytes in **global VM**       |
| `#$$`            | Force bytes in **local VM**        |
| `#l` `#x`        | Literal / Executable attribute     |
| `#r` `#w`        | ReadOnly / ReadWrite               |

#### Suffixes — Array / Dict / Set — `#[=|$|$$][r|w]`

| Suffix    | Meaning                                                 |
| --------- | ------------------------------------------------------- |
| *(none)*  | Follow enclosing `m_curr_alloc_global` (default: local) |
| `#=`      | eq-temporary storage                                    |
| `#$`      | Force container in **global VM**                        |
| `#$$`     | Force container in **local VM**                         |
| `#r` `#w` | ReadOnly / ReadWrite (default)                          |

#### Suffixes — Procedures — `#[=|$|$$][a|p][e|l][r|w]`

| Suffix    | Meaning                                |
| --------- | -------------------------------------- |
| `#=`      | eqproc (scratch storage)               |
| `#$`      | Force body in **global VM**            |
| `#$$`     | Force body in **local VM**             |
| `#a` `#p` | Array form / Packed form (default)     |
| `#e` `#l` | Early binding / Late binding (default) |
| `#r` `#w` | ReadOnly (`#pw` rejected) / ReadWrite  |

Example combinations: `#$er` (global packed early-bound read-only),
`#$$aer` (force-local array early-bound read-only).

#### Suffixes — locals preamble capacity

| Suffix            | Meaning                                          |
| ----------------- | ------------------------------------------------ |
| `\| a b c \|`     | Bare preamble — frame dict capacity = K (here 3) |
| `\| a b c \| #N`  | Absolute capacity (N ≥ K)                        |
| `\| a b c \| #+N` | Relative capacity (total = K + N)                |
| `\| \| #N`        | Empty preamble — empty frame dict of capacity N  |

#### Mutex constraints

  * **First slot of `#`** is one of `=` / `$` / `$$` — any pair raises
    `/syntax-error` ("mutually exclusive").
  * **`#pw`** raises `/unsupported` (packed procs are always ReadOnly).
  * **`#$$$` suffix** raises `/syntax-error` (max two `$` after `#`).  A bare
    `$$$` prefix is NOT special — it parses as an ordinary name.

---

### 13.1 Numeric Literals

```trix
42             % Integer
42i            % Integer (explicit)
255b           % Byte
1000u          % UInteger
999999l        % Long
100ul          % ULong
4096a          % Address
3.14           % Real
3.14d          % Double
16#FF#         % hex 255 (radix form)
2#1010#        % binary 10 (radix form)
8#777#u        % octal 511u
0xFF           % hex 255 (C-style prefix)
0b1010         % binary 10 (C-style prefix)
0o777          % octal 511 (C-style prefix)
0x1.8p1        % hex float 3.0
1_000_000      % underscore separators
```

### 13.2 String Literals

<!-- doctest: skip (string-literal syntax illustration; (code)#x is executable and looks up undefined code) -->
```trix
(hello)          % text string
(line\n\ttab)    % with escapes
(caf\u00E9)      % Unicode escape
<48 65 6C 6C 6F> % hex string: "Hello"
<(\d+\.\d+)>     % raw string (regex)
(A)#b            % byte extraction: 65
(frozen)#r       % read-only string
(code)#x         % executable string
(temp)#=         % temporary storage
(Hello)#a        % byte array: [72b 101b 108b 108b 111b]
(Hello)#ar       % read-only byte array
```

### 13.3 Names

<!-- doctest: skip (name-form syntax illustration; \name and //name require name defined) -->
```trix
/name                              % literal name
\name                              % executable name
//name                             % immediate lookup (literal result)
\\name                             % immediate lookup (preserving execute bit)
//:systemdict:numbers:real-type:pi % dictionary path
```

### 13.4 Composites

```trix
[1 2 3]           % array
[1 2 3]#r         % read-only array
<< /x 1 /y 2 >>   % dictionary
<< /x 1 >>#r      % read-only dictionary
{{ 1 2 3 }}       % set
{{ /a /b /c }}#r  % read-only set
{ dup mul }       % procedure (packed, late-bound)
{ dup mul }#e     % early-bound
{ dup mul }#a     % array representation
{ |x y| x y add } % local variable bindings
```

### 13.5 Infix Expressions

```trix
$( 2 + 3 * 4 )             % strict: 14 (precedence respected)
$[ 3 + 4.0 ]               % auto-promote: 7.0
$( sqrt(9.0) )             % function call: 3.0
$( max(a, b) + 1 )         % multi-arg function + arithmetic
$( a * x * x + b * x + c ) % polynomial
```

### 13.6 Comments

```trix
% line comment
%{ block comment (nests) %}
#!/usr/bin/env trix      % shebang (first line only)
```
