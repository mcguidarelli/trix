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
-->

# Trix Format String Cheat Sheet

Trix format strings use a **Python-PEP-3101 subset** (`str.format`-style)
augmented with C-printf type letters and Trix-specific types.  Same
grammar works in three directions:

  * **`print-fmt` family** — format Trix values into output (stdout,
    a Stream, or a String buffer).
  * **`scan-fmt` family** — parse formatted input from a String or
    Stream back into typed values.
  * **String interpolation** — `\{name}` inside a `(...)` string
    literal splices the named value at scan time, formatted via the
    default object form.

---

## 1. Field grammar

A format string is plain text plus zero or more **replacement fields**:

```
{[arg-id][:format-spec]}
```

  * `{N}`  — explicit positional argument id (0-based).
  * `{}`   — implicit; auto-increments.  Cannot be mixed with `{N}`.
  * `{{`   — a literal `{`.
  * `}}`   — a literal `}`.

The format spec inside the colon follows Python's mini-language:

```
[[fill]align][sign][#][0][width][.precision][type]
```

Width and precision can each be a nested `{N}` (read from the
argument list at runtime).

### Field examples

```trix
({:d} of {:d}) [ 7 10 ] aprint-fmt           % "7 of 10"
({:0>4d}) [ 42 ] aprint-fmt                  % "0042" -- pad with '0', right-align
({:<10s}|) [ (hi) ] aprint-fmt               % "hi        |"
({:.{}f}) [ 3.14159 2 ] aprint-fmt           % "3.14" -- nested precision from args
({0:d} {1:s} {0:x}) [ 255 (val) ] aprint-fmt % "255 val ff"
```

---

## 2. Alignment, fill, sign, alt, zero-pad, width, precision

| Segment | Spec | Meaning |
| --- | --- | --- |
| Fill+align | `<` | Left-align (default for strings) |
|  | `>` | Right-align (default for numbers) |
|  | `^` | Centre |
| Fill char | any-char before `<` / `>` / `^` | Padding character (default: space) |
| Sign | `+` | Always show sign on numbers (`+42`) |
|  | `-` | Show only `-` (default) |
|  | ` ` | Space for positive (` 42`) |
| Alt form | `#` | Add `0x` / `0o` / `0b` prefix on integers; force decimal point on floats (e.g. `1.0` not `1`) |
| Zero pad | `0` | Pad numbers with `0` instead of space; equivalent to `0>` with sign before pad |
| Width | `N` or `{i}` | Minimum field width in characters; pads as needed |
| Precision | `.N` or `.{i}` | Floats: digits after decimal point.  Strings: max chars.  Integers: sets the output radix base 2..36 (prints `BASE#digits`, e.g. `{:.2d}` of 42 -> `2#101010`) |

---

## 3. Type characters (`print-fmt` family)

### Integers — `int`, `uint`, `byte`

| Type | Output |
| --- | --- |
| *(none)* / `d` | Decimal |
| `x` | Hex lowercase (`ff`) |
| `X` | Hex uppercase (`FF`) |
| `o` | Octal |
| `b` | Binary |
| `c` | As single character (interpreted as codepoint) |
| `#d` *(alt form)* | Decimal; `#` has no effect (no prefix, no sign change) -- use the `+`/` ` sign flag to force a sign |
| `#x` / `#X` / `#o` / `#b` | With `0x` / `0X` / `0o` / `0b` prefix |

### Floats — `real`, `double`

| Type                          | Output                                    |
| ----------------------------- | ----------------------------------------- |
| *(none)* / `g`                | Shortest of fixed/scientific              |
| `G`                           | Like `g`, uppercase exponent              |
| `f`                           | Fixed-point (`3.14`)                      |
| `F`                           | Like `f`; `inf` → `INF`, `nan` → `NAN`    |
| `e`                           | Scientific (`3.14e+00`)                   |
| `E`                           | Like `e`, uppercase exponent              |
| `#g` / `#f` etc. *(alt form)* | Always show decimal point (`1.0` not `1`) |

### Strings — `str`

