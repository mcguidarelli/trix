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

# CHIP-8 + Super-CHIP Programming Manual

A complete reference for the virtual machine emulated by
[`examples/chip8.trx`](chip8.trx), the assembler in
[`examples/chip8-asm.trx`](chip8-asm.trx), and the ROM fetcher
under [`examples/chip8-roms/`](chip8-roms/).

This manual is self-contained: read it end-to-end and you can write
playable CHIP-8 / Super-CHIP programs and assemble them straight into
binary ROMs that load into the emulator.

## Table of Contents

- [1. CHIP-8 in 60 Seconds](#1-chip-8-in-60-seconds)
- [2. Virtual Machine](#2-virtual-machine)
  - [2.1 Memory Map](#21-memory-map)
  - [2.2 Registers](#22-registers)
  - [2.3 The Stack](#23-the-stack)
  - [2.4 Display](#24-display)
  - [2.5 Keypad](#25-keypad)
  - [2.6 Timers](#26-timers)
  - [2.7 Font Sprites](#27-font-sprites)
- [3. Assembly Source Format](#3-assembly-source-format)
  - [3.1 Lines and Comments](#31-lines-and-comments)
  - [3.2 Numeric Literals](#32-numeric-literals)
  - [3.3 Operand Types](#33-operand-types)
  - [3.4 The Disassembler-Compatible Prefix](#34-the-disassembler-compatible-prefix)
  - [3.5 Pseudo-Directives](#35-pseudo-directives)
- [4. Instruction Set](#4-instruction-set)
  - [4.1 Control Flow](#41-control-flow)
  - [4.2 Conditional Skips](#42-conditional-skips)
  - [4.3 Register Loads and Arithmetic](#43-register-loads-and-arithmetic)
  - [4.4 ALU Ops (8XY*)](#44-alu-ops-8xy)
  - [4.5 The I Register](#45-the-i-register)
  - [4.6 Random Numbers](#46-random-numbers)
  - [4.7 Sprite Draw](#47-sprite-draw)
  - [4.8 Keyboard](#48-keyboard)
  - [4.9 Timers](#49-timers)
  - [4.10 BCD and Memory Bulk Ops](#410-bcd-and-memory-bulk-ops)
  - [4.11 Super-CHIP Extensions](#411-super-chip-extensions)
- [5. Programming Patterns](#5-programming-patterns)
  - [5.1 Hello, World (the IBM Logo)](#51-hello-world-the-ibm-logo)
  - [5.2 Frame Pacing via DT](#52-frame-pacing-via-dt)
  - [5.3 Sprite Animation and XOR Erase](#53-sprite-animation-and-xor-erase)
  - [5.4 Reading the Keypad](#54-reading-the-keypad)
  - [5.5 Subroutines](#55-subroutines)
  - [5.6 Random Numbers](#56-random-numbers)
  - [5.7 Drawing a Hex Digit](#57-drawing-a-hex-digit)
  - [5.8 Sound](#58-sound)
- [6. Super-CHIP Programming](#6-super-chip-programming)
- [7. Common Pitfalls and Variant Behavior](#7-common-pitfalls-and-variant-behavior)
- [8. Worked Example: `blink.ch8`](#8-worked-example-blinkch8)
- [9. Tooling Workflow](#9-tooling-workflow)
- [10. Further Reading](#10-further-reading)

---

## 1. CHIP-8 in 60 Seconds

CHIP-8 is a virtual machine designed by **Joseph Weisbecker** in 1977
to run small games on the COSMAC VIP and Telmac 1800 hobbyist
computers.  Programs are 2-byte big-endian opcodes (35 of them) over
4 KB of memory.  The display is 64x32 monochrome pixels; sprites are
XOR-drawn and a collision flag falls out for free.  There are 16
8-bit registers, one 16-bit index register, a delay timer, a sound
timer, and a 16-key hex keypad.

The instruction set is small enough to write a complete emulator in
an afternoon; the hardware is just expressive enough to host
recognizable games (Pong, Space Invaders, Brix).  Half a
century later it remains the canonical "first emulator" project for
any aspiring VM author.

**Super-CHIP** (SCHIP, 1991, by Erik Bryntse) layers a few extensions
on top: a 128x64 hi-res mode, 16x16 sprites, four scroll opcodes, an
EXIT instruction, eight RPL flag bytes for persistent storage, and a
hi-res font.  All standard CHIP-8 ROMs run unchanged on a SCHIP
interpreter.

This emulator targets both ISAs simultaneously.  ROMs that need only
CHIP-8 leave the resolution at 64x32; SCHIP ROMs issue `HIGH` to
switch.

---

## 2. Virtual Machine

### 2.1 Memory Map

```
  0x000 .. 0x04F   Low-res font sprites (16 chars × 5 bytes = 80 B)
  0x050 .. 0x0EF   Hi-res font sprites  (16 chars × 10 bytes = 160 B; SCHIP)
  0x0F0 .. 0x1FF   Reserved (originally COSMAC VIP interpreter)
  0x200 .. 0xFFF   Program ROM + working memory (3584 bytes)
```

The CPU starts execution at `0x200` after reset; ROMs are loaded at
that address.  All memory above `0x200` is shared between code and
data, so sprite tables, lookup tables, and dynamic working storage
all live alongside the program.

The bottom 256 bytes hold the system font sprites that the emulator
populates at boot, regardless of which ROM is loaded.  `LD F, Vx`
points `I` into the lo-res font region at `0x000`; `LD HF, Vx`
points it into the hi-res font region at `0x050`.

### 2.2 Registers

| Register     | Width  | Description                                                                        |
| ------------ | ------ | ---------------------------------------------------------------------------------- |
| `V0` .. `VE` | 8-bit  | General-purpose registers.                                                         |
| `VF`         | 8-bit  | Carry / borrow / collision flag (also general).                                    |
| `I`          | 16-bit | Index register: source/target address for memory ops, sprite address for `DRW`.    |
| `PC`         | 16-bit | Program counter; advanced by 2 each cycle (or by 4 on a successful skip).          |
| `DT`         | 8-bit  | Delay timer; counts down at 60 Hz while > 0.                                       |
| `ST`         | 8-bit  | Sound timer; counts down at 60 Hz while > 0.  Beeps on transition (with `--bell`). |
| `SP`         | 8-bit  | Stack pointer; index of next free call-stack slot (0..16).                         |

`VF` is technically a general-purpose register, but every instruction
that produces a carry, borrow, or collision overwrites it without
warning.  Code that wants to compute `Vx + Vy` and *also* keep a flag
elsewhere must save `VF` afterward.

The Super-CHIP additions:

| Register | Width | Description |
| --- | --- | --- |
| `R0`..`R7` | 8 × 8-bit | RPL persistent flags.  Survive reset; written by `LD R, Vx`, read by `LD Vx, R`.  Index is implicit (Vx in the instruction selects the count, not the slot). |

In this emulator RPL flags are scoped to a single run -- they reset
on every fresh `cpu-reset`.  A real HP48 would persist them across
power cycles.

### 2.3 The Stack

The call stack is 16 slots deep.  `CALL NNN` pushes the current `PC`
and jumps to `NNN`; `RET` pops the saved `PC`.  Recursion deeper
than 16 raises an opstack-overflow trap (the emulator throws
`/opstack-overflow` and the actor halts).  `RET` from an empty stack
throws `/opstack-underflow`.

Use the stack for ordinary subroutines; do not use it as a data
stack -- there are no `PUSH`/`POP` opcodes.

### 2.4 Display

Two resolutions:

- **Lo-res** (default): 64 wide × 32 tall, monochrome.
- **Hi-res** (SCHIP only, after `HIGH`): 128 × 64.

A pixel is one bit.  All sprite drawing is **XOR**: drawing a sprite
toggles each pixel covered by a `1` bit; pixels covered by a `0` bit
are unchanged.  This is what gives CHIP-8 its characteristic
"collision flag for free" -- if the XOR turns any pixel from on to
off, `VF` is set to `1`; otherwise `0`.

Sprites are 8 pixels wide and 1..15 rows tall (`DXYN`, where `N` is
the row count).  Each row is one byte; bit 7 is the leftmost pixel.
SCHIP adds a 16x16 sprite mode (`DXY0` in hi-res) that reads 32 bytes
(2 bytes per row, 16 rows).

Coordinates wrap at the framebuffer edges in this emulator (matches
the practical behavior of most modern interpreters; the COSMAC
original sometimes clipped).

`CLS` clears the framebuffer.  Switching resolution (`HIGH` / `LOW`)
clears it as a side effect.

### 2.5 Keypad

CHIP-8 has 16 hex keys (`0`..`F`).  This emulator maps them onto the
modern PC layout:

```
  Chip-8        PC keyboard
  -------       -----------
  1 2 3 C       1 2 3 4
  4 5 6 D   <-  Q W E R
  7 8 9 E       A S D F
  A 0 B F       Z X C V
```

`SKP Vx` skips the next instruction if the chip-8 key whose number is
in `Vx` is currently held.  `SKNP Vx` is the inverse (skip if NOT
held).  `LD Vx, K` blocks until any key is pressed and stores its
chip-8 number in `Vx`.

ESC quits the emulator; this is an emulator-level convention, not a
CHIP-8 instruction.

### 2.6 Timers

Two 8-bit timers, both decrementing at exactly 60 Hz when > 0:

- **DT** (delay timer): the program reads it via `LD Vx, DT` and
  spins / branches based on the value.  Used for frame pacing.
- **ST** (sound timer): the program writes it via `LD ST, Vx`; while
  ST > 0 the audio output is "active".  This emulator's audio is
  opt-in via `--bell`: it rings the terminal once per `ST > 0`
  transition, not once per active frame (which would be deafening).

Timers are independent of the CPU instruction rate.  At the default
500 IPS, ~8 instructions execute per 60 Hz frame; at `--ips=1000`,
~16.

### 2.7 Font Sprites

Two pre-populated font tables in low memory:

- **Lo-res**: 16 sprites × 5 bytes each at addresses `0x000`, `0x005`,
  `0x00A`, ..., `0x04B` (digit `D` is at `0x000 + D × 5`).  Each
  sprite is 4 wide × 5 tall (top 4 bits of each byte).
- **Hi-res** (SCHIP): 16 sprites × 10 bytes each at addresses
  `0x050`, `0x05A`, ..., `0x0E6` (digit `D` is at `0x050 + D × 10`).
  Each sprite is 8 wide × 10 tall (full byte width).

`LD F, Vx` sets `I = Vx × 5`; immediately follow with `DRW Vx', Vy', 5`
to draw the digit at any position.

`LD HF, Vx` (SCHIP) sets `I = 0x50 + Vx × 10`; follow with
`DRW Vx', Vy', A` (the SCHIP convention is also `DXY0` with N=10
implicit, but `DXYA` works in hi-res).

---

## 3. Assembly Source Format

### 3.1 Lines and Comments

One instruction per line.  Whitespace within a line is insignificant
(other than separating the mnemonic from its operands).  Empty lines
are skipped.

`%` introduces a comment that runs to end-of-line.  This is the same
comment character Trix uses, so a `.s` file is also a valid Trix
script that prints nothing -- nice for piping through dual-purpose
tools.

```chip8
% This is a complete program: blink a single pixel forever.
CLS
LD I, 200            % I = address of sprite data (must be set up beforehand)
LD V0, 20            % x = 32
LD V1, 10            % y = 16
DRW V0, V1, 1        % toggle a 1-row sprite at (32, 16)
LD V0, 1E            % delay = 30 frames (~0.5s @ 60Hz)
LD DT, V0
LD V0, DT            % poll DT
SE V0, 0             % skip next if DT == 0
JP 20E               % loop back to "LD V0, DT"
JP 208               % toggle sprite again
```

### 3.2 Numeric Literals

Numeric literals are **hex** by default -- no `0x` prefix needed.  A
literal's width is determined by context, not by the literal's
written length:

- `LD V0, 5`        → `60 05`  (NN context, 8 bits)
- `LD V0, FF`       → `60 FF`  (NN context, 8 bits; high bits truncated if needed)
- `LD I, 200`       → `A2 00`  (NNN context, 12 bits)
- `JP 234`          → `12 34`  (NNN context, 12 bits)
- `DRW V0, V1, F`   → `D0 1F`  (N context, 4 bits)

The assembler does not validate that a literal "fits" in its
contextual width -- `LD V0, FFFF` masks down to the low 8 bits and
emits `60 FF`.  Be deliberate.

### 3.3 Operand Types

| Form         | Type          | Notes                                                 |
| ------------ | ------------- | ----------------------------------------------------- |
| `V0` .. `VF` | Register      | Case-insensitive (`v0`..`vF` work).                   |
| `0` .. `FFF` | Hex literal   | Width inferred from instruction.                      |
| `I`          | Index reg     | Used in `LD I, NNN`, `LD [I], Vx`, `LD Vx, [I]`, etc. |
| `DT`         | Delay timer   | `LD Vx, DT` and `LD DT, Vx`.                          |
| `ST`         | Sound timer   | `LD ST, Vx` only (you can't read ST).                 |
| `K`          | "any key"     | `LD Vx, K`.  Blocks until a key is pressed.           |
| `F`          | Font select   | `LD F, Vx` -- sets I to lo-res font for digit Vx.     |
| `HF`         | Hi-res font   | `LD HF, Vx` (SCHIP) -- sets I to hi-res font for Vx.  |
| `B`          | BCD output    | `LD B, Vx` -- store BCD of Vx at `[I..I+2]`.          |
| `[I]`        | "memory at I" | `LD [I], Vx` and `LD Vx, [I]`.                        |
| `R`          | RPL flags     | `LD R, Vx` and `LD Vx, R` (SCHIP).                    |

Operand separators: commas, parens, and runs of whitespace are
interchangeable.  The assembler normalizes them all to whitespace
and tokenizes.  These all assemble to the same opcode:

```
LD V0, 42
LD V0,42
LD V0 42
LD , V0 , 42
```

Use whatever reads cleanly.  The disassembler always emits the
canonical `MNEMONIC Op1, Op2, Op3` form.

### 3.4 The Disassembler-Compatible Prefix

The disassembler prepends each line with `ADDR: HHLL  ` where
`ADDR` is the instruction's load address (3 hex digits) and `HHLL`
is the encoded opcode (4 hex digits) for documentation:

```
200: 00E0  CLS
202: A22A  LD I, 22A
204: 600C  LD V0, 0C
```

The assembler **tolerates** this prefix -- detection: token 0 ends
with `:` and token 1 is all hex.  When detected, both tokens are
discarded; the rest of the line assembles as if the prefix weren't
there.  This makes `disasm | asm` round-trip byte-for-byte.

You can paste disassembly directly back into a source file, edit
specific instructions, then re-assemble:

```sh
./trix examples/chip8.trx     --disasm snek.ch8 > snek.s
$EDITOR snek.s
./trix examples/chip8-asm.trx snek.s snek-modded.ch8
```

The address column in the prefix is purely documentation; the
assembler doesn't validate it against the instruction's actual load
position.

### 3.5 Pseudo-Directives

| Directive | Bytes | Description |
| --- | --- | --- |
| `??? HHLL` | 2 | Emit the raw two bytes `HH LL`.  The disassembler uses this for unrecognized opcodes and for sprite/data sections (where the bytes happen to disassemble as something nonsensical). |
| `.byte HH` | 1 | Emit a single raw byte.  Used by the disassembler when a ROM's total size isn't a multiple of two (so the last byte can't be paired into a 2-byte word). |

You won't usually write these in hand-authored source.  They exist
so the round-trip `disasm | asm` pipeline is lossless for every
ROM the emulator can load -- including data-only sprite tables
and odd-byte-length ROMs.

---

## 4. Instruction Set

This section gives bit-level encoding and runtime semantics for every
opcode.  In each table, `X`/`Y` are register nibbles (0..15) and
`N`/`NN`/`NNN` are immediate values of 4/8/12 bits respectively.

### 4.1 Control Flow

| Mnemonic     | Encoding | Semantics                                                |
| ------------ | -------- | -------------------------------------------------------- |
| `CLS`        | `00E0`   | Clear the framebuffer to all-off.                        |
| `RET`        | `00EE`   | Pop the call stack into PC.                              |
| `JP NNN`     | `1NNN`   | PC = NNN.                                                |
| `CALL NNN`   | `2NNN`   | Push current PC; PC = NNN.                               |
| `JP V0, NNN` | `BNNN`   | PC = NNN + V0 (computed jump for jump-tables).           |
| `SYS NNN`    | `0NNN`   | Originally invoked a COSMAC VIP host call; ignored here. |

`SYS NNN` is a no-op in every modern interpreter.  Old ROMs sometimes
contain it as a remnant of when it called a routine in the COSMAC
host monitor.  This emulator silently advances PC and doesn't trap.

### 4.2 Conditional Skips

Each skip instruction either advances PC by 2 (no skip) or by 4 (skip
the following instruction):

| Mnemonic     | Encoding | Skip condition                     |
| ------------ | -------- | ---------------------------------- |
| `SE Vx, NN`  | `3XNN`   | Vx == NN                           |
| `SNE Vx, NN` | `4XNN`   | Vx != NN                           |
| `SE Vx, Vy`  | `5XY0`   | Vx == Vy                           |
| `SNE Vx, Vy` | `9XY0`   | Vx != Vy                           |
| `SKP Vx`     | `EX9E`   | key whose number is Vx is held     |
| `SKNP Vx`    | `EXA1`   | key whose number is Vx is NOT held |

**Pattern**: combine with `JP` for if/else.  CHIP-8 has no
fall-through "ELSE" -- you build it from a skip + a JP:

```
SE V0, 5            % "if (V0 != 5)"  (skip the JP if equal)
JP not_five         % then-branch lives here, GOTOs out at not_five
% V0 == 5 fall-through:
LD V1, FF
JP done
not_five:
LD V1, 00
done:
```

(Labels aren't a thing in this assembler -- substitute the absolute
address.  Future revisions may add a label pass.)

### 4.3 Register Loads and Arithmetic

| Mnemonic     | Encoding | Semantics                                |
| ------------ | -------- | ---------------------------------------- |
| `LD Vx, NN`  | `6XNN`   | Vx = NN.                                 |
| `ADD Vx, NN` | `7XNN`   | Vx += NN (modulo 256; **VF unchanged**). |
| `LD Vx, Vy`  | `8XY0`   | Vx = Vy.                                 |

Note that `ADD Vx, NN` does NOT set VF on overflow -- this is
deliberate per the spec; only `ADD Vx, Vy` (8XY4) sets a carry.

### 4.4 ALU Ops (8XY*)

All take two registers and write the result to Vx.  Several have
side-effects on `VF`:

| Mnemonic      | Encoding | Vx becomes       | VF side-effect                          |
| ------------- | -------- | ---------------- | --------------------------------------- |
| `LD Vx, Vy`   | `8XY0`   | Vy               | (none)                                  |
| `OR Vx, Vy`   | `8XY1`   | Vx \| Vy         | (none in modern; reset on COSMAC)       |
| `AND Vx, Vy`  | `8XY2`   | Vx & Vy          | (none in modern; reset on COSMAC)       |
| `XOR Vx, Vy`  | `8XY3`   | Vx ^ Vy          | (none in modern; reset on COSMAC)       |
| `ADD Vx, Vy`  | `8XY4`   | (Vx + Vy) & 0xFF | VF = 1 if sum > 255, else 0             |
| `SUB Vx, Vy`  | `8XY5`   | (Vx - Vy) & 0xFF | VF = 1 if Vx >= Vy, else 0 (NOT borrow) |
| `SHR Vx, Vy`  | `8XY6`   | Vx >> 1          | VF = original LSB of Vx                 |
| `SUBN Vx, Vy` | `8XY7`   | (Vy - Vx) & 0xFF | VF = 1 if Vy >= Vx, else 0              |
| `SHL Vx, Vy`  | `8XYE`   | (Vx << 1) & 0xFF | VF = original MSB of Vx                 |

This emulator follows the **modern (CHIP-48 / SCHIP)** convention for
shifts: `SHR Vx, Vy` shifts `Vx` (ignoring `Vy`).  The original
COSMAC VIP loaded `Vy` into `Vx` first, then shifted -- some test
ROMs care about this difference.  See [§7](#7-common-pitfalls-and-variant-behavior).

### 4.5 The I Register

| Mnemonic    | Encoding | Semantics                |
| ----------- | -------- | ------------------------ |
| `LD I, NNN` | `ANNN`   | I = NNN.                 |
| `ADD I, Vx` | `FX1E`   | I += Vx (modulo 0xFFFF). |

`I` is the only memory-pointer register.  Sprite draws (`DRW`) read
their pixel data starting at `I`; bulk register loads/stores
(`LD [I], Vx` / `LD Vx, [I]`) read/write at `I`; BCD output
(`LD B, Vx`) writes 3 bytes starting at `I`.

`ADD I, Vx` is sometimes used as a poor-man's "indexed load":
position `I` at the base of an array via `LD I, BASE`, then
`ADD I, Vx` to point to element `Vx`.

### 4.6 Random Numbers

| Mnemonic     | Encoding | Semantics                  |
| ------------ | -------- | -------------------------- |
| `RND Vx, NN` | `CXNN`   | Vx = (random byte) AND NN. |

The `AND NN` masks the random byte -- handy for getting a random
value in `[0, 2^k)` where `k` is the number of low bits set in `NN`.

```
RND V0, 3F          % V0 in [0, 64) -- a random column on lo-res screen
```

The PRNG is whatever the host provides; in this emulator it's the
standard Trix `random` operator.  Re-running a ROM produces a
different random stream each time.

### 4.7 Sprite Draw

| Mnemonic | Encoding | Semantics |
| --- | --- | --- |
| `DRW Vx, Vy, N` | `DXYN` | XOR an N-row × 8-column sprite from `[I..I+N-1]` at screen position `(Vx, Vy)`.  Sets `VF = 1` if any pixel went from on to off (collision), else `0`. |

In **hi-res mode (SCHIP)**, `DRW Vx, Vy, 0` (i.e., N=0) is a special
form that draws a 16 × 16 sprite from `[I..I+31]` (32 bytes total, 2
bytes per row).  In lo-res mode `N=0` is undefined per the spec --
this emulator treats it as a no-op draw rather than crashing.

Sprites that wrap past the right or bottom edge of the framebuffer
wrap around to the opposite edge in this emulator (and most modern
ones).  The COSMAC VIP clipped instead.

### 4.8 Keyboard

| Mnemonic   | Encoding | Semantics                                               |
| ---------- | -------- | ------------------------------------------------------- |
| `SKP Vx`   | `EX9E`   | Skip next instruction if key Vx is currently held.      |
| `SKNP Vx`  | `EXA1`   | Skip next instruction if key Vx is NOT currently held.  |
| `LD Vx, K` | `FX0A`   | Block until any key is pressed; Vx = key nibble (0..F). |

`LD Vx, K` is a true block: the emulator decrements PC by 2 each
cycle until a key is detected, so the same instruction re-executes
indefinitely.  Other coroutines (timers, renderer) keep running
during the wait.

In a terminal there's no real "key release" event -- this emulator
fakes one by holding each key bit for `KEY-HOLD-MS` (default 150 ms)
after the keystroke is observed.  ROMs that poll keys at >7 Hz see
the held state; ROMs that poll less often see the key as released
between presses.  Most CHIP-8 games poll every frame and don't care.

### 4.9 Timers

| Mnemonic    | Encoding | Semantics                              |
| ----------- | -------- | -------------------------------------- |
| `LD Vx, DT` | `FX07`   | Vx = current value of the delay timer. |
| `LD DT, Vx` | `FX15`   | Delay timer = Vx.                      |
| `LD ST, Vx` | `FX18`   | Sound timer = Vx.                      |

There is no `LD Vx, ST` -- ST is write-only.

The decrement happens automatically in a separate 60 Hz coroutine.
You don't need to explicitly tick it.

### 4.10 BCD and Memory Bulk Ops

| Mnemonic     | Encoding | Semantics                                                             |
| ------------ | -------- | --------------------------------------------------------------------- |
| `LD F, Vx`   | `FX29`   | I = address of the lo-res font sprite for digit Vx (5 bytes).         |
| `LD HF, Vx`  | `FX30`   | I = address of the hi-res font sprite for digit Vx (10 bytes; SCHIP). |
| `LD B, Vx`   | `FX33`   | Write BCD of Vx at `[I..I+2]`: hundreds, tens, ones.                  |
| `LD [I], Vx` | `FX55`   | memory[I..I+x] = V0..Vx.                                              |
| `LD Vx, [I]` | `FX65`   | V0..Vx = memory[I..I+x].                                              |

In modern interpreters (this one included) `FX55` and `FX65` leave
`I` unchanged.  The original COSMAC VIP advanced `I` by `x+1` after
the bulk transfer.  Some test ROMs check this distinction; if a
ROM relies on the COSMAC behavior it will misbehave here -- patch
the ROM or use a different interpreter.

`LD B, Vx` is the standard way to display a multi-digit decimal
score.  Combined with `LD F, Vx` and three sprite draws it lets you
render the value of a register on screen with a few instructions:

```
LD I, 400           % temporary buffer for BCD digits
LD B, V0            % memory[400..402] = BCD of V0

LD V1, 10           % display x
LD V2, 08           % display y

LD V3, [I]          % V0..V3 = BCD digits ... wait, this clobbers V0
```

(This idiom needs care -- see [§5.7](#57-drawing-a-hex-digit) for a
non-clobbering version.)

### 4.11 Super-CHIP Extensions

| Mnemonic      | Encoding | Semantics                                                                       |
| ------------- | -------- | ------------------------------------------------------------------------------- |
| `SCD N`       | `00CN`   | Scroll display down N rows; top N rows zero-filled.  N >= rows acts like `CLS`. |
| `SCR`         | `00FB`   | Scroll display right 4 columns; leftmost 4 columns zero-filled.                 |
| `SCL`         | `00FC`   | Scroll display left 4 columns; rightmost 4 columns zero-filled.                 |
| `EXIT`        | `00FD`   | Halt the emulator (sets `/halted true`).                                        |
| `LOW`         | `00FE`   | Enter lo-res mode (64 × 32); clears framebuffer.                                |
| `HIGH`        | `00FF`   | Enter hi-res mode (128 × 64); clears framebuffer.                               |
| `DRW Vx,Vy,0` | `DXY0`   | (hi-res only) XOR-draw a 16 × 16 sprite from `[I..I+31]` at `(Vx, Vy)`.         |
| `LD HF, Vx`   | `FX30`   | I = address of hi-res font sprite for digit Vx (10 bytes).                      |
| `LD R, Vx`    | `FX75`   | RPL flags 0..x = V0..Vx.  `x` must be 0..7.                                     |
| `LD Vx, R`    | `FX85`   | V0..Vx = RPL flags 0..x.  `x` must be 0..7.                                     |

All three scroll opcodes shift the entire framebuffer; vacated edge
pixels become zero.  Implemented as a whole-row `get-interval` /
`put-interval` blit into a fresh zero-filled buffer (SCR / SCL) or one
big block-blit for SCD.  Honors current resolution so scrolling in
hi-res shifts the full 128-wide rows.  Each scroll sets `/display-dirty`
so the renderer repaints on its next 60Hz tick.

`EXIT` is the SCHIP-defined "the program is done" instruction.
Without it, a ROM has no way to terminate cleanly and the emulator
runs until ESC.  Some ROMs use `EXIT` from a "press ESC to quit"
handler; others never hit it.

---

## 5. Programming Patterns

### 5.1 Hello, World (the IBM Logo)

The traditional "first ROM" is the IBM logo render.  Here's a
walkthrough of that universally-published test vector.  (The repo ships
no ROMs -- assemble your own from a published byte listing, or fetch
CC0 ROMs with `chip8-roms/fetch-ch8.py`.)

```
CLS                         % blank the screen
LD I, 22A                   % I = address of letter "I" sprite (15 rows tall)
LD V0, 0C                   % x = 12
LD V1, 08                   % y = 8
DRW V0, V1, F               % draw 15-row sprite at (12, 8)
ADD V0, 09                  % x += 9 (advance to next letter)
LD I, 239                   % I = address of letter "B" sprite
DRW V0, V1, F
LD I, 248                   % "M" letter (top half)
ADD V0, 08
DRW V0, V1, F
ADD V0, 04
LD I, 257                   % "M" letter (bottom half)
DRW V0, V1, F
ADD V0, 08
LD I, 266                   % final segment
DRW V0, V1, F
ADD V0, 08
LD I, 275                   % final segment
DRW V0, V1, F
JP 228                      % infinite loop on the JP instruction itself
% sprite data follows at 22A..
```

Six XOR-draws at fixed positions render "IBM" out of pre-positioned
sprite tables.  No timers, no input, no logic -- just memory layout
and sprite addresses.  When everything's right, you see "IBM":

```
            ######## #########   #####         #####            
            ######## ########### ######       ######            
              ####     ###   ###   #####     #####              
              ####     #######     ####### #######              
              ####     #######     ### ####### ###              
              ####     ###   ###   ###  #####  ###              
            ######## ########### #####   ###   #####            
            ######## #########   #####    #    #####            
```

(Captured from an IBM-logo ROM via `--snapshot=30`, blank rows
squeezed for readability.)  When something's wrong, a
single draw lands wrong and the bug is visible at a glance -- which
is why this ROM is the universal first-emulator test.

### 5.2 Frame Pacing via DT

CHIP-8 has no instruction-rate guarantee -- the CPU runs at whatever
the host says.  To pace gameplay to a wall-clock rhythm, set DT to
the desired frame count and spin until it hits zero:

```
LD V0, 0A           % 10 frames = ~166 ms at 60 Hz
LD DT, V0
delay_loop:
LD V0, DT
SE V0, 0
JP delay_loop
% ... continue with the next game tick
```

The `LD V0, DT; SE V0, 0; JP delay_loop` is the canonical idiom.
CPU instructions still execute during the wait, but at our default
500 IPS that's only ~80 wasted instructions per 10-frame delay --
negligible.

### 5.3 Sprite Animation and XOR Erase

XOR drawing means **drawing the same sprite twice at the same
position erases it**.  This makes simple animation trivial:

```
% Initial position (V0 = x, V1 = y)
LD V0, 10
LD V1, 10
LD I, sprite_addr

main_loop:
DRW V0, V1, 5       % draw sprite at (V0, V1)
... wait one frame via DT ...
DRW V0, V1, 5       % erase: same XOR un-draws it
% update position
ADD V0, 01
JP main_loop
```

Collision detection:

```
DRW V0, V1, 5
SE VF, 1
JP no_collision     % skip the next JP only if VF == 1
JP collided
no_collision:
% normal frame ...
```

The `VF == 1` check after `DRW` is how every CHIP-8 game implements
"did the ball hit the paddle / wall / brick?".  No bounding-box
math, no AABB tests -- the framebuffer itself stores the geometry.

### 5.4 Reading the Keypad

Two flavors: **non-blocking** (poll, branch) and **blocking** (wait
for any keypress).

Non-blocking is the gameplay form -- check whether each control
key is currently held, branch accordingly:

```
LD V0, 04           % chip8 key "4" (PC: Q in this emulator's map)
SKNP V0
JP move_up          % key 4 IS held -> jump to move-up handler

LD V0, 06           % chip8 key "6"
SKNP V0
JP move_down

% no movement key held -> idle handler
JP idle
```

Note `SKNP` skips when the key is **not** held -- so `SKNP V0; JP target`
is "if key V0 IS held, jump to target".  It reads backward from
expectation.

Blocking (`LD Vx, K`) is for menu screens and pauses:

```
% Title screen: wait for any key, then start the game.
LD V0, K            % blocks here until a keypress
% V0 now holds the chip8 key number (0..F)
JP start_game
```

While blocking, `DT` and `ST` continue to count down -- handy for
"press any key to start" prompts that also play a beep.

### 5.5 Subroutines

`CALL NNN` and `RET` are the only subroutine primitives.  No
register-window magic, no automatic register saving -- the callee
clobbers whatever it pleases and the caller has to know.  Conventions
emerge ROM-by-ROM.

```
% caller:
LD V0, 05
LD V1, 10
CALL draw_at_v0_v1
% V0, V1 may now contain whatever draw_at_v0_v1 left in them
% (in practice this routine doesn't change V0/V1, but VF is now collision)

% callee:
draw_at_v0_v1:
LD I, sprite_data
DRW V0, V1, 5
RET
```

Recursion works (16 levels deep), but few CHIP-8 games use it.

### 5.6 Random Numbers

`RND Vx, NN` is the only RNG.  Mask common patterns:

```
RND V0, 3F          % V0 in [0, 64)  -- a column on the 64-wide screen
RND V0, 1F          % V0 in [0, 32)  -- a row on the 32-tall screen
RND V0, 0F          % V0 in [0, 16)  -- a chip8 nibble
RND V0, 01          % V0 in {0, 1}   -- a coin flip
```

For non-power-of-two ranges, use rejection or modulo:

```
% V0 in [0, 6) (a die roll)
roll:
RND V0, 07          % V0 in [0, 8)
SE V0, 06           % skip next if V0 == 6
JP not_six_or_seven
JP roll
not_six_or_seven:
SE V0, 07           % skip next if V0 == 7
JP done
JP roll
done:
% V0 in [0, 6)
```

(Modulo would need `LD I, ...; ADD I, V0; LD V0, [I]` against a
lookup table -- usually rejection is cheaper.)

### 5.7 Drawing a Hex Digit

Combine `LD F, Vx` and `DRW`:

```
% Draw the value of V0 (a digit 0..F) at (10, 10):
LD I, ...           % I will be set by LD F
LD V1, 0A           % x = 10
LD V2, 0A           % y = 10
LD F, V0            % I = address of font sprite for V0 (5 bytes)
DRW V1, V2, 5       % draw 5-row sprite at (10, 10)
```

For a full 8-bit value, use `LD B, Vx` to break it into BCD digits,
then draw each:

```
% Display V0 (0..255) as up-to-3 decimal digits at (V1, V2).
%
% Save V0 first because LD Vx, [I] will clobber V0..V2.
LD I, 400           % temporary buffer at 0x400
LD B, V0            % memory[400..402] = hundreds, tens, ones digits

LD V3, [I]          % V0=hundreds, V1=tens, V2=ones, V3 unchanged
% Note: V1 and V2 (our position regs) are now overwritten!
% In real code, save them first or use higher-numbered registers.
```

In practice, score-display code uses high-numbered regs for the BCD
target so position regs aren't clobbered:

```
LD I, 400
LD B, VC            % store BCD of VC
LD V2, [I]          % loads V0=hundreds, V1=tens, V2=ones
% (chose registers so the load doesn't stomp position registers)
```

### 5.8 Sound

`LD ST, Vx` activates the sound timer.  In this emulator the actual
audio is **opt-in** via `--bell`: the terminal bell rings once per
ST > 0 transition (not once per frame -- that would be unbearable).

```
LD V0, 04           % short beep (~67 ms)
LD ST, V0
```

ROMs that don't care about audio just leave ST at zero and run
silently.  ROMs that do care sometimes use ST as an event marker
(beep on hit, beep on score) by setting it briefly each time.

---

## 6. Super-CHIP Programming

A SCHIP program looks like a CHIP-8 program plus some opening
instructions to enter hi-res mode:

```
HIGH                % enter 128x64 mode
CLS                 % implicit, but harmless

LD I, sprite_addr
LD V0, 40           % x = 64 (center of 128-wide screen)
LD V1, 20           % y = 32 (center of 64-tall screen)
DRW V0, V1, 0       % 16x16 sprite -- N=0 in hi-res
```

Key differences:

- `HIGH` and `LOW` switch resolutions.  Each switch clears the
  framebuffer.  Mixing modes mid-game is rare but supported.
- `DRW Vx, Vy, 0` in hi-res draws a 16x16 sprite from 32 bytes at
  `[I..I+31]`.  In lo-res `N=0` is undefined -- don't.
- `LD HF, Vx` points `I` at the hi-res font sprite for digit `Vx`
  (10 bytes).  Useful for bigger score readouts.
- `LD R, Vx` and `LD Vx, R` save/restore up to 8 bytes to RPL flag
  storage.  In a real HP48 these survived power cycles; here they
  reset every run.
- `EXIT` cleanly halts.  Without `EXIT` the program runs until ESC.

The three scroll opcodes (`SCD N`, `SCR`, `SCL`) are fully implemented.
They shift the entire framebuffer (whichever resolution is active) and
zero-fill the vacated edge.  ROMs that use scrolling for gameplay
render those transitions correctly.

---

## 7. Common Pitfalls and Variant Behavior

The CHIP-8 spec has accumulated **interpreter dialects** over four
decades.  This emulator targets the **modern CHIP-48 / SCHIP**
behavior wherever the original COSMAC VIP differs.  Specifically:

| Op            | COSMAC VIP                                    | CHIP-48 / SCHIP (this emulator)  |
| ------------- | --------------------------------------------- | -------------------------------- |
| `8XY1/2/3`    | Reset VF as side-effect.                      | VF unchanged.                    |
| `8XY6 SHR`    | Vx = Vy >> 1 (loads Vy into Vx, then shifts). | Vx >>= 1 (Vy ignored).           |
| `8XYE SHL`    | Vx = Vy << 1.                                 | Vx <<= 1.                        |
| `BNNN JP V0`  | PC = NNN + V0.                                | PC = NNN + V0 (no change here).  |
| `FX55 / FX65` | I += x+1 after bulk transfer.                 | I unchanged.                     |
| `DXYN` wrap   | Often clipped at edges.                       | Wraps modulo (FB-cols, FB-rows). |

Some test ROMs and "interpreter quirk" suites (BC_test, SCTEST,
chip8-test-suite) check these distinctions explicitly.  This
emulator passes the modern-quirk subset; test ROMs that demand
COSMAC VIP behavior will report failures.

Other gotchas:

- **`LD Vx, K` blocks the CPU but not the timers.**  DT and ST
  continue counting.
- **No `LD ST, Vx` read-back.**  ST is write-only.  If you need to
  poll a timer, use DT.
- **`SYS NNN` is a no-op.**  ROMs from the COSMAC VIP era may
  contain `SYS 158` etc. -- safe to ignore.
- **`VF` is a real register too.**  You can `LD V0, VF` and read the
  collision flag back.  But the next arithmetic op will clobber it.
- **`I` is 16 bits, not 12.**  `ADD I, Vx` can push I past `0xFFF`.
  Wrap behavior varies per interpreter; this one masks to 16 bits.

---

## 8. Worked Example: `blink.ch8`

Here's the full source for a small `blink.ch8` demo (ours, CC0), line
by line, with the resulting bytes in the right margin:

```
CLS                                    % 00 E0   clear framebuffer
LD I, 216                              % A2 16   I = sprite addr
LD V0, 10                              % 60 10   x = 16
LD V1, 08                              % 61 08   y = 8
draw:
DRW V0, V1, 8                          % D0 18   XOR-draw 8x8 sprite
LD V0, 20                              % 60 20   delay = 32 frames (~0.5s)
LD DT, V0                              % F0 15   DT = 32
wait:
LD V0, DT                              % F0 07   V0 = current DT
SE V0, 0                               % 30 00   skip next if V0 == 0
JP wait                                % 12 0E   else loop wait
JP draw                                % 12 08   re-draw (XOR toggles)
sprite:                                % 21 6
% 8x8 box outline:                     %
% FF                                   % 11111111
% 81                                   % 10000001
% 81                                   % 10000001
% 81                                   % 10000001
% 81                                   % 10000001
% 81                                   % 10000001
% 81                                   % 10000001
% FF                                   % 11111111
.byte FF                               % FF
.byte 81                               % 81
.byte 81                               % 81
.byte 81                               % 81
.byte 81                               % 81
.byte 81                               % 81
.byte 81                               % 81
.byte FF                               % FF
```

(In the actual ROM, the sprite bytes are encoded in-line as data
without the `.byte` directive; the disassembler emits them as
`??? FF81` etc. when round-tripping.  The above shows the cleaner
hand-written form.)

Total: 22 bytes of code + 8 bytes of sprite = 30 bytes.

To assemble and run:

```sh
./trix examples/chip8-asm.trx blink.s blink.ch8
./trix examples/chip8.trx     blink.ch8
```

ESC quits.  Try `--bell` to hear the (silent here, but try a
sound-using ROM) audio feedback, `--ascii` for non-Unicode terminals.

---

## 9. Tooling Workflow

Three pieces:

| Tool                               | Purpose                                                            |
| ---------------------------------- | ------------------------------------------------------------------ |
| `examples/chip8.trx`               | Emulator.  Loads `.ch8` files; runs interactively or disassembles. |
| `examples/chip8-asm.trx`           | Assembler.  Reads `.s` source; writes `.ch8` binary.               |
| `examples/chip8-roms/fetch-ch8.py` | Fetcher for CC0 ROMs -- none are bundled (see that dir's README).  |

**Round-trip verification.**  Every ROM the emulator loads disassembles
to text that re-assembles back to the same bytes:

```sh
for rom in examples/chip8-roms/*.ch8; do
    ./trix examples/chip8.trx     --disasm "$rom" > /tmp/r.s
    ./trix examples/chip8-asm.trx /tmp/r.s /tmp/r.ch8
    cmp -s "$rom" /tmp/r.ch8 && echo "MATCH $rom" || echo "DIFF  $rom"
done
```

Every ROM produces `MATCH`.  Any future regression in either tool will
break this loop.

**Editing a ROM.**  Disassemble, edit, re-assemble:

```sh
./trix examples/chip8.trx     --disasm examples/chip8-roms/snek.ch8 > snek.s
$EDITOR snek.s               # change a sprite, tweak a constant, etc.
./trix examples/chip8-asm.trx snek.s my-snek.ch8
./trix examples/chip8.trx     my-snek.ch8
```

**Headless test of a new ROM.**  The emulator's `--disasm` flag
doesn't enter raw-mode and runs without a tty -- useful from CI:

```sh
./trix examples/chip8.trx --disasm my-rom.ch8 > /dev/null && echo "disasm OK"
```

If `disasm OK` prints, the ROM at minimum decodes cleanly; whether
it *runs* still requires a real terminal (or a stripped-down
non-interactive variant of `chip8.trx` -- not currently shipped).

---

## 10. Further Reading

- **Cowgod's Chip-8 Technical Reference** -- the canonical spec
  reference, written for ROM authors.  Easily found online.
- **Tobias Langhoff, *Guide to making a CHIP-8 emulator*** (2021) --
  the modern interpreter-author tutorial; covers exactly the
  COSMAC-vs-modern quirks that make test ROMs fail.
- **Erik Bryntse, SCHIP 1.1 specification** (1991) -- the original
  Super-CHIP spec.  Shipped alongside the canonical SCHIP demo
  corpus that this emulator's `*-schip.ch8` ROMs come from.
- **Octo** (johnearnest.github.io/Octo) -- a higher-level CHIP-8
  language and IDE; the assembler here intentionally targets the
  raw mnemonic format used by hardware-style references rather than
  Octo's friendlier syntax.
- **chip8-test-suite** (Timendus/chip8-test-suite) -- the standard
  conformance suite for CHIP-8 quirks and SCHIP behaviors.  This
  emulator passes the modern-quirk subset.
