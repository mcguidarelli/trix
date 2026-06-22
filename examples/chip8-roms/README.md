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

# CHIP-8 + Super-CHIP ROMs

The [`chip8.trx`](../chip8.trx) emulator loads `.ch8` ROM files from this
directory.  **No ROMs are bundled** -- they are
third-party content -- so this directory ships only the fetcher.

For the assembler syntax, instruction set, and a programming guide, see
[`../chip8-manual.md`](../chip8-manual.md).

## Fetching ROMs

[`fetch-ch8.py`](fetch-ch8.py) downloads public-domain ROMs on demand
(stdlib only -- no pip installs):

```sh
./examples/chip8-roms/fetch-ch8.py            # download the whole catalog
./examples/chip8-roms/fetch-ch8.py --list     # show the catalog, download nothing
./examples/chip8-roms/fetch-ch8.py snek tank  # only targets matching these names
./examples/chip8-roms/fetch-ch8.py --force    # re-download even if present
```

Every ROM it fetches comes from John Earnest's
[chip8Archive](https://github.com/JohnEarnest/chip8Archive), whose contents
are released under the **Creative Commons 0 (CC0) "No Rights Reserved"
public-domain dedication** -- genuinely free to redistribute.  The downloads
are git-ignored; only the fetcher is committed.

It deliberately does **not** fetch the 1990s "classic" ROM packs (Pong, Brix,
Space Invaders, the Super-CHIP game ports).  Those are clones of
trademarked franchises whose "public domain" status is asserted but legally
murky -- not ours to redistribute.  Bring your own copies if you want them:
drop any `.ch8` into this directory and the no-argument menu lists it.

## Running

```sh
./examples/chip8-roms/fetch-ch8.py snek                       # fetch one first
./trix examples/chip8.trx examples/chip8-roms/snek.ch8
./trix examples/chip8.trx --ascii  examples/chip8-roms/1dcell.ch8
./trix examples/chip8.trx --disasm examples/chip8-roms/snek.ch8
./trix examples/chip8.trx                                     # interactive menu of fetched ROMs
```

ESC quits.  See `examples/chip8.trx --help` for the full key map and flag
list, or [`../chip8-manual.md`](../chip8-manual.md) for the authoritative
programming reference.

A headless ASCII frame is handy for documenting or smoke-testing a ROM:

```sh
./trix examples/chip8.trx --snapshot=120 examples/chip8-roms/snek.ch8
```

steps the CPU 120 times and dumps the framebuffer as text (`#` on, ` ` off).

## Round-trip verification

Every ROM the emulator loads disassembles to text that re-assembles back to
the same bytes:

```sh
for rom in examples/chip8-roms/*.ch8; do
    ./trix examples/chip8.trx     --disasm "$rom" > /tmp/r.s
    ./trix examples/chip8-asm.trx /tmp/r.s /tmp/r.ch8
    cmp -s "$rom" /tmp/r.ch8 && echo "MATCH $rom" || echo "DIFF  $rom"
done
```

A non-empty diff usually means the assembler doesn't yet recognize some
opcode; the disassembly line that doesn't round-trip is the one to inspect.
Sprite/data sections round-trip via the `??? HHLL` pseudo-mnemonic, and any
odd trailing byte via the `.byte HH` directive.