| Type | Output |
| --- | --- |
| *(none)* / `s` | The string contents as-is |
| `?` | Printable-escape form: graphic chars as-is, named escapes (`\t`, `\n`) / caret notation (`\^A`) for control chars (useful for logging untrusted input) |
| `O` | Object form — quoted with delimiters and escapes |
| `x` | Hex dump of the bytes |
| Precision (`.N`) | Truncate to N characters |

### Booleans

| Type           | Output                                   |
| -------------- | ---------------------------------------- |
| *(none)* / `s` | `true` or `false`                        |
| `d`            | `1` or `0`                               |
| `c`            | `t` or `f` (single character)            |
| `O`            | `--true--` or `--false--` (pstack-style) |

### Bytes — `byte`

| Type | Output |
| --- | --- |
| *(none)* / `d` | Decimal |
| `x` / `X` | Hex lowercase / uppercase |
| `c` | As single character |
| `?` | Printable-escape form: graphic bytes as-is, named escapes (`\t`, `\n`, `\r`, ...) for common controls, caret notation (`\^A`, `\^?`) for the rest |
| `o` `b` | Octal / binary |

### Trix-specific

| Type | Output |
| --- | --- |
| `T` | Type tag (the `pstack`-style tag, e.g. `--integer--`, `--string--`, `--real--`).  Works for any type.  For the bare type-name use the `type` op + `{:s}` |
| `O` | Object form — same as `==` debug print (e.g. `42i`, `(hi)`, `[ 1 2 3 ]`).  Works for any type |
| `I` *(ULong only)* | Chrono instant — value is ms-since-1970-UTC.  Format body is a `strftime` template terminated by `}` |
| `Il` | Like `I` but format in **local time zone** instead of UTC |
| `D` *(UInteger only)* | Calendar date (udate) — `strftime` template body terminated by `}` (no `Dl` variant) |

### Chrono instant examples

```trix
% ULong holding ms-since-1970 -> ISO-8601 in UTC
(now: {:I%Y-%m-%dT%H:%M:%SZ}) [ 1700000000000ul ] aprint-fmt =
% -> "now: 2023-11-14T22:13:20Z"

% Same value in local zone (output depends on the host time zone)
(local: {:Il%Y-%m-%d %H:%M:%S}) [ 1700000000000ul ] aprint-fmt =
% -> e.g. "local: 2023-11-14 17:13:20"
```

The `strftime` body inside the spec runs until the closing `}`.  Use
`}}` to embed a literal `}`.

---

## 4. Type characters (`scan-fmt` family)

`sscan-fmt` / `fscan-fmt` parse formatted text **back** into typed
values.  Type set is narrower than `print-fmt`:

| Type | Reads | Target slot type |
| --- | --- | --- |
| *(none)* | Auto-detect (number, address, boolean, or whitespace-delimited string) | Numeric / Address / Boolean / String |
| `d` | Decimal integer (optional sign) | `int` / `uint` |
| `x` | Hex integer (optional `0x` prefix) | `int` / `uint` (or `byte` with `c`-mode escape) |
| `f` | Floating point | `real` / `double` |
| `s` | Word — up to width or next whitespace; also accepts `true`/`false` for booleans | `str` / `bool` |
| `c` | Single character | `byte` / `str` |
| `?` | Printable-escape form — graphic chars or `\t`/`\n`/`\xNN` escapes | `byte` / `str` (not boolean) |
| `D <strftime>` | Calendar date via a `strftime` template body (required, like `I`) | `uinteger` |
| `I[l] <strftime>` | Chrono instant via strftime template | `ulong` (ms since 1970 UTC) |

**Whitespace in the format string** matches zero or more whitespace
characters in the input.  **Literal characters** must match exactly
(`/scan-match-fail` on mismatch).

### Scan examples

```trix
% Parse "42 hello 3.14" -> int, string, real
(42 hello 3.14)  ({:d} {:s} {:f})  mark 0 (        ) 0.0  sscan-fmt
% -> 42, "hello", 3.14 left in the mark slots; count (3) on top

% Parse ISO-8601 timestamp -> ULong ms
(2023-11-14T22:13:20Z) ({:I%Y-%m-%dT%H:%M:%SZ}) mark 0ul sscan-fmt
% -> 1700000000000ul, count=1
```

