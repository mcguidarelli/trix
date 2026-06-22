<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Z-Machine Programming and Implementation Manual

A complete reference for the Z-Machine virtual machine emulated by
[`examples/zmachine.trx`](zmachine.trx) and the story-file
catalog under [`examples/zmachine/`](zmachine/CATALOG.md) (the catalog
and fetch tooling ship with Trix; no story file is bundled).

This manual is self-contained: read it end-to-end and you will come
away with a working mental model of the Z-Machine -- the VM Infocom
designed in 1979 to ship one game across a dozen incompatible home
computers -- plus a guided tour of how this implementation maps the
spec into ~7900 lines of Trix.  No prior background assumed.

## Table of Contents

- [1. The Z-Machine in 60 Seconds](#1-the-z-machine-in-60-seconds)
- [2. The Story File](#2-the-story-file)
  - [2.1 Header Layout](#21-header-layout)
  - [2.2 Version Variants V1-V8](#22-version-variants-v1-v8)
- [3. Memory Map](#3-memory-map)
  - [3.1 Three Zones](#31-three-zones)
  - [3.2 Packed Addresses](#32-packed-addresses)
  - [3.3 The 16-bit Address Wrap (Spec §1.1.3)](#33-the-16-bit-address-wrap-spec-113)
  - [3.4 Banking for Stories Larger than 64K](#34-banking-for-stories-larger-than-64k)
- [4. ZSCII Text Encoding](#4-zscii-text-encoding)
  - [4.1 Z-Characters and the Three Alphabets](#41-z-characters-and-the-three-alphabets)
  - [4.2 Word Packing and the End-of-String Bit](#42-word-packing-and-the-end-of-string-bit)
  - [4.3 Abbreviations](#43-abbreviations)
  - [4.4 The 10-bit ZSCII Escape](#44-the-10-bit-zscii-escape)
- [5. The Object Tree](#5-the-object-tree)
  - [5.1 V3 Format (4-byte body, 32 attributes)](#51-v3-format-4-byte-body-32-attributes)
  - [5.2 V4+ Format (14-byte body, 48 attributes)](#52-v4-format-14-byte-body-48-attributes)
  - [5.3 Property Tables](#53-property-tables)
- [6. Variables, Locals, Stack](#6-variables-locals-stack)
- [7. Opcode Encoding Forms](#7-opcode-encoding-forms)
- [8. Instruction Set Reference](#8-instruction-set-reference)
  - [8.1 Arithmetic and Logic](#81-arithmetic-and-logic)
  - [8.2 Branching](#82-branching)
  - [8.3 Object Operations](#83-object-operations)
  - [8.4 Print and Output](#84-print-and-output)
  - [8.5 Input](#85-input)
  - [8.6 Routine Call and Return](#86-routine-call-and-return)
  - [8.7 Save, Restore, Undo](#87-save-restore-undo)
  - [8.8 Windows and Cursor (V4+)](#88-windows-and-cursor-v4)
  - [8.9 Color and Style (V5+)](#89-color-and-style-v5)
- [9. Windows and Display](#9-windows-and-display)
  - [9.1 V3 Status Line](#91-v3-status-line)
  - [9.2 V4+ Split Window](#92-v4-split-window)
  - [9.3 Output Stream Routing](#93-output-stream-routing)
- [10. Save, Restore, and Undo](#10-save-restore-and-undo)
  - [10.1 On-Disk Save / Restore](#101-on-disk-save--restore)
  - [10.2 The Manual Snapshot System (save_undo)](#102-the-manual-snapshot-system-save_undo)
  - [10.3 Named Save Slots](#103-named-save-slots)
- [11. Compliance Suites](#11-compliance-suites)
- [12. CLI and Showcase Commands](#12-cli-and-showcase-commands)
  - [12.1 Command-Line Flags](#121-command-line-flags)
  - [12.2 Showcase Slash-Commands](#122-showcase-slash-commands)
- [13. Implementation Tour](#13-implementation-tour)
  - [13.1 File Map](#131-file-map)
  - [13.2 Key Data Structures](#132-key-data-structures)
  - [13.3 Design Decisions](#133-design-decisions)
- [14. Further Reading](#14-further-reading)

---

## 1. The Z-Machine in 60 Seconds

In 1979, Infocom faced a packaging problem.  They had written a 1MB
MDL/Lisp engine for *Zork* on a PDP-10; to sell it on home computers
with 32 KB of RAM and a dozen incompatible architectures, they did
something clever -- they invented a **virtual machine** specifically
for text adventures, compiled their game scripts down to its bytecode,
and wrote a tiny interpreter for each target platform.  The bytecode
file (the **story file**) was identical across machines; only the
interpreter changed.

The Z-Machine ("ZIP" in Infocom's documents, "Z-Machine" in the
post-1995 Standard) ran the entire *Zork* trilogy and most of the
Infocom catalogue.  After Activision shelved Infocom in 1989, Graham
Nelson reverse-engineered it from the binaries, published the
**Inform** language that compiles to it, and the Z-Machine became the
de-facto interactive-fiction VM -- still used by the IF community
today, four decades later.

The instruction set is small: about 180 opcodes across versions, with
a clean separation between game-state mutation (objects, globals,
stack) and host-platform interaction (text I/O, save/restore).  The
VM has 16-bit values, big-endian byte order, a single accumulator-free
register set (everything is stack or memory), and a clever
text-encoding scheme that packs printable ASCII into 5-bit Z-characters
to save bytes on 1980s floppy disks.

This implementation targets versions **V1, V2, V3, V4, V5, V7, and V8**.
V6 is the graphical Z-Machine -- pictures, mouse, fonts, windowed UI --
a different surface from the V1-V5/V7/V8 prose interpreters.  V6 is
intentionally out of scope.

---

## 2. The Story File

### 2.1 Header Layout

A Z-Machine story file is a single big-endian byte stream.  The first
**64 bytes** are the header: a fixed-layout descriptor that identifies
the story (release + serial number), specifies where the major
structures live in memory (dictionary, object table, globals,
abbreviations), advertises capability flags both directions
(interpreter -> game and game -> interpreter), and records the
file-length checksum.

| Offset | Width   | Field                | Notes                                      |
| ------ | ------- | -------------------- | ------------------------------------------ |
| `0x00` | byte    | Version (1..8)       | Determines layout of everything else.      |
| `0x01` | byte    | Flags 1              | Capabilities; meaning differs V1-3 vs V4+. |
| `0x02` | word    | Release number       | Author-set; identifies the build.          |
| `0x04` | word    | High-memory base     | Boundary between static and high.          |
| `0x06` | word    | Initial PC           | Where execution begins.                    |
| `0x08` | word    | Dictionary address   | -> dict header                             |
| `0x0A` | word    | Object table address | -> object tree                             |
| `0x0C` | word    | Globals address      | -> 240 16-bit globals                      |
| `0x0E` | word    | Static-memory base   | Boundary between dynamic and static.       |
| `0x10` | word    | Flags 2              | More capabilities (mostly game-set).       |
| `0x12` | 6 bytes | Serial number        | Build date, ASCII (e.g., `840726`).        |
| `0x18` | word    | Abbreviations table  | -> abbrev pointer table                    |
| `0x1A` | word    | File-length / N      | Scaled; multiply by 2/4/8 per version.     |
| `0x1C` | word    | Checksum             | Sum of bytes 0x40..end mod 0x10000.        |
| `0x1E` | byte    | Interpreter number   | Set by interpreter (V4+).                  |
| `0x1F` | byte    | Interpreter version  | Set by interpreter (V4+).                  |
| `0x20` | byte    | Screen height (rows) | Set by interpreter (V4+).                  |
| `0x21` | byte    | Screen width  (cols) | Set by interpreter (V4+).                  |
| `0x32` | word    | Standard revision    | Spec revision the interpreter follows.     |

The Inform-Fiction specification (see [§14](#14-further-reading))
documents the full 64-byte layout.  IFhd -- the canonical Z-Machine
fingerprint -- is `release : serial-number`, e.g. Zork I = `88:840726`.

Run `./trix examples/zmachine.trx --header <story.z3>` to see this
layout dumped in human-readable form for any story file.

### 2.2 Version Variants V1-V8

The header's first byte distinguishes seven (eight, counting V6)
incompatible variants of the same VM.

| Version | Year  | What Changed                                                     |
| ------- | ----- | ---------------------------------------------------------------- |
| V1      | 1979  | Original; *Zork I* (Personal Software release).                  |
| V2      | 1980s | Adds extra opcodes; rare.                                        |
| V3      | 1982  | The classic Infocom format.  Most of the catalogue.              |
| V4      | 1985  | Wider object headers (48 attrs vs 32), windowing, sound effects. |
| V5      | 1987  | Color, single-key input, EXT opcode family, undo, mouse.         |
| V6      | 1988  | Graphics + windowing; out of scope here.                         |
| V7      | 1990s | Same as V5 with extended packed-address resolver.                |
| V8      | 1990s | Wider packed addresses (multiply by 8) for stories up to 512 KB. |

The interpreter dispatches on the version byte at every divergence
point.  Most user-visible differences live in the object table format
(V1-3 vs V4+), the input opcodes (V3 `sread` vs V5 `aread`), and the
windowing model (V3 implicit status line vs V4+ explicit
`split_window`).

---

## 3. Memory Map

### 3.1 Three Zones

The story file divides its address space into three named zones:

```
  0x0000 +---------------------+ 0x0000
         |  header (64 B)      |
  0x0040 +---------------------+
         |                     |
         |   dynamic memory    |  read-write; snapshotted on save
         |                     |
  ?      +---------------------+ h-static-mem
         |                     |
         |    static memory    |  read-only; not snapshotted (fixed)
         |                     |
  ?      +---------------------+ h-high-mem
         |                     |
         |     high memory     |  read-only; never written; routines +
         |                     |  packed strings live here
         |                     |
  EOF    +---------------------+
```

The boundaries are set at compile time and stored in the header
(`h-static-mem` at offset `0x0E`, `h-high-mem` at `0x04`).

Dynamic memory holds the writable game state: globals, the object
tree, dynamic property tables, and dictionary state if the game
modifies it.  Save/restore captures the dynamic-memory bytes; static
and high memory are immutable, so they don't need to be saved.

High memory is reachable only through **packed addresses** (see
below), which is what lets V8 stories exceed the 16-bit limit.

### 3.2 Packed Addresses

Z-Machine values are 16-bit words.  Routines and packed strings live
in high memory, often above the 64 KB ceiling those words can
address.  The trick: packed addresses are **scaled**, decoded by
multiplying by a per-version constant.

| Version | Routine multiplier  | String multiplier                             |
| ------- | ------------------- | --------------------------------------------- |
| V1-3    | 2                   | 2                                             |
| V4-5    | 4                   | 4                                             |
| V6-7    | 4 + routines-offset | 4 + strings-offset (header words 0x28 / 0x2A) |
| V8      | 8                   | 8                                             |

So V3 reaches 128 KB, V5 reaches 256 KB, and V8 reaches 512 KB even
though every operand is 16-bit.

In Trix: `z-paddr-r` and `z-paddr-s` resolve a 16-bit packed value to
the byte address of the routine / string.

### 3.3 The 16-bit Address Wrap (Spec §1.1.3)

For **direct memory ops** (`loadw`, `loadb`, `storew`, `storeb`), the
spec says: "if the calculated address would exceed the addressable
range, wrap mod 0x10000."

This shows up in V8 stories whose static-memory tables straddle the
64 KB line.  Without the wrap, `loadw 0xFFFE 1` would read at
`0x10000` (out of range); with the wrap, it reads at `0x0000`.  The
hitchhiker.z5 and anchor.z8 catalog games trip on this -- our
implementation handles it via `z-rb` / `z-rw` clamping.

### 3.4 Banking for Stories Larger than 64K

Trix strings have a 65535-byte cap, so V8 stories (up to 512 KB) need
to be split across multiple Trix strings.  The loader (§0) chunks the
file into 32 KB banks (`/sb-bank-size` = `0x8000`) stored in
`/story-bytes` (a Trix array of strings).  `sb-byte-get` and
`sb-byte-put` translate any 0..512K address into (bank, offset) pairs.

V3 stories (max 128 KB) fit in one or two banks; V5 stories
(max 256 KB) take up to 8 banks; V8 stories take up to 16.

---

## 4. ZSCII Text Encoding

ZSCII ("Z-machine Standard Code for Information Interchange") is a
variant of ASCII tuned for the constraints of 1979 microcomputer
storage.  The encoding crams printable text into **5-bit Z-characters**
packed three-per-16-bit-word, with special escapes for capitalization,
punctuation, and the rest of ASCII.

### 4.1 Z-Characters and the Three Alphabets

A 5-bit Z-character (0..31) means different things depending on the
**current alphabet**:

```
  Z-char  A0 (lowercase)  A1 (uppercase)  A2 (punct + digit)
  ------  --------------  --------------  ------------------
  0       (space)         (space)         (space)
  1       (abbrev 1)      (abbrev 1)      (abbrev 1)
  2       (abbrev 2)      (abbrev 2)      (abbrev 2)
  3       (abbrev 3)      (abbrev 3)      (abbrev 3)
  4       shift to A1     shift to A1     shift to A1
  5       shift to A2     shift to A2     shift to A2
  6       a               A               (10-bit ZSCII escape)
  7       b               B               \n
  8       c               C               0
  9       d               D               1
  ...     ...             ...             ...
  31      z               Z               )
```

Decoding starts in **A0** (lowercase) and resets to A0 after each
non-shift character.  Z-character `4` shifts to A1 for *one*
character, and `5` shifts to A2 for one character.  In V1 and V2,
shifts could lock; in V3+, they're always temporary.

### 4.2 Word Packing and the End-of-String Bit

Three Z-characters fit in a 16-bit word, packed high-bit-first:

```
  +----+--------+--------+--------+
  | E  |  Z1    |  Z2    |  Z3    |
  +----+--------+--------+--------+
   bit 15  bits 14-10  9-5    4-0
```

The high bit (`E`) is the **end-of-string** marker -- set on the last
word.  A string is a sequence of words ending with the first one that
has bit 15 set.

This is why every Z-Machine string length is a multiple of 3
Z-characters (padded with `5`s if needed).  It's also why packed
addresses can be 16-bit but reach above 64 KB: strings always start
on a word boundary, and the multiplier exploits that.

### 4.3 Abbreviations

Z-characters 1, 2, and 3 (in any alphabet) introduce a 2-character
escape that expands to one of 96 abbreviation strings.  The next
Z-char (0..31) selects which abbreviation, decoded as
`32 * (zc - 1) + next-zc`.

The abbreviation table lives at `h-abbrev-addr`; each entry is a
packed-string address pointing to the expanded text.

This is how Infocom kept the *Zork* trilogy in 128 KB.  Common phrases
("the ", "ing ", "ould") are stored once and referenced by 2-char
codes throughout the story text.

### 4.4 The 10-bit ZSCII Escape

The Z-character pair `(5, 6)` -- shift to A2, then `6` -- escapes the
next *two* Z-characters into a 10-bit raw ZSCII codepoint.  High 5
bits, then low 5 bits.  This lets strings include characters outside
the alphabets (digits in V1, accented letters, punctuation).

ZSCII codepoints 32-126 map to ASCII; 13 is newline; 0 is null; 9 and
11 are tab/sentence-space (V6 only); 155-251 are extra characters
from the V5 Unicode table (not implemented here -- they emit `?`).

---

## 5. The Object Tree

The object tree is the Z-Machine's primary world model.  Every entity
the player can examine, take, or interact with is an **object**:
rooms, items, NPCs, ghosts, light sources.  Objects have a short
name, a set of yes/no **attributes**, and a list of typed **properties**.
They form a tree via parent/sibling/child pointers (so a room contains
items; a chest inside the room contains more items; etc.).

### 5.1 V3 Format (4-byte body, 32 attributes)

```
  Object header (V3): 9 bytes per object
  +---------+---------+---------+---------+-----+-----+-----+--------+
  | attr0-7 | attr8-15| attr16-23| attr24-31| par | sib | chl |  prop  |
  +---------+---------+---------+---------+-----+-----+-----+--------+
   0         1         2         3         4     5     6     7-8

  par/sib/chl are 1-byte object numbers (max 255 objects).
  prop is a 2-byte address pointing to this object's property table.
```

The 32 attribute bits are accessed by bit position 0..31 (attribute 0
is the high bit of byte 0).  `test_attr`, `set_attr`, `clear_attr`
are the spec opcodes.

V3 supports up to 255 objects.  The object table starts with 31
"default" property words (used when an object lacks a property
explicitly), followed by the object headers in numerical order.

### 5.2 V4+ Format (14-byte body, 48 attributes)

```
  Object header (V4+): 14 bytes per object
  +---------+---------+----+----+----+--------+
  | attr0-7 | ... 5x  | par| sib| chl|  prop  |
  +---------+---------+----+----+----+--------+
   0         1-5       6-7  8-9  10-11 12-13

  par/sib/chl are 2-byte object numbers (max 65535 objects).
  prop is a 2-byte address pointing to this object's property table.
```

V4+ has 48 attribute bits and 16-bit object numbers, supporting much
larger games.  V4+ also has 63 default property words.

### 5.3 Property Tables

Each object's property table starts with a Z-encoded "short name"
string (the object's printable name), followed by zero or more
properties in *descending* property number, terminated by a zero byte.

```
  V3 property entry:       V4+ property entry:
  +------+-----------+     +-----+-----+-----------+
  | size | data ...  |     | sz1 | sz2 | data ...  |
  +------+-----------+     +-----+-----+-----------+
                            (sz1 has top bit = "size byte present")
```

In V3, the `size` byte encodes both the property number (low 5 bits)
and length-1 (top 3 bits, so 1..8 bytes).  V4+ uses up to two size
bytes for properties up to 64 bytes long.

Spec opcodes: `get_prop`, `put_prop`, `get_prop_addr`,
`get_prop_len`, `get_next_prop`.

The implementation walks property tables linearly; this is
spec-correct but O(N) per access.  Most games have <10 properties per
object, so it doesn't matter.

---

## 6. Variables, Locals, Stack

The Z-Machine has a unified 8-bit "variable number" address space,
with 256 distinct slots:

| Number    | Meaning                                                |
| --------- | ------------------------------------------------------ |
| `0`       | Top of evaluation stack (push on write, pop on read).  |
| `1..15`   | Routine local variables (1-indexed; per-call frame).   |
| `16..255` | Global variables (16-bit; stored at `h-globals-addr`). |

Almost every opcode that takes a variable accepts any of these three
forms via this 8-bit number.  The decoder doesn't distinguish; the
runtime looks up the value via the variable number at execution time.

The **evaluation stack** is a single 16-bit-word stack used for
intermediate computation.  Routine calls don't have a return value
register -- the spec instead says the return value is "stored" via a
**store-target** byte attached to the call instruction.  If the target
is `0`, the return value is pushed on the stack; otherwise it goes to
the named local or global.

Each routine call pushes a **frame** containing the return PC, the
locals array (up to 15 16-bit slots), the number of arguments
actually passed, the store-target for the call's return value, and
the eval-stack base (so frame-pop can discard partial pushes).

The stack and frame array are both pre-allocated; we don't grow them
dynamically.  Stack overflow at 1024 elements throws
`/opstack-overflow`; frame overflow at 256 frames throws similarly.

`load`, `store`, `inc`, `dec`, `inc_chk`, `dec_chk`, `load_indirect`,
`store_indirect`, `pull`, `push` are the variable-access opcodes.
The "indirect" forms compute a variable number at runtime (e.g.,
"increment whatever variable global 17 names").

---

## 7. Opcode Encoding Forms

The Z-Machine has four instruction encoding forms, distinguished by
the top bits of the opcode byte:

```
  Top bits   Form         Operands                                      Example
  --------   ----         --------                                      -------
  00......   long         2 OP, both small (byte) or variable           je
  10......   short        0 or 1 OP, type encoded in bits 4-5           ret
  110.....   variable     up to 4 OPs, type byte follows                call
  10100100   extended     EXT prefix, opcode byte follows (V5+)         save_undo
```

Each form carries one of four operand types:

| Type           | Width  | Meaning                                        |
| -------------- | ------ | ---------------------------------------------- |
| Large constant | 16-bit | Immediate word (e.g., a packed address).       |
| Small constant | 8-bit  | Immediate byte (zero-extended to 16 bits).     |
| Variable       | 8-bit  | Variable number (looked up via the §6 scheme). |
| Omitted        | 0      | This operand is not present.                   |

The decoder reads the form, then the operand-type byte (variable form),
then up to 4 operands per the type table.  Some opcodes also have a
**store-target** byte (1 byte: variable number for the result) and/or
a **branch byte** (1 or 2 bytes: 1-bit polarity, 1-bit length, then a
6- or 14-bit signed offset).

Branch offsets of 0 or 1 are special: they mean `rfalse` (return false
from the current routine) and `rtrue` (return true).

In Trix: `z-decode-instr` reads from `/z-pc`, advances it past the
last consumed byte, and returns an instruction record:
`{ kind, opcode, operands, store-target, branch }`.  The dispatcher
(`z-execute-instr`) looks up the opcode in one of five dicts (`op-2op`,
`op-1op`, `op-0op`, `op-var`, `op-ext`) and calls the registered
handler with the instruction record.

---

## 8. Instruction Set Reference

There are about 180 opcodes across all versions.  This section
summarizes them by category; for the bit-level encoding of each, see
the spec.  Trix dispatch tables live in §5 of `zmachine.trx`.

### 8.1 Arithmetic and Logic

| Mnemonic    | Form                         | Effect                                            |
| ----------- | ---------------------------- | ------------------------------------------------- |
| `add`       | 2OP:14                       | store a + b (signed 16-bit, mod 0x10000)          |
| `sub`       | 2OP:15                       | store a - b                                       |
| `mul`       | 2OP:16                       | store a * b (low 16 bits)                         |
| `div`       | 2OP:17                       | store a / b (signed, truncated toward zero)       |
| `mod`       | 2OP:18                       | store a mod b                                     |
| `and`       | 2OP:09                       | store bitwise AND                                 |
| `or`        | 2OP:08                       | store bitwise OR                                  |
| `not`       | 1OP:0F (V1-4) / VAR:18 (V5+) | store bitwise NOT (16-bit)                        |
| `log_shift` | EXT:02                       | store left/right shift (signed count, zero-fill)  |
| `art_shift` | EXT:03                       | store left/right *arithmetic* shift (sign-extend) |

Spec §11.4: `art_shift` of a negative value uses **floor** division
(so the sign extends).  Trix's `div` truncates toward zero, so we
post-correct: `(q, r) = (v / 32, v mod 32); if v<0 and r!=0 then q -= 1`.

### 8.2 Branching

| Mnemonic    | Form   | Branch condition                                   |
| ----------- | ------ | -------------------------------------------------- |
| `je`        | 2OP:01 | a == any of b/c/d                                  |
| `jl`        | 2OP:02 | a < b (signed)                                     |
| `jg`        | 2OP:03 | a > b (signed)                                     |
| `jin`       | 2OP:06 | a is child of b                                    |
| `test`      | 2OP:07 | (a & b) == b (all set)                             |
| `test_attr` | 2OP:0A | object a has attribute b                           |
| `inc_chk`   | 2OP:05 | ++var-a > b (signed)                               |
| `dec_chk`   | 2OP:04 | --var-a < b (signed)                               |
| `jz`        | 1OP:00 | a == 0                                             |
| `jump`      | 1OP:0C | unconditional (signed offset; not a "branch" form) |
| `verify`    | 0OP:0D | stored checksum matches                            |

`inc_chk` / `dec_chk` increment/decrement *in 16-bit signed space* --
spec §11.4 mandates wrap so that incrementing `0x7FFF` yields
`0x8000` (= -32768) and the comparison takes the wrapped value.  The
implementation re-wraps after the inc/dec (this is one of the eight
spec compliance bugs the EOD#27 sweep fixed).

### 8.3 Object Operations

| Mnemonic        | Form   | Effect                                    |
| --------------- | ------ | ----------------------------------------- |
| `test_attr`     | 2OP:0A | branch on attribute                       |
| `set_attr`      | 2OP:0B | set attribute bit                         |
| `clear_attr`    | 2OP:0C | clear attribute bit                       |
| `jin`           | 2OP:06 | branch if a is direct child of b          |
| `get_parent`    | 1OP:03 | store parent of object                    |
| `get_sibling`   | 1OP:01 | store sibling and branch                  |
| `get_child`     | 1OP:02 | store first child and branch              |
| `insert_obj`    | 2OP:0E | make a a child of b (relink tree)         |
| `remove_obj`    | 1OP:09 | detach a from its parent                  |
| `get_prop`      | 2OP:11 | store property value (default if absent)  |
| `put_prop`      | VAR:03 | set property value                        |
| `get_prop_addr` | 2OP:12 | store address of property data            |
| `get_prop_len`  | 1OP:04 | store length of property at given address |
| `get_next_prop` | 2OP:13 | iterate properties in order               |
| `print_obj`     | 1OP:0A | print object's short-name                 |

### 8.4 Print and Output

| Mnemonic | Form | Effect |
| --- | --- | --- |
| `print` | 0OP:02 | print inline ZSCII string (ends at next E-bit) |
| `print_ret` | 0OP:03 | print inline string + newline + rtrue |
| `new_line` | 0OP:0B | print newline |
| `print_paddr` | 1OP:0D | print string at packed address |
| `print_addr` | 1OP:07 | print string at byte address |
| `print_char` | VAR:05 | print one ZSCII codepoint |
| `print_num` | VAR:06 | print signed decimal |
| `print_obj` | 1OP:0A | print object's short-name |
| `output_stream` | VAR:13 | activate / deactivate output stream (1=screen, 2=transcript, 3=memory, 4=record) |
| `print_unicode` | EXT:0B | print 16-bit Unicode codepoint (V5+) |
| `print_table` | VAR:1E | print rectangular text region (V5+) |

### 8.5 Input

| Mnemonic      | Form   | Effect                                                     |
| ------------- | ------ | ---------------------------------------------------------- |
| `sread`       | VAR:04 | read line, tokenise, parse-buffer (V3-4)                   |
| `aread`       | VAR:04 | same as `sread` but stores terminator char (V5+)           |
| `read_char`   | VAR:16 | read one keystroke, store ZSCII codepoint (V4+)            |
| `tokenise`    | VAR:1B | re-tokenise an already-read line (V5+)                     |
| `encode_text` | VAR:1C | encode an ASCII string into Z-encoded dictionary key (V5+) |

`sread`/`aread` operands include optional **time** + **routine** for
timed input -- the spec says the interpreter calls the routine every
`time/10` seconds during input, and aborts if the routine returns
true.  This implementation **ignores** the time/routine operands
(flags1 bit 7 stays clear so games know not to rely on them).

### 8.6 Routine Call and Return

| Mnemonic     | Form   | Effect                                                       |
| ------------ | ------ | ------------------------------------------------------------ |
| `call`       | VAR:00 | call routine; store result via store-target (V4+: `call_vs`) |
| `call_vs2`   | VAR:0C | 8-operand variant (V4+)                                      |
| `call_vn`    | VAR:19 | call routine; discard result (V5+)                           |
| `call_vn2`   | VAR:1A | 8-operand discard variant (V5+)                              |
| `call_2s`    | 2OP:19 | 1-arg call, store result (V4+)                               |
| `call_2n`    | 2OP:1A | 1-arg call, discard (V5+)                                    |
| `call_1s`    | 1OP:08 | 0-arg call, store (V4+)                                      |
| `call_1n`    | 1OP:0F | 0-arg call, discard (V5+)                                    |
| `ret`        | 1OP:0B | return value from routine                                    |
| `rtrue`      | 0OP:00 | ret 1                                                        |
| `rfalse`     | 0OP:01 | ret 0                                                        |
| `ret_popped` | 0OP:08 | ret top-of-stack                                             |

A routine's first byte is the **locals count** (0..15).  In V1-4, the
next `2 * count` bytes are signed 16-bit *defaults* for each local;
in V5+, locals always default to zero (the prefix bytes are absent).
Arguments overwrite the leading locals; missing locals keep their
default value.

### 8.7 Save, Restore, Undo

| Mnemonic | Form | Effect |
| --- | --- | --- |
| `save` | 0OP:05 (V3, branch) / EXT:00 (V5+, store) | Save state to "an unspecified location" (we use Trix snap-shot + a manual dyn-mem byte buffer). |
| `restore` | 0OP:06 (V3, branch) / EXT:01 (V5+, store) | Restore previously saved state. |
| `restart` | 0OP:07 | Re-enter the initial post-init state. |
| `save_undo` | EXT:09 (V5+) | Snapshot to in-memory undo slot.  Stores 1 on the way through, 2 on restore. |
| `restore_undo` | EXT:0A (V5+) | Blit the undo slot back; stores 0 if no slot, 2 on success. |
| `verify` | 0OP:0D | Branch on file-checksum match (we precompute at load). |
| `piracy` | 0OP:0F | Branch.  Always take the "genuine" branch (no real interpreter ever did otherwise). |

See [§10](#10-save-restore-and-undo) for implementation details.

### 8.8 Windows and Cursor (V4+)

| Mnemonic       | Form   | Effect                                                           |
| -------------- | ------ | ---------------------------------------------------------------- |
| `split_window` | VAR:0A | Reserve N rows for the upper window.                             |
| `set_window`   | VAR:0B | 0 = lower (scroll), 1 = upper (fixed-grid).                      |
| `erase_window` | VAR:0D | -1 = collapse + clear all, -2 = clear only, 0 = lower, 1 = upper |
| `erase_line`   | VAR:0E | Erase to end-of-line within current window (V5+)                 |
| `set_cursor`   | VAR:0F | Position upper-window cursor.                                    |
| `get_cursor`   | VAR:10 | Store tracked (row, col) into a 4-byte array.                    |

See [§9](#9-windows-and-display) for implementation details.

### 8.9 Color and Style (V5+)

| Mnemonic | Form | Effect |
| --- | --- | --- |
| `set_text_style` | VAR:11 | 0=reset, bit 0=reverse, 1=bold, 2=italic, 3=fixed-pitch |
| `set_colour` | 2OP:1B | 9-color palette (2..9 = black/red/green/yellow/blue/magenta/cyan/white; 0 = no change, 1 = default) |
| `set_true_colour` | EXT:0D | 15-bit RGB (R5+G5+B5); -1 no change, -2 default |

`set_text_style` emits ANSI SGR escapes (`\x1B[7m`, `\x1B[1m`,
`\x1B[3m`); `set_colour` emits 8-color SGR (30..37 fg, 40..47 bg);
`set_true_colour` emits 24-bit truecolor SGR (38;2;R;G;B and
48;2;R;G;B), upscaling each 5-bit channel to 8 bits via replication.

---

## 9. Windows and Display

### 9.1 V3 Status Line

V3's spec §8.2 describes a fixed 1-row "status line" at the top of
the screen, rendered by the *interpreter* (not the game).  It shows
the player's current location plus either a score/moves pair or a
clock, depending on flags1 bit 1.

This implementation uses **ANSI DECSTBM** (CSI `t;b r`) to lock
line 1 as a non-scrolling region while game text scrolls inside lines
2..rows.  `show_status` (0OP:0xC) is auto-called before each `sread`,
and uses cursor save / home / paint / restore (`\x1B7` + `\x1B[H` +
content + `\x1B[K` + `\x1B[0m` + `\x1B8`) so the lower-window cursor
stays put.

When stdout isn't a tty (pipes, `--script` to a redirected file,
self-test capture), we fall back to **inline reverse-video text** so
transcripts still show the status line in stream order.

### 9.2 V4+ Split Window

V4+ removes the implicit status line and gives the game an explicit
`split_window N` opcode to claim N upper rows.  Output then routes to
either the upper window (no scroll, no wrap, fixed-grid, cursor
position controlled by `set_cursor`) or the lower window (default
scrolling) based on `set_window` calls.

The implementation generalizes the V3 status-line infrastructure:
DECSTBM is set to (upper-height + 1) .. rows, and a 3-state machine
(NORMAL / ESC-seen / CSI-body) routes upper-window output through
`z-upper-write`, which uses cursor save / direct positioning / restore
and passes ANSI escape sequences through verbatim without advancing
the tracked cursor.

This is what lets *Beyond Zork* draw its color-coded HUD, *Trinity*
render its date/time bar, and *A Mind Forever Voyaging* set its
inventory row.

### 9.3 Output Stream Routing

Per spec §7.1.2, the Z-Machine has four output streams:

| #   | Name            | What we do                                                   |
| --- | --------------- | ------------------------------------------------------------ |
| 1   | Screen          | Default; routes through z-output-text.                       |
| 2   | Transcript file | Not wired (game would write to a host file).                 |
| 3   | Memory table    | Fully implemented; up to 16 nested tables per spec §7.1.2.3. |
| 4   | Command record  | Not wired.                                                   |

`z-output-text` is the central router.  Priority order:

1. **Stream 3 (memory)** -- when active, suppresses ALL other streams
   and writes ZSCII bytes into the game's table.
2. **Test capture** -- when `/z-test-capturing` is true, appends to a
   Trix string buffer for self-test assertions.
3. **Upper window** -- when window 1 is active and stdout is a tty,
   routes to `z-upper-write` for cursor-positioned output.
4. **Stdout** -- the default lower-window scrolling stream.

---

## 10. Save, Restore, and Undo

The Z-Machine has two save mechanisms: an **on-disk** save (`save` /
`restore`) for between-session persistence, and an **in-memory** undo
(`save_undo` / `restore_undo`) for one-step "take that back" semantics.
This implementation provides both, but with different mechanisms.

### 10.1 On-Disk Save / Restore

Spec opcodes: `0OP:5 save` and `0OP:6 restore` (V3, branch form);
`EXT:0 save` and `EXT:1 restore` (V5+, store form).

These use Trix's transactional **snap-shot/thaw** mechanism, which
captures the entire Trix heap (eval-stack contents, frame stack,
local arrays, name bindings).  A complementary manual byte-buffer
captures dynamic memory (Trix string-byte writes aren't journaled, so
we copy them explicitly into `/z-save-dynmem` before snap-shot, and
restore them before thaw).

This is fast and convenient but has two known limitations:
- `restore` rolls back the file-stream read offset of `--script`
  input, which can cause replay loops if a script triggers a restore.
- The `save` instruction's frame is part of the captured state, so
  after restore the handler runs to completion a second time.

These edge cases are explicitly documented as out-of-scope for v1.

### 10.2 The Manual Snapshot System (save_undo)

`EXT:9 save_undo` and `EXT:0xA restore_undo` need a much faster, lighter
mechanism than on-disk save -- and crucially, they must NOT roll back
file-stream offsets (since undo can fire mid-`--script`).

Solution: capture **only** the Z-machine state into a fresh Trix-side
struct, never touching Trix snap-shot.  The snapshot is a 6-slot
array:

```
  [0] dynmem-bytes       Trix string of length h-static-mem
  [1] z-eval-top         eval-stack used count
  [2] eval-copy          Trix array of eval-top entries
  [3] z-frame-top        used frame count
  [4] frame-copy         Trix array of fresh 6-tuple frames with
                         deep-copied 16-slot locals arrays
  [5] z-pc               program counter
```

Frame deep-copy is essential -- each frame's locals-array is its own
Trix array, and post-snapshot writes to live locals must not leak
into the snapshot.

**The store-target round-trip trick.**  Per spec, `save_undo` stores 1
on the way through (live), and the *same* store-target sees 2 after a
future `restore_undo` blits the snapshot back.  This works because:

1. `save_undo` writes 2 to the target FIRST.
2. Then captures the snapshot (target=2 baked into eval/frame state).
3. Then overwrites with 1 for the live continuation.

For stack-target writes (target=0), step 1 pushes 2 onto eval-stack;
step 3 uses `z-var-poke` (peek-replace via `z-stack-replace`) to
overwrite the top with 1 instead of pushing a second value.  The live
stack has 1 on top; the snapshot's eval-stack has 2 baked in.

After a successful restore the slot is **invalidated** -- a second
`restore_undo` (without an intervening `save_undo`) returns 0.  This
is the single-slot semantic; multi-level undo (a stack of snapshots)
is a praxix-only feature beyond what the spec mandates.

### 10.3 Named Save Slots

The same manual snapshot system backs the showcase's named save slots
(`/save <name>`, `/restore <name>`, `/saves`, `/delete <name>`).
Each named slot is a snapshot stored in `/z-named-saves` keyed by
name.  Because nothing goes through Trix snap-shot, replay under
`--script` works cleanly:

```
> open mailbox
> /save start
> north
> north
> /restore start
> look          % back at "West of House" with the mailbox open
```

This is a feature spec-compliant interpreters can't easily provide
(the spec gives only a single undo slot).  Trix-snapshotting is what
makes it almost free.

---

## 11. Compliance Suites

Two well-known Z-Machine conformance test ROMs are catalogued here
(fetched by `fetch-stories.py` like every other story file, never bundled):

### `czech.z5` (Comprehensive Z-machine Emulation CHecker)

By Evin Robertson.  425 unattended tests covering every opcode's
boundary cases.  Run:

```
./trix examples/zmachine.trx examples/zmachine/czech.z5
```

Reports `Performed N tests.  Passed: P, Failed: F.  Print tests: K`.
This implementation passes **425 / 425** (406 automatic + 19 visual
print-test inspections).  Eight spec-compliance bugs surfaced by
czech were fixed during the EOD#27 sweep:

1. Indirect-store family (2OP:0x0D `store` with var=0) -- spec §6.3.4
   peek-replace, not push.
2. `art_shift` floor division on negatives (vs Trix's truncated-toward-zero `div`).
3. `random` with negative seed -- must initialize PRNG and produce
   a deterministic sequence.
4. `verify` -- checksum is computed against the original on-disk
   bytes, not current dynamic memory.
5. `inc_chk` / `dec_chk` -- 16-bit signed wrap before comparison.
6. `copy_table` -- positive size = smart copy (forward when dst<=src,
   backward when dst>src) to avoid corrupting source on overlap.
7. `output_stream 3` (memory) -- previously unimplemented; now full
   16-nested-table support per spec §7.1.2.3.
8. `flags1` interpreter capability bits -- V4+ bits 2/3/4
   (bold/italic/fixed-space), V5+ bit 0 (colors).

### `praxix.z5`

By Andrew Plotkin.  Interactive (accepts `all` to run every category):

```
printf 'all\nquit\n' > /tmp/cmds
./trix examples/zmachine.trx --script /tmp/cmds examples/zmachine/praxix.z5
```

This implementation passes **all but 2 multiundo tests** (which
require a stack of snapshots, not just a single slot -- praxix tests
this even though the spec doesn't mandate it).

---

## 12. CLI and Showcase Commands

### 12.1 Command-Line Flags

```
./trix examples/zmachine.trx [flags] [story-file]
```

| Flag | Effect |
| --- | --- |
| `--header <story>` | Dump the 64-byte header in human-readable form, exit. |
| `--script <cmds-file>` | Feed sread input from a text file instead of stdin. |
| `--self-test` | Run the in-source assertion suite (272 checks). |
| `--auto-map` | Record V3 room transitions; query via `/map`. |
| `--theme <name>` | CRT theme: `classic` (default), `phosphor`, `amber`, `paper`. Sets terminal default fg/bg via OSC 10/11. |
| `--hints <file>` | Load an InvisiClues-style `.hints` file; query via `/hint`. |
| `--pager` | Auto-pause every page of game text (tty only).  Reset per `sread`/`aread`. |
| `--pager-pause=<ms>` | Pager pause length in ms.  Default 500. |
| `--typewriter` | Per-char delay on game output (tty only).  ANSI escape sequences emit atomically (no inter-char delay). |
| `--typewriter-delay=<ms>` | Per-char delay in ms.  Default 12 (~80 cps). |
| `--vm-size <bytes>` | Set the Trix VM heap size.  Default 1M; classic stories fit it, opcode-hungry Dialog-era V8 titles want 2M+ (live working set + story banks must fit between the interpreter's periodic gc sweeps). |
| `--help`, `-h` | Print usage and exit. |

### 12.2 Showcase Slash-Commands

Slash-prefixed input is intercepted *before* the game's parser sees
it -- no game turn is consumed, no clock advances.

```
/help              -- list these commands
/inspect <obj>     -- short-name + attributes + properties + parent chain
/dict <word>       -- dictionary entry: encoded key + byte address
/globals           -- list nonzero globals (G00..GEF)
/where             -- walk current location's parent chain
/stats             -- runtime stats: turns, opcodes, wall time
/map               -- auto-map adjacency graph (needs --auto-map)
/theme <name>      -- live theme switch (classic, phosphor, amber, paper)
/hint              -- list InvisiClues questions (needs --hints)
/hint <N>          -- reveal next hint for question N
/hint reset [<N>]  -- reset all reveal counters (or just question N)
/save <name>       -- capture state into a named slot (manual snapshot)
/restore <name>    -- blit a named slot back
/saves             -- list named slot names
/delete <name>     -- remove a named slot
```

`/save` / `/restore` are backed by the same manual snapshot machinery
that powers `save_undo`, so they work cleanly under `--script`.  `/map`
records a graph of room transitions (V3-only; V4+ doesn't put the
location in the status line).

---

## 13. Implementation Tour

### 13.1 File Map

`examples/zmachine.trx` is a single ~7900-line Trix program organized
into 14 sections (see the file header for the live ToC):

```
  §0   Story-file loader + 64-byte header parser + --header dump
  §1   Memory primitives (z-rb / z-rw / z-wb / z-ww with bank-aware
       sb-byte-get / sb-byte-put) + packed-address resolvers
  §2   ZSCII text decoder (singleton 4 KB /zd-out buffer; recursive
       abbreviation expansion)
  §3   Object tree (V3 + V4+ formats; obj 0 = "no object" sentinel)
  §4   Eval stack + frames + var dispatcher
  §5   Opcode decoder + disassembler (singleton /z-instr)
  §6   Dispatcher + 31 simple opcodes; on-disk save/restore;
       MANUAL SNAPSHOT SYSTEM (z-snapshot / z-restore-snapshot)
  §7   Routine call/return + end-to-end z-step / z-run
  §8   Object opcodes (test_attr / get_*/ put_prop / insert_obj / ...)
  §9   Print opcodes + z-output-text 4-sink router (stream-3 memory,
       capture, upper-window, stdout); z-stream3-stack memory output
  §10  Input + dictionary + tokenizer
  §11  V3 ship: status line via DECSTBM, --script CLI, run loop;
       upper-window infrastructure (z-upper-write, z-update-decstbm)
  §12  V4 / V5 / EXT extensions (windows, save_undo, color, EXT ops)
  §13  Self-test fixtures (catch/throw, set_colour, set_true_colour,
       window/cursor)
  §14  Showcase commands; auto-map; CRT themes; InvisiClues hints;
       game-fingerprint splash; named saves; main entry
```

### 13.2 Key Data Structures

| Global | Type | Purpose |
| --- | --- | --- |
| `/story-bytes` | string \| array of strings | The story file (V3 single string; V8 banked array). |
| `/z-pc` | integer | Program counter (byte address). |
| `/z-eval-stack` | 1024-array | Evaluation stack. |
| `/z-eval-top` | integer | Used count. |
| `/z-frame-stack` | 256-array | Routine frame stack. |
| `/z-frame-top` | integer | Used count. |
| `/z-instr` | dict | Singleton; mutated in place by decoder. |
| `/z-save-dynmem` | string | On-disk save: dynamic-memory snapshot. |
| `/z-save-token` | save-token | On-disk save: Trix snap-shot token. |
| `/z-undo-snapshot` | array \| null | save_undo single slot. |
| `/z-named-saves` | dict | Named save slots (showcase). |
| `/z-stream3-stack` | 16-array | Memory output stream nesting. |
| `/z-upper-height` | integer | Rows reserved for upper window. |
| `/z-window-active` | 0 \| 1 | Current output window. |
| `/z-upper-cur-{row,col}` | integer | Tracked upper-window cursor. |
| `/z-test-capturing` | bool | When true, redirect output to /z-test-capture. |
| `/known-games` | dict | IFhd -> title, for the splash. |
| `/z-auto-map-graph` | dict | --auto-map adjacency: room -> dir -> room. |
| `/z-hints-questions` | array | --hints loaded questions + reveal counters. |
| `/themes` | dict | --theme palettes (OSC 10/11 sequences). |

### 13.3 Design Decisions

**Pure Trix, no C++ side.**  Every byte of the interpreter is in
`zmachine.trx`.  No new C++ ops were added; the implementation
exercises only what was already in Trix (strings, dicts, arrays,
proc frames, snap-shot/thaw, terminal mode, ANSI sequences).

**One file, in narrative order.**  Sections are sequenced so that
each one builds only on what came before.  Reading top to bottom is
a guided tour of the spec.

**Decoder mutates in place.**  `/z-instr` is a singleton dict; the
decoder rewrites its fields each cycle.  This avoids per-instruction
allocation in the hot path (~125k opcodes/second on a modest laptop).

**Spec correctness over performance.**  Where trade-offs arose, we
chose the spec-correct path.  E.g., dictionary search is linear
(not binary) -- spec-conformant and simple, at the cost of a few
microseconds per parse.  Object property lookup is linear, not
hashed.

**ANSI escapes for windowing, not the Trix screen subsystem.**  The V3
status line and V4+ split window use raw ANSI DECSTBM + cursor save/
restore rather than Trix's `make-screen` / `screen-render` framework.
Reasons: (1) z-upper-write needs to interleave with arbitrary game
output (including ESC sequences from `set_text_style`), which is
hard to do via diff-rendered virtual buffers; (2) DECSTBM is widely
supported and cheaper.

**Manual snapshot for undo, NOT Trix snap-shot.**  `save_undo` /
`restore_undo` capture only Z-machine state, never touching the
Trix file-stream or proc-frame.  This is what makes `--script` mode
tolerate undo/restore without replay loops.

**Tty-aware output gating.**  Visual control sequences (DECSTBM,
cursor save/restore, screen clears) are gated on `z-status-tty?`
**and** `z-test-capturing not`, so transcripts and self-tests stay
clean.

---

## 14. Further Reading

- **The Inform-Fiction Z-Machine Standards Document (1.1)** -- the
  canonical spec.  https://www.inform-fiction.org/zmachine/standards/z1point1/
- **Graham Nelson, *The Inform Designer's Manual*** -- the
  forty-year history of how a 1979 VM became a community
  standard, plus a tutorial on writing IF in Inform 6/7.
- **Marnix Klooster, *The Z-Machine Standards Document, Annotated*** --
  notes on the spec's gnarlier corners (10-bit ZSCII escapes,
  pre-V5 timer ambiguities, etc.).
- **Andrew Plotkin, *praxix*** -- the conformance test ROM used here.
  https://www.ifarchive.org/if-archive/infocom/interpreters/tools/praxix.zip
- **Evin Robertson, *czech*** -- the comprehensive automatic conformance
  ROM. https://www.ifarchive.org/if-archive/infocom/interpreters/tools/czech_0_8.zip
- **Frotz** -- the long-running canonical reference interpreter,
  written in C; useful for cross-checking edge cases.
  https://gitlab.com/DavidGriffith/frotz
- **The IF Archive** -- comprehensive repository of Z-code stories,
  tools, and historical documents.  https://www.ifarchive.org/
- **The Interactive Fiction Database (IFDB)** -- catalog of every
  IF release with reviews, hints, and download links.
  https://ifdb.org/
- **examples/zmachine/CATALOG.md** -- this implementation's
  catalog of 99 playable story files plus the two compliance ROMs
  (a catalog of fetchable titles; no story file is bundled).
