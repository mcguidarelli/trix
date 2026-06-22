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

# String Processing in Trix

Trix provides a comprehensive string processing toolkit: literal syntax with
escape sequences and scan-time interpolation, regex matching and replacement,
split/join/trim/pad operations, format strings for both output (printf) and
input (scanf), character predicates, binary pack/unpack, and case conversion.
All string operations work on byte strings with explicit encoding boundaries.

This document covers the complete string system: creation syntax, all
operators with examples, regex patterns, format strings, and real-world
processing patterns.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [String Creation](#3-string-creation)
4. [Access and Modification](#4-access-and-modification)
5. [Search and Matching](#5-search-and-matching)
6. [Transformation](#6-transformation)
7. [Splitting and Joining](#7-splitting-and-joining)
8. [Regular Expressions](#8-regular-expressions)
9. [Character Predicates](#9-character-predicates)
10. [Format Strings](#10-format-strings)
11. [Scan-Time String Interpolation](#11-scan-time-string-interpolation)
12. [String Comparison](#12-string-comparison)
13. [Type Conversion](#13-type-conversion)
14. [Save/Restore Behavior](#14-saverestore-behavior)
15. [Real-World Patterns](#15-real-world-patterns)

---

## 1. Overview

Trix strings are mutable byte arrays.  Each byte is an unsigned 8-bit value
(0-255).  Strings have no inherent encoding -- they are raw byte sequences.
When string content represents text, it is conventionally UTF-8, but the
language does not enforce this.

Key characteristics:

- **Mutable by default**: `put` modifies bytes in place.  Use `#r` for
  read-only strings.
- **Byte-addressed**: `length` returns byte count.  `get` returns a byte at
  an index.  Negative indexing is supported.
- **No null termination**: Strings can contain null bytes.
- **VM-allocated**: String data lives in the VM heap.  Strings allocated after
  a save point are discarded on restore (but byte modifications via `put` are
  not journaled).

---

## 2. Quick Reference

### Creation and Conversion

| Operator    | Stack Effect     | Description                                          |
| ----------- | ---------------- | ---------------------------------------------------- |
| `string`    | `int -- str`     | Create zero-filled string of length N                |
| `to-string` | `any dst -- str` | Convert any value to string (writes into dst buffer) |
| `to-name`   | `str -- name`    | Convert string to name                               |
| `to-number` | `str -- num`     | Parse string as number                               |
| `concat`    | `str str -- str` | Concatenate two strings                              |

### Access

| Operator       | Stack Effect             | Description                 |
| -------------- | ------------------------ | --------------------------- |
| `length`       | `str -- int`             | Byte length                 |
| `get`          | `str index -- byte`      | Get byte at index           |
| `put`          | `str index byte --`      | Set byte at index           |
| `get-interval` | `str index count -- str` | Extract substring           |
| `copy`         | `src dst -- dst`         | Copy bytes into destination |

### Search

| Operator          | Stack Effect                                   | Description                          |
| ----------------- | ---------------------------------------------- | ------------------------------------ |
| `search`          | `str seek -- post match pre true \| str false` | Find first occurrence                |
| `string-index-of` | `str substr -- int`                            | Index of substring (-1 if not found) |
| `contains?`       | `str substr -- bool`                           | Substring test                       |
| `starts-with?`    | `str prefix -- bool`                           | Prefix test                          |
| `ends-with?`      | `str suffix -- bool`                           | Suffix test                          |
| `count-substring` | `str sub -- int`                               | Count non-overlapping occurrences    |

### Transformation

| Operator        | Stack Effect            | Description                 |
| --------------- | ----------------------- | --------------------------- |
| `uppercase`     | `str -- str`            | Convert to uppercase        |
| `lowercase`     | `str -- str`            | Convert to lowercase        |
| `capitalize`    | `str -- str`            | Uppercase first byte        |
| `trim`          | `str -- str`            | Remove whitespace both ends |
| `trim-left`     | `str -- str`            | Remove leading whitespace   |
| `trim-right`    | `str -- str`            | Remove trailing whitespace  |
| `reverse`       | `str -- str`            | Reverse byte order          |
| `replace`       | `str old new -- str`    | Replace all occurrences     |
| `remove-prefix` | `str prefix -- str`     | Remove prefix if present    |
| `remove-suffix` | `str suffix -- str`     | Remove suffix if present    |
| `repeat-string` | `str count -- str`      | Repeat N times              |
| `pad-left`      | `str width byte -- str` | Left-pad to width           |
| `pad-right`     | `str width byte -- str` | Right-pad to width          |

### Splitting and Joining

| Operator | Stack Effect       | Description               |
| -------- | ------------------ | ------------------------- |
| `split`  | `str delim -- arr` | Split by delimiter        |
| `join`   | `arr delim -- str` | Join array with delimiter |
| `chars`  | `str -- arr`       | Split into array of bytes |

### Regex (POSIX ERE)

| Operator         | Stack Effect                             | Description         |
| ---------------- | ---------------------------------------- | ------------------- |
| `regex-match`    | `str pattern -- bool`                    | Match test          |
| `regex-search`   | `str pattern -- [matches] true \| false` | Find with groups    |
| `regex-find-all` | `str pattern -- arr`                     | Find all matches    |
| `regex-replace`  | `str pattern repl -- str`                | Replace all matches |
| `regex-split`    | `str pattern -- arr`                     | Split by pattern    |

### Character Predicates

| Operator     | Stack Effect        | Description       |
| ------------ | ------------------- | ----------------- |
| `digit?`     | `byte\|str -- bool` | 0-9               |
| `alpha?`     | `byte\|str -- bool` | a-z A-Z           |
| `alnum?`     | `byte\|str -- bool` | a-z A-Z 0-9       |
| `hex-digit?` | `byte\|str -- bool` | 0-9 A-F a-f       |
| `lower?`     | `byte\|str -- bool` | a-z               |
| `upper?`     | `byte\|str -- bool` | A-Z               |
| `space?`     | `byte\|str -- bool` | HT LF VT FF CR SP |
| `printable?` | `byte\|str -- bool` | 0x20..0x7E        |

### Format Strings

| Operator      | Stack Effect                                        | Description            |
| ------------- | --------------------------------------------------- | ---------------------- |
| `print-fmt`   | `fmt mark args -- count true \| count false`        | Format to stdout       |
| `sprint-fmt`  | `dst fmt mark args -- str true \| count false`      | Format to string       |
| `fprint-fmt`  | `stream fmt mark args -- count true \| count false` | Format to stream       |
| `sscan-fmt`   | `src fmt mark args -- args count`                   | Parse from string      |
| `aprint-fmt`  | `fmt arr -- count true \| count false`              | Format from array      |
| `asprint-fmt` | `dst fmt arr -- str true \| count false`            | Format array to string |
| `afprint-fmt` | `stream fmt arr -- count true \| count false`       | Format array to stream |

---

## 3. String Creation

### 3.1 Literal Strings

Strings are delimited by parentheses:

```
(hello)                  % basic string
()                       % empty string
(multi-line
string)                  % newlines preserved
(nested ((parens)))      % balanced parens are allowed
```

### 3.2 Escape Sequences

Inside `(...)`, backslash introduces escape sequences:

```
\n    newline (10)          \r    carriage return (13)
\t    tab (9)               \b    backspace (8)
\f    form feed (12)        \v    vertical tab (11)
\a    bell (7)              \e    escape (27)
\\    backslash (92)        \0    null byte (0)
\(    literal (             \)    literal )
\101  octal (1-3 digits)    \xFF  hex (1-2 digits)
\^A   caret control (1)     \^Z   caret control (26)
\uXXXX  Unicode BMP (4 hex) \UXXXXXXXX  Unicode (8 hex; both emit UTF-8)
```

```
(line one\nline two)     % two lines
(tab\there)              % tab character
(C:\\Users\\data)        % backslashes
(null: \0 byte)          % embedded null
```

### 3.3 String Suffixes

```
(hello)#r       % ReadOnly (put raises read-only error)
(hello)#x       % Executable (dispatched by interpreter)
(A)#b           % Convert 1-char string to Byte value (65b)
(hello)#=       % Eq-string (uses temporary storage)
(hello)#lw      % Explicit Literal + Writable (defaults)
```

### 3.4 Hex Strings

Hex strings encode raw bytes as hex digit pairs:

```
<48656C6C6F>     % "Hello"
<48 65 6C 6C 6F> % whitespace ignored
<>               % empty
<41>#b           % hex string to Byte (65b)
```

### 3.5 Raw Strings

Raw strings preserve backslashes literally:

```
<(C:\Users\data)>   % no escape processing: C:\Users\data
<(regex: \d+\.\d+)> % regex pattern without double escaping
```

### 3.6 Programmatic Creation

```
10 string                % 10 zero-filled bytes
42 32 string to-string   % => (42)
3.14 32 string to-string % => (3.14)
true 32 string to-string % => (true)
/foo 32 string to-string % => (foo)
```

---

## 4. Access and Modification

### 4.1 Length and Indexing

```
(hello) length           % => 5
(hello) 0 get            % => 104b  ('h')
(hello) 4 get            % => 111b  ('o')
(hello) -1 get           % => 111b  (negative index: last byte)
```

### 4.2 Byte Modification

`put` modifies a byte in place.  The string must be writable:

```
(hello) dup 0 72b put    % change 'h' to 'H' => (Hello)

(hello)#r dup 0 72b put  % ERROR: /read-only
```

**Important:** String byte writes via `put` are **not journaled** by
save/restore.  They persist across `restore`.  This is a deliberate
space trade-off (journaling a 1-byte write requires 12+ bytes of overhead).

### 4.3 Substrings

`get-interval` extracts a contiguous range:

```
(hello world) 6 5 get-interval    % => (world)
(hello world) 0 5 get-interval    % => (hello)
```

### 4.4 Concatenation

```
(hello) ( world) concat   % => (hello world)
(a) (b) concat (c) concat % => (abc)
```

### 4.5 Copy

`copy` writes bytes from source into destination:

```
(hello) 10 string copy            % => (hello)  % copy sets dest length = source length (5)
```

---

## 5. Search and Matching

### 5.1 search

`search` finds the first occurrence of a substring.  On success, it returns
three string segments and `true`:

```
(hello world) (world) search
% => () (world) (hello ) true
% Stack (top to bottom): true, (hello ), (world), ()
% pre = (hello ), match = (world), post = ()
```

On failure:
```
(hello) (xyz) search
% => (hello) false
```

### 5.2 string-index-of

Returns the byte index of the first occurrence, or -1:

```
(hello world) (world) string-index-of    % => 6
(hello) (xyz) string-index-of            % => -1
```

### 5.3 Prefix, Suffix, and Contains

```
(hello world) (hello) starts-with?       % => true
(hello world) (world) ends-with?         % => true
(hello world) (lo wo) contains?          % => true
(hello world) (xyz) contains?            % => false
```

### 5.4 Count Occurrences

```
(banana) (an) count-substring            % => 2 (non-overlapping)
(aaa) (aa) count-substring               % => 1 (non-overlapping)
```

---

## 6. Transformation

### 6.1 Case Conversion

```
(hello) uppercase        % => (HELLO)
(HELLO) lowercase        % => (hello)
(hello) capitalize       % => (Hello)  (first byte only)
```

### 6.2 Trimming

```
(  hello  ) trim         % => (hello)
(  hello  ) trim-left    % => (hello  )
(  hello  ) trim-right   % => (  hello)
(\t\n hello \n) trim     % => (hello)
```

### 6.3 Padding

```
(42) 6 32b pad-left      % => (    42)  (pad with spaces)
(42) 6 48b pad-left      % => (000042)  (pad with '0')
(hi) 10 46b pad-right    % => (hi........)  (pad with '.')
```

### 6.4 Replace

```
(hello world) (o) (0) replace           % => (hell0 w0rld)
(aabbcc) (bb) () replace                % => (aacc)  (delete every "bb")
```

`replace` replaces all non-overlapping occurrences of `old` with `new`.

### 6.5 Remove Prefix/Suffix

```
(hello world) (hello ) remove-prefix    % => (world)
(hello world) ( world) remove-suffix    % => (hello)
(hello) (xyz) remove-prefix             % => (hello)  (no change)
```

### 6.6 Repeat

```
(ab) 3 repeat-string     % => (ababab)
(-) 20 repeat-string     % => (--------------------)
```

### 6.7 Reverse

```
(hello) reverse           % => (olleh)
```

---

## 7. Splitting and Joining

### 7.1 split

Split a string by a delimiter, returning an array of strings:

```
(a,b,c) (,) split            % => [(a) (b) (c)]
(hello world) ( ) split      % => [(hello) (world)]
(one::two::three) (::) split % => [(one) (two) (three)]
(no-delim) (,) split         % => [(no-delim)]
```

### 7.2 join

Join an array of strings with a delimiter:

```
[(a) (b) (c)] (,) join              % => (a,b,c)
[(hello) (world)] ( ) join          % => (hello world)
[(one) (two) (three)] () join       % => (onetwothree)
```

### 7.3 chars

Split a string into an array of its bytes (Byte values, not 1-char strings):

```
(hello) chars    % => [104b 101b 108b 108b 111b]
```

### 7.4 Round-Trip: split then join

```
(a,b,c) (,) split (;) join     % => (a;b;c)
```

---

## 8. Regular Expressions

Trix uses POSIX Extended Regular Expressions (ERE).

### 8.1 regex-match

Tests whether the pattern matches anywhere in the string:

```
(hello123) ([0-9]+) regex-match      % => true
(hello) ([0-9]+) regex-match         % => false
```

Use anchors for full-string matching:

```
(hello) (^hello$) regex-match        % => true (exact match)
(hello world) (^hello$) regex-match  % => false
```

### 8.2 regex-search

Finds the first match with capture groups.  Returns an array where index 0
is the full match and 1+ are capture groups:

```
(age: 42) (([a-z]+): ([0-9]+)) regex-search
% => [(age: 42) (age) (42)] true

(no match) ([0-9]+) regex-search
% => false
```

### 8.3 regex-find-all

Returns an array of all matches.  Each match is an array (full match +
capture groups):

```
(a1b2c3) (([a-z])([0-9])) regex-find-all
% => [[(a1) (a) (1)] [(b2) (b) (2)] [(c3) (c) (3)]]
% NB: the whole pattern is ONE string -- wrap it in a single outer paren,
% else ([a-z]) ([0-9]) scans as two separate string literals.

(hello) ([0-9]) regex-find-all
% => []  (empty array, no matches)
```

### 8.4 regex-replace

Replaces all non-overlapping matches:

```
(hello 123 world 456) ([0-9]+) (NUM) regex-replace
% => (hello NUM world NUM)

(hello) ([0-9]+) (NUM) regex-replace
% => (hello)  (no matches, unchanged)
```

### 8.5 regex-split

Splits the string at pattern matches:

```
(one42two99three) ([0-9]+) regex-split
% => [(one) (two) (three)]

(hello) ([0-9]+) regex-split
% => [(hello)]  (no matches, one segment)
```

### 8.6 POSIX ERE Quick Reference

```
.           any byte
[abc]       character class
[^abc]      negated class
[a-z]       range
^           start of string
$           end of string
*           zero or more
+           one or more
?           zero or one
{n}         exactly n
{n,}        n or more
{n,m}       between n and m
|           alternation
(...)       capture group
```

---

## 9. Character Predicates

Character predicates accept either a Byte or a String.  For strings, they
return `true` if and only if the string is non-empty and every byte matches
the predicate:

```
65b digit?          % => false ('A')
48b digit?          % => true  ('0')
(123) digit?        % => true  (all digits)
(12a) digit?        % => false (contains non-digit)
() digit?           % => false (empty string)
```

### Full List

| Predicate    | Matches           | ASCII Range          |
| ------------ | ----------------- | -------------------- |
| `digit?`     | 0-9               | 48-57                |
| `alpha?`     | a-z, A-Z          | 65-90, 97-122        |
| `alnum?`     | a-z, A-Z, 0-9     | 48-57, 65-90, 97-122 |
| `hex-digit?` | 0-9, A-F, a-f     | 48-57, 65-70, 97-102 |
| `lower?`     | a-z               | 97-122               |
| `upper?`     | A-Z               | 65-90                |
| `space?`     | HT LF VT FF CR SP | 9-13, 32             |
| `printable?` | visible ASCII     | 32-126               |

### Usage Patterns

```
% Validate input
(hello123) alnum?        % => true
(hello 123) alnum?       % => false (space is not alnum)

% Filter characters from a string.  chars yields bytes, so box each kept
% byte into a 1-char string before join (join requires string elements).
(Hello, World! 123) chars { alpha? } filter
{ 1 string dup 0 4 -1 roll put } map () join
% => (HelloWorld)
```

---

## 10. Format Strings

### 10.1 Output Formatting (print-fmt / sprint-fmt / fprint-fmt)

Format strings use `{}` placeholders.  Arguments are passed via mark or
array:

```
% Basic: format string first, then mark + args above it.
% {0} is the first arg above the mark, {1} the second.
({1} scored {0}\n) mark 42 (Alice) print-fmt pop pop
% prints: Alice scored 42

% Default indexing: arguments numbered from bottom of mark
({}\n) mark 42 print-fmt pop pop
% prints: 42
```

### 10.2 Format Specifiers

```
{0}         % default format
{0:d}       % integer (decimal)
{0:x}       % integer (hex)
{0:o}       % integer (octal)
{0:b}       % integer (binary)
{0:f}       % floating point
{0:e}       % scientific notation
{0:g}       % general (auto f or e)
{0:s}       % string
{0:c}       % character (byte as char)
{0:O}       % object detailed form
{0:#s}      % object alt form
```

### 10.3 Width, Precision, and Flags

```
{0:10d}      % width 10, right-aligned
{0:<10d}     % width 10, left-aligned
{0:06d}      % width 6, zero-padded
{0:+d}       % show sign for positive
{0:.2f}      % 2 decimal places
{0:10.5f}    % width 10, precision 5
```

### 10.4 Format to String

```
=string ({} + {} = {}) mark 3 4 7 sprint-fmt pop
% => (3 + 4 = 7)
```

### 10.5 Array-Argument Variants

```
({}: {}\n) [(Alice) 42] aprint-fmt pop pop
% prints: Alice: 42
```

### 10.6 Input Parsing (sscan-fmt)

`sscan-fmt` parses structured text.  Stack effect:
```
src-str fmt-str mark arg1 ... argN -- arg1 ... argN count
```

The target slots above the mark receive the parsed values.  Their declared
types act as implicit type suffixes for unsuffixed input; strict type safety
is enforced (no lossy implicit conversion).  A `null` slot auto-detects
whatever natural type the input parses as.

```
(42 3.14 hello ) ({} {} {}) mark 0 0. 128 string sscan-fmt
% stack: 42 3.14 (hello) 3
% 3 values parsed: Integer 42, Real 3.14, String (hello)
```

### 10.7 Formatted String Building

```
% Build a table row
/format-row {
    % name score -- str
    |name score|
    =string ({:-20s} {:>5d}\n) mark name score sprint-fmt pop
} def

(Alice) 95 format-row    % => (Alice                   95\n)
```

---

## 11. Scan-Time String Interpolation

Inside `(...)` strings, `\{name}` performs immediate name lookup at scan
time (not runtime) and splices the value as text:

```
/x 42 def
(The answer is \{x})     % => (The answer is 42)
```

### 11.1 Dict Path Interpolation

```
(Pi is \{:systemdict:numbers:real-type:pi})
% => (Pi is 3.1415927)
```

### 11.2 All Types Supported

Any Trix type can be interpolated.  Values are formatted using the same
output as `=`:

```
/arr [1 2 3] def
(Array: \{arr})           % => (Array: array)

/flag true def
(Flag: \{flag})           % => (Flag: true)
```

### 11.3 Scan-Time vs Runtime

`\{name}` resolves at scan time (like `//`).  The value is captured when
the scanner processes the string literal, not when the string is used:

```
/x 10 def
/msg (x is \{x}) def     % msg = (x is 10)
/x 20 def
msg =                      % prints: x is 10  (not 20)
```

For runtime string building, use `sprint-fmt` instead.

---

## 12. String Comparison

### 12.1 Value Equality

Strings are compared by content with `eq` and `ne`:

```
(hello) (hello) eq       % => true (same content)
(hello) (world) eq       % => false
(hello) (Hello) eq       % => false (case-sensitive)
```

### 12.2 Lexicographic Ordering

```
(apple) (banana) lt % => true (a < b)
(hello) (hello) le  % => true (equal)
(xyz) (abc) gt      % => true (x > a)
```

### 12.3 Deep Equality

For strings, `deep-eq` is equivalent to `eq` (strings have no nested
structure):

```
(hello) (hello) deep-eq   % => true
```

---

## 13. Type Conversion

### 13.1 to-string

Converts any value to its string representation:

```
42 32 string to-string             % => (42)
3.14 32 string to-string           % => (3.14)
true 32 string to-string           % => (true)
null 32 string to-string           % => (null)
/foo 32 string to-string           % => (foo)
[1 2 3] 64 string to-string        % => (array)
```

### 13.2 to-name

Converts a string to a name:

```
(hello) to-name          % => /hello
```

### 13.3 to-number

Parses a string as a number:

```
(42) to-number           % => 42
(3.14) to-number         % => 3.14
```

### 13.4 token

Parses a single Trix value from a string, returning the remainder:

```
(42 hello) token         % => ( hello) 42 true
( hello) token           % => () /hello true  (name)
() token                 % => false (empty, nothing to parse)
```

---

## 14. Save/Restore Behavior

String **allocation** is rolled back by restore: strings created after a save
point are discarded when the heap pointer is reset.

String **byte modifications** via `put` are **not journaled**.  They persist
across restore.  This is a deliberate design trade-off:

```
/s (hello) def
save
    s 0 72b put           % change 'h' to 'H'
restore
s =                        % prints: Hello  (modification persisted!)
```

`save` pushes a save-token Integer and `restore` consumes it, so keep the
working value in a name (not on the operand stack) across the save/restore.
To get journaled byte data, use an array of Byte values (8x memory cost):

```
[ 104b 101b 108b 108b 111b ] /a exch def    % [h e l l o] as byte array
save /tok exch def                           % save -> token (kept in tok)
a 0 72b put                                  % change first byte to 'H'
tok restore                                  % roll back
a 0 get ==                                   % => 104b (rolled back to 'h')
```

Or allocate the string after the save point, so it is discarded entirely:

```
save /tok exch def               % save -> token (kept in tok)
(hello) 0 72b put                % string allocated AFTER the save
tok restore                      % heap rolled back: the string is discarded
```

---

## 15. Real-World Patterns

### 15.1 CSV Parser

```
/parse-csv {
    % str -- array-of-arrays
    (\n) split
    { (,) split { trim } map } map
} def

(name,age,city
Alice,30,NYC
Bob,25,LA) parse-csv
% => [[(name) (age) (city)] [(Alice) (30) (NYC)] [(Bob) (25) (LA)]]
```

### 15.2 Key-Value Config Parser

```
/parse-config {
    % str -- dict
    /acc << >> def
    (\n) split {
        dup length 0 eq { pop } {
        dup (#) starts-with? { pop } {
            (=) split
            dup length 2 ge {
                dup 0 get trim to-name      % key (name)
                exch 1 get trim             % value (string)
                acc 3 1 roll put            % acc key value put
            } { pop } if-else
        } if-else
        } if-else
    } for-all
    acc
} def

(# config file
timeout=30
name=Alice
debug=true) parse-config
% => << /timeout (30) /name (Alice) /debug (true) >>
```

### 15.3 Template Engine

```
/render-template {
    % template-str dict -- result-str
    /vars exch def
    vars {
        % key value -- ; replace {key} with value in the template on the stack
        /val exch def
        /key exch def
        ({) key 32 string to-string concat (}) concat   % build placeholder: {key}
        val
        replace
    } for-all
} def

(Hello {name}, you have {count} messages.)
<< /name (Alice) /count (5) >>
render-template
% => (Hello Alice, you have 5 messages.)
```

### 15.4 URL Query String Parser

```
/parse-query {
    % str -- dict
    /acc << >> def
    (&) split {
        (=) split
        dup length 2 ge {
            dup 0 get exch 1 get        % key value
            acc 3 1 roll put            % acc key value put
        } { pop } if-else
    } for-all
    acc
} def

(name=Alice&age=30&city=NYC) parse-query
% => << (name) (Alice) (age) (30) (city) (NYC) >>
```

### 15.5 Text Statistics

```
/text-stats {
    % str -- dict
    |txt|
    txt length /bytes exch def
    txt (\n) split length /lines exch def
    txt ( ) split length /words exch def
    txt chars { alpha? } filter length /letters exch def
    txt chars { digit? } filter length /digits exch def
    << /bytes bytes /lines lines /words words
       /letters letters /digits digits >>
} def

(Hello World 123\nSecond line) text-stats
% => << /bytes 27 /lines 2 /words 4 /letters 20 /digits 3 >>
```

### 15.6 Log Line Parser

```
/parse-log-line {
    % str -- dict
    % Format: [LEVEL] timestamp message
    |line|
    line (]) string-index-of /rb exch def               % index of ]
    line 1 rb 1 sub get-interval /level exch def        % between [ and ]
    line rb 2 add line length rb 2 add sub get-interval % drop "] "
    /rest exch def
    rest ( ) string-index-of /sp exch def
    rest 0 sp get-interval /ts exch def
    rest sp 1 add rest length sp 1 add sub get-interval /msg exch def
    << /level level /timestamp ts /message msg >>
} def

([ERROR] 2026-03-29 Connection failed) parse-log-line
% => << /level (ERROR) /timestamp (2026-03-29) /message (Connection failed) >>
```

### 15.7 Slug Generator

```
/slugify {
    % str -- str
    lowercase
    chars
    % keep alphanumerics, replace everything else with "-".
    % chars yields bytes, so the keep branch boxes the byte into a
    % 1-char string -- join needs every element to be a string.
    { dup alnum?
        { 1 string dup 0 4 -1 roll put }   % byte -> (c)
        { pop (-) }
        if-else } map
    () join
    % collapse multiple dashes
    { dup (--) contains? } { (--) (-) replace } while
    (-) remove-prefix
    (-) remove-suffix
} def

(Hello, World! This is a Test.) slugify
% => (hello-world-this-is-a-test)
```

### 15.8 Regex-Based Field Extraction

```
/extract-emails {
    % str -- array
    % Raw string <( ... )> -- the outer ( ) are the raw-string delimiters,
    % so backslashes (\.) reach the regex verbatim.  The inner ( ) is the
    % capture group, so regex-find-all yields [full-match ...] per hit and
    % { 0 get } picks out the whole address.
    <(([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]+))> regex-find-all
    { 0 get } map
} def

(Contact alice@example.com or bob@test.org for info) extract-emails
% => [(alice@example.com) (bob@test.org)]
```

### 15.9 String Validation

```
/valid-identifier? {
    % str -- bool
    dup length 0 eq { pop false } {
        dup 0 get dup alpha? exch 95b eq or         % starts with letter or _
        { chars { dup alnum? exch 95b eq or } all } % rest is alnum or _
        { pop false }
        if-else
    } if-else
} def

(hello_world) valid-identifier?     % => true
(123abc) valid-identifier?          % => false
(_private) valid-identifier?        % => true
```

### 15.10 Binary Data as Hex Dump

```
/hex-dump {
    % str -- (prints hex dump)
    |data|
    data length /len exch def
    0 16 len {
        |offset|
        % Print offset
        =string ({:04x}: ) mark offset sprint-fmt pop print

        % Print hex bytes
        0 1 15 {
            |j|
            offset j add len lt
            { =string ({:02x} ) mark data offset j add get sprint-fmt pop print }
            { (   ) print }
            if-else
        } for

        % Print ASCII
        (|) print
        0 1 15 {
            |j|
            offset j add len lt
            { =string ({:c}) mark
              data offset j add get dup printable? { } { pop 46b } if-else
              sprint-fmt pop print }
            if
        } for
        (|) print nl
    } for
} def
```

---

## 16. Implementation Details

This section covers the C++ internals of the string system for Trix
maintainers and contributors.

### 16.1 String VM Allocation

Strings are allocated in the VM heap as contiguous byte arrays with a hidden
NUL terminator for C interoperability:

```
VM Heap:
  offset -> [ byte0 byte1 ... byteN-1 NUL ]
            |<---- m_string_length ---->|  +1 for NUL
```

Allocation uses `vm_start_alloc<vm_t>()` for uninitialized space, copies the
string data, writes the NUL terminator, then `vm_end_alloc<vm_t>(length + 1)`
to finalize. The NUL is not included in the string's length.

### 16.2 String Object Layout

A String is an 8-byte Object:

```
Object {
    m_aat:            LiteralAttrib | Type::String  (1 byte)
    m_object_save_level:                             (1 byte)
    m_string_length:  length_t                       (2 bytes, max 65535)
    m_string:         vm_offset_t                    (4 bytes, offset to data)
};
```

Created via `Object::make_string(offset, length)`. The maximum string length
is 65,535 bytes (16-bit `length_t`).

### 16.3 Byte Access Implementation

**`get` on strings:** Returns the byte at the given index. Supports negative
indexing (-1 = last byte). The implementation accesses the VM heap via
`string_vptr(trx)` which returns a pointer to the byte array, then indexes
directly: `data[index]`. O(1) access.

**`put` on strings:** Writes a byte at the given index. Bounds-checked.
Accesses the byte array via `string_vptr(trx)` and writes directly:
`string_ptr[index] = b`. **Not journaled by save/restore** -- byte writes
persist across `restore`. This is a deliberate space trade-off: journaling
a 1-byte write would require 12+ bytes of journal overhead.

**`get-interval` on strings:** Returns a VIEW (alias) into the source string --
it adjusts the offset (`m_string`) and length (`m_string_length`) of a copied
Object header but shares the original's VM byte storage; it does NOT allocate or
copy. Byte writes (`put`) through the sub-string are therefore visible in the
source. (The same aliasing applies to `take`, `drop`, and `trim`.)

### 16.4 String Copy Semantics

Strings are **not** copy-on-write. The allocating operators (`concat`,
`uppercase`, `lowercase`, `capitalize`, `replace`) build a fresh string in the
VM heap. The interval/sub-string operators (`get-interval`, `take`, `drop`,
`trim`) instead return a VIEW that SHARES the source's VM bytes (offset and
length adjusted in place), so a `put` through the view mutates the original.
`copy` writes into a caller-supplied destination string rather than allocating.

`concat` allocates a new string of `length1 + length2` bytes, copies both
sources, and appends a NUL terminator. The operand strings are unchanged.

### 16.5 Regex Implementation

Trix uses POSIX ERE via the system `<regex.h>` library (`regcomp()` /
`regexec()` / `regfree()`). An RAII wrapper ensures `regfree()` is called
when the compiled pattern goes out of scope.

Patterns are compiled on each call -- there is no regex cache. For
performance-critical code with repeated pattern use, the host can register
a pre-compiled regex as a user operator.

`regex-search` and `regex-find-all` use `REG_EXTENDED` mode. Capture groups
are extracted via `regmatch_t` offset pairs and converted to Trix string
substrings.

### 16.6 String Formatting (PrintFmt)

The `sprint-fmt` / `print-fmt` / `fprint-fmt` operators use the `PrintFmt`
class, which parses `{N:fmt}` placeholders and dispatches to type-specific
formatters. The format engine:

1. Scans the format string for `{` delimiters
2. Parses the argument index and optional format specifier
3. Dispatches to the appropriate `print_*` method based on the operand's type
4. Writes output to stdout (print-fmt), a stream (fprint-fmt), or a string
   buffer (sprint-fmt)

The `sscan-fmt` operator uses the `ScanFmt` class, which parses the same
placeholder syntax but reads input from a source string, converting text to
typed Trix values.