Scan errors:

| Error                    | Cause                                                        |
| ------------------------ | ------------------------------------------------------------ |
| `/scan-match-fail`       | Literal character in format didn't match input               |
| `/scan-input-fail`       | Hit EOF before all fields scanned                            |
| `/scan-type-fail`        | Input is not a valid number for the target type (e.g. `abc`) |
| `/numerical-overflow`    | Parsed value is out of range for the target type             |
| `/scan-type-mismatch`    | Argument slot type incompatible with format type             |
| `/scan-duplicate-arg-id` | Same `{N}` referenced twice                                  |

See [`errors-cheatsheet.md`](errors-cheatsheet.md) for recovery patterns.

---

## 5. The operator family

| Op            | Stack effect                          | Args shape                              |
| ------------- | ------------------------------------- | --------------------------------------- |
| `print-fmt`   | `fmt mark arg* -- int bool`           | Write to stdout                         |
| `fprint-fmt`  | `stream fmt mark arg* -- int bool`    | Write to a Stream                       |
| `sprint-fmt`  | `dst-str fmt mark arg* -- str bool`   | Write into a pre-allocated string       |
| `aprint-fmt`  | `fmt arr -- int bool`                 | Args from an Array, write to stdout     |
| `afprint-fmt` | `stream fmt arr -- int bool`          | Args from an Array, write to a Stream   |
| `asprint-fmt` | `dst-str fmt arr -- str bool`         | Args from an Array, write into a string |
| `sscan-fmt`   | `input-str fmt mark arg* -- arg* int` | Scan from a string                      |
| `fscan-fmt`   | `stream fmt mark arg* -- arg* int`    | Scan from a Stream                      |

The mark-based forms (`print-fmt` / `sprint-fmt`) read args from the
operand stack above the most recent mark.  The array forms
(`aprint-fmt` / `asprint-fmt`) take a packed array — convenient for
forwarding from a proc that already has its args boxed.

The `print`-family return values:
  * `int bool` — output count + success flag.  `bool=false` means
    overflow occurred (some output was dropped, only for the
    sized-string variants).
  * `str bool` for the `sprint`-family — the produced string + success.

The two `sscan-fmt`/`fscan-fmt` ops write results back into the
argument slots above the mark — pre-allocate the slots first
(`mark 0 0.0 (...)` etc.), then read out after the call.  Returns
the count of successfully scanned fields on top.

---

## 6. String interpolation

Inside a `(...)` string literal, **`\{name}`** triggers scan-time
interpolation.  The Name is looked up immediately (like `//name`)
and its value is spliced in as text using the default object form
(same shape as `=`).

```trix
/x 42 def
(x is \{x}) =                  % "x is 42"
(x is \{:userdict:x}) =        % dict path -- "x is 42"
```

  * Works only inside escape-aware `( ... )` strings (not `<( ... )>`
    raw strings and not `< ... >` hex strings).
  * The name must be already-defined at scan time; a missing name is
    an `/undefined` raised at scan time (in the scanner, not at run time).
  * A bare `{` is already literal; only the `\{name}` sequence triggers
    interpolation.  To emit a literal backslash-brace use `\\{`.
  * No format spec — always the default object form.  Use
    `print-fmt`/`sprint-fmt` when you need width, precision, or
    type-specific formatting.

---

## 7. Common recipes

### Right-aligned table column

```trix
({:>10d}|) [ 42 ] aprint-fmt    % "        42|"
```

### Hex dump byte

```trix
({:02X}) [ 255b ] aprint-fmt    % "FF"
({:#04x}) [ 16b ] aprint-fmt    % "0x10" (alt form adds prefix)
```

### Floating-point with fixed precision

```trix
({:.4f}) [ 3.14159 ] aprint-fmt    % "3.1416"
({:8.2f}) [ 3.14159 ] aprint-fmt   % "    3.14" (width 8, precision 2)
```

### Scientific notation

```trix
({:.3e}) [ 1234567.0 ] aprint-fmt  % "1.235e+06"
```

### Type-tagged debug output

```trix
% Print the pstack-style type tag and the debug form for any value
% Use {0:T} {0:O} to refer to the same arg twice (otherwise auto-increment
% advances and you get /range-check):
({0:T} {0:O}) [ 42 ] aprint-fmt   % "--integer-- 42i"
({0:T} {0:O}) [ (hi) ] aprint-fmt % "--string-- (hi)#lw" (object form carries the container type-suffix)
```

### Truncate long string

```trix
({:.5s}) [ (Hello, world!) ] aprint-fmt    % "Hello"
({:>10.5s}) [ (Hello, world!) ] aprint-fmt % "     Hello"
```

### Nested width/precision from args

```trix
({:{}.{}f}) [ 3.14159 8 3 ] aprint-fmt % "   3.142" (width=8, prec=3)
```

### Parse a CSV row

`{:s}` reads up to the next whitespace, so it eats embedded commas — a
single `sscan-fmt` over a comma-delimited row does not work.  Split on
the comma first, then scan the typed fields:

```trix
(42,Alice,3.5) (,) split  /fields exch def                % -> [ (42) (Alice) (3.5) ]
fields 0 get ({:d}) mark 0   sscan-fmt pop /n    exch def % int  42
fields 1 get                         /name exch def       % string (Alice)
fields 2 get ({:f}) mark 0.0 sscan-fmt pop /x    exch def % real 3.5
(msg) n 42 eq assert
(msg) name (Alice) eq assert
(msg) x 3.5 eq assert
```

### Parse a date

```trix
(2023-11-14) ({:I%Y-%m-%d}) mark 0ul sscan-fmt
% -> ms-since-1970-UTC for 2023-11-14T00:00:00Z; count=1
```

---

## 8. Errors

Format-string errors raise `/invalid-format-string` at runtime.
Common causes:

| Cause | Example |
| --- | --- |
| Unmatched `{` | `({)` |
| Unmatched `}` | `(})` |
| Invalid type letter | `({:z})` |
| Width / precision spec with wrong shape | `({:.})` |
| Argument id out of range (raises `/range-check`, not `/invalid-format-string`) | `({2})` with two args (max id = 1) |
| Type mismatch (e.g. `:d` on a string) | `({:d}) [ (hi) ]` |
| Mixing implicit `{}` and explicit `{N}` in the same format string | `({} {0})` |

Catch with `try`:

```trix
{ ({:z}) [ 1 ] aprint-fmt } try /invalid-format-string eq =       % true
{ ({:d}) [ (str) ] aprint-fmt } try /invalid-format-string eq =   % true
```

---

## 9. Comparison with C printf / Python format

| Concept | C printf | Python format | Trix |
| --- | --- | --- | --- |
| Positional refs | `%d %s %d` | `{0} {1} {0}` | `{0} {1} {0}` |
| Auto-increment | n/a | `{} {}` | `{} {}` |
| Width | `%10d` | `{:10d}` | `{:10d}` |
| Precision | `%.4f` | `{:.4f}` | `{:.4f}` |
| Alignment | `%-10s` | `{:<10}` / `{:>10}` / `{:^10}` | `{:<10}` / `{:>10}` / `{:^10}` |
| Fill char | n/a | `{:*<10}` | `{:*<10}` |
| Alt form prefix | `%#x` | `{:#x}` | `{:#x}` |
| Type name | n/a | n/a | `{:T}` |
| Object form | n/a | `{!r}` | `{:O}` |
| Boolean | n/a | `{}` (Python: `True`/`False`) | `{:s}` (`true`/`false`); `{:d}` for `1`/`0` |
| Chrono | `strftime` separate call | `f"{dt:%Y-%m-%d}"` | `{:I%Y-%m-%d}` |
| Nested width | `%*d` | `{:{width}}` | `{:{}d}` |

---

## Where to go next

  * **Per-op stack effects:** [`trix-reference.md`](trix-reference.md) § 3.19.
  * **Error-name catalog + recovery patterns:** [`errors-cheatsheet.md`](errors-cheatsheet.md).
  * **Print engine internals (maintainer-side):** `src/printfmt.inl`.
  * **Scan engine internals (maintainer-side):** `src/scanfmt.inl`.
