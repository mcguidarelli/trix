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

# DWARF Host Introspection

Trix is an embeddable VM: it runs *inside* a host C++ program, and `peek`
/ `poke` already let a script read and write that host's live memory. But
raw memory access only gets you so far. To read a host global you need its
**address**; to make sense of the bytes you read, you need its **type
layout**. Those two facts live in the host binary's *debug information* --
its DWARF -- not in the running code.

`lib/dwarf.trx` answers exactly those two questions:

- **name -> address** -- `dwarf-lookup ctx (g_state)` finds where a global
  or function lives, by name.
- **type -> layout** -- `dwarf-layout ctx type-offset` reports a type's
  size, fields, and offsets, and `dwarf-peek` goes one step further and
  hands back the value already decoded into a typed Trix Record.

The result is **scriptable, type-aware introspection of the host program
Trix is embedded in**: load the host's own ELF, ask for a symbol by name,
and read its live value as a structured Trix value -- all from a script.

This manual serves two readers. The first half (sections 1-6) is for the
**programmer** using the library: the API, recipes, and the rough edges to
know about. The second half (sections 7-11) is for the **maintainer**: a
reader's-eye primer on the DWARF format, how the reader is built, the C++
primitives underneath it, and how it is tested.

---

## Table of contents

1. [What this is, and why](#1-what-this-is-and-why)
2. [Quick start](#2-quick-start)
3. [The two-layer model](#3-the-two-layer-model)
4. [API reference](#4-api-reference)
   - 4.1 [Loading and closing](#41-loading-and-closing)
   - 4.2 [Symbol lookup](#42-symbol-lookup)
   - 4.3 [Type layout](#43-type-layout)
   - 4.4 [Reading values](#44-reading-values)
   - 4.5 [Source lines](#45-source-lines)
5. [Cookbook](#5-cookbook)
   - 5.1 [Dump a type's layout (a tiny `pahole`)](#51-dump-a-types-layout-a-tiny-pahole)
   - 5.2 [Read a live struct from the running process](#52-read-a-live-struct-from-the-running-process)
   - 5.3 [Cross-check addresses against `nm`](#53-cross-check-addresses-against-nm)
   - 5.4 [Map an address back to source (a tiny `addr2line`)](#54-map-an-address-back-to-source-a-tiny-addr2line)
   - 5.5 [List an enum's constants](#55-list-an-enums-constants)
   - 5.6 [Read an array global](#56-read-an-array-global)
   - 5.7 [Disambiguate same-named globals](#57-disambiguate-same-named-globals)
   - 5.8 [Enumerate a function's overloads](#58-enumerate-a-functions-overloads)
   - 5.9 [Introspect a loaded shared object](#59-introspect-a-loaded-shared-object)
   - 5.10 [Bake offsets offline (the embedded story)](#510-bake-offsets-offline-the-embedded-story)
6. [Limits and gotchas](#6-limits-and-gotchas)
7. [The DWARF format: a reader's primer](#7-the-dwarf-format-a-readers-primer)
8. [Maintainer internals](#8-maintainer-internals)
9. [The C++ primitives (Layer 1)](#9-the-c-primitives-layer-1)
10. [Testing](#10-testing)
11. [Build gating and the operator count](#11-build-gating-and-the-operator-count)

---

## 1. What this is, and why

Trix's differentiating identity here is **in-process self-introspection**:
the script and the host C++ application share one address space, so the
target memory is already reachable through `peek`/`peek-bytes`. What was
missing is the *metadata* -- the map from a source-level name to an address,
and from an address to a typed shape. DWARF is that map, and this library
reads it.

A few framing points:

- **It is a peer to the interactive debugger, not a part of it.** The
  [debugger](debugger.md) (`lib/debugger.trx`) operates on a *running Trix
  program* -- Trix-level frames, breakpoints, watches. The DWARF reader
  operates on the *host C++ program* that embeds Trix. They sit at different
  levels and neither depends on the other.
- **The parser is written in Trix.** Only the handful of things a stack
  language genuinely cannot do cheaply or safely -- `mmap` an ELF, copy a
  windowed blob of host memory, decode a LEB128 varint, find a module's load
  bias, and two performance-critical inner loops -- live in C++
  (`src/ops_dwarf.inl`, eight operators). The ELF header walk, compilation-
  unit iteration, abbreviation tables, DIE decoding, type layout, value
  decoding, and the entire public API are Trix (`lib/dwarf.trx`). This keeps
  the self-hosting story intact.
- **Scope.** Linux / ELF64 only (`<elf.h>` is glibc; Mach-O and PE are not
  supported). DWARF versions 2 through 5, 32- and 64-bit DWARF, both byte
  orders. The feature is opt-out at compile time (`-DTRIX_NO_DWARF`, which
  also drops `<sys/mman.h>` / `<link.h>`) and gated at run time under
  `--sandbox` -- like every other raw-memory operator, it raises
  `/unsupported` there.
- **What is *out* of scope.** Local variables, stack unwinding, CFI
  evaluation, and live process control (`ptrace`) are deliberately not here.
  That is the out-of-process, GDB-competitor direction; the chosen identity
  (in-process self-introspection of globals, functions, types, and source
  lines) does not need it. Split DWARF (`.dwo` / `.dwp`) is also out of v1
  and is rejected with a clear error (see [6](#6-limits-and-gotchas)).

---

## 2. Quick start

Load the library with `run`, then load a binary, look something up, and
read it. The path to `lib/dwarf.trx` is resolved relative to the working
directory (or the module search path; see [Running Trix](cli.md)).

```
(lib/dwarf.trx) run                      % define the dwarf-* procedures

% Introspect THIS running trix binary (it must have been built with -g).
executable-path dwarf-load /ctx exch def

% name -> address
ctx (g_some_global) dwarf-lookup /info exch def
info /address get =                      % e.g. 6296784ul  (link-time VA)

% address/type -> a decoded value.  A struct comes back as a Record.
ctx (g_some_global) dwarf-peek /val exch def
val /some_field get =

% a function's entry address, and its source location
ctx (main) dwarf-lookup-fn /fn exch def
fn /address get =                        % DW_AT_low_pc
ctx (main) dwarf-line-fn /row exch def
row /file get =  row /line get =          % e.g. trix.cpp  113

ctx dwarf-close                          % release the mmap'd image
```

**Reading another on-disk binary** works the same way -- pass its path to
`dwarf-load` instead of `executable-path`. The static reads (`dwarf-lookup`,
`dwarf-layout`, `dwarf-peek`) work on any ELF on disk; only `dwarf-peek-live`
requires that the binary be a module mapped in *this* process.

> **The optimized `trix.opt` has no debug info.** A release build is
> compiled without `-g`, so `executable-path dwarf-load` on `trix.opt`
> finds no `.debug_info` and throws. To introspect Trix itself with the
> optimized binary, load a separately-built **debug** `./trix` *by path*:
> `(./trix) dwarf-load`. Loading the ~140K-DIE engine binary wants headroom
> -- run with `--vm-size=268435456` (256 MiB) or so (see
> [6](#6-limits-and-gotchas)).

---

## 3. The two-layer model

### L1 -- the C++ shim (`src/ops_dwarf.inl`)

Eight operators supply the primitives a stack language cannot do cheaply or
safely: `dwarf-open` (`mmap` a file read-only), `dwarf-munmap`,
`peek-bytes` (copy an N-byte window of host memory into a VM string for
`unpack`), `leb128-decode`, `module-load-bias` / `module-load-bias-for`
(the PIE/ASLR runtime slide), and the two fused inner loops `dwarf-read-die`
and `dwarf-line-lookup`. See [9](#9-the-c-primitives-layer-1) for each one's
stack effect. `peek-bytes` and `leb128-decode` are generally useful well
beyond DWARF.

### L2 -- the Trix reader (`lib/dwarf.trx`)

Everything else: the ELF section table walk, CU iteration across DWARF
versions, abbreviation handling, DIE decoding, type layout, value decoding,
the line-table driver, and the public API.

### Storage is lazy, by necessity

A single Trix container caps at **65535 elements** (`length_t` is
`uint16_t`; this cap is load-bearing in the object schema and snapshot
format and never widens). A real binary has far more DIEs than that -- the
trix debug binary alone is **~140K DIEs in one compilation unit** -- so the
reader can never materialize the whole DIE tree into one structure.

Instead, `dwarf-load` retains only two small things:

- a **paged name index** -- every variable with a static address and every
  function with an entry address, hashed into 64 fixed sub-dictionaries
  (capacity ~= 64 x 65535), so a DIE-dense C++ binary stays under the
  per-dict cap; and
- a **per-CU descriptor table** -- a handful of section offsets per
  compilation unit.

Type layout is re-parsed *on demand* from the `mmap`'d bytes: `dwarf-layout`
walks just the requested type's subtree when you ask for it. **Resident
memory scales with the number of globals and functions, not the number of
DIEs.**

### Two inner loops are fused into C++

Per-DIE parsing (abbreviation lookup plus form decoding) ran ~15-20
interpreted operations *per attribute*; on a real binary that interpreter
dispatch overhead -- not allocation -- dominated a load (~64% of one
profile). So one C++ operator, `dwarf-read-die`, parses a single DIE given a
per-CU "scope" array, and the Trix walks drive it. This cut the engine-
binary load from **~70s to ~0.4s**. The line-number program's opcode loop is
fused the same way into `dwarf-line-lookup`. Both are pure optimizations of
logic that is otherwise expressible in Trix; they change no semantics.

---

## 4. API reference

Every public entry point is a `dwarf-` procedure defined by
`(lib/dwarf.trx) run`. Stack effects use the usual Trix notation
(`inputs -- outputs`).

### 4.1 Loading and closing

```
dwarf-load   path  -- ctx        open an ELF + index its DWARF
dwarf-close  ctx   --            release the ctx's mmap'd image (idempotent)
```

`dwarf-load` opens the ELF at `path` read-only, validates it
(`/invalid-image-file` if it is not a 64-bit ELF, has no `.debug_info`, or
has a malformed compilation unit), indexes every CU, and returns an opaque
**context dict** (`ctx`) that every other call takes as its first argument.
The context owns the `mmap`'d image and the name index.

`dwarf-close` `munmap`s the image and invalidates the context. After it,
`dwarf-layout` / `dwarf-peek*` raise `/undefined` (cleanly, rather than
faulting) because their backing bytes are gone; `dwarf-lookup*` still return
cached infos, but those infos can no longer be peeked. Closing an already-
closed or never-loaded context is a no-op. Close contexts you no longer need
if your program loads many ELFs, so you do not leak address space.

### 4.2 Symbol lookup

```
dwarf-lookup        ctx name -- info | null      one global by name
dwarf-lookup-all    ctx name -- [ info ... ]     ALL globals sharing a name
dwarf-lookup-fn     ctx name -- info | null      one function by name
dwarf-lookup-fn-all ctx name -- [ info ... ]     ALL functions sharing a name
```

All four are O(1) hashed-index lookups. They return the **shared, stored**
info dict -- treat it as read-only; do not mutate it.

An **info dict** has this shape:

```
<< /name          (g_state)     % the DW_AT_name (a String)
   /address       6296784ul     % link-time VA: DW_OP_addr for a global,
                                 % DW_AT_low_pc for a function (a ULong)
   /type-offset   12345         % .debug_info offset of the type DIE
                                 % (for a function, the RETURN type; null = void)
   /cu-name       (foo.cpp)      % the defining CU's source file
   /qualified-name (ns::C::name) % present only on a namespace/class-scoped name
>>
```

Notes:

- **Only defined symbols are indexed.** A global needs a static location
  (`DW_OP_addr`); a function needs a body (`DW_AT_low_pc`). Pure
  declarations are skipped.
- **Bare *or* qualified names resolve.** A namespace- or class-scoped name
  is indexed under both its bare `name` and its qualified `ns::Class::name`
  form, so either query finds it. The qualified form disambiguates; a bare
  query that collides resolves to the **last** definition indexed.
- **Same-name collisions are preserved.** Cross-CU file-static globals,
  per-function statics (think a repeated `__PRETTY_FUNCTION__`), and
  function overloads all share a name. `dwarf-lookup` / `dwarf-lookup-fn`
  return the newest; the `-all` variants return *every* match, newest first.
  `/cu-name` and `/address` tell them apart, and `dwarf-peek-info`
  (below) reads a specific one.

### 4.3 Type layout

```
dwarf-layout  ctx type-offset -- layout
```

Resolves a type DIE (by its `.debug_info` offset -- e.g. an info's
`/type-offset`) to a **layout dict**, following typedef / `const` /
`volatile` / `restrict` / `_Atomic` qualifier chains first. Nested
aggregate and array members are expanded recursively in the same call,
bounded to 16 levels so pathological or cyclic DWARF cannot recurse forever.

The shape depends on `/kind`:

```
% base type / enum (a scalar)
<< /kind /base|/enum  /name  /byte-size  /encoding  /peek-type
   /type-offset  /resolved-type-offset
   [/enumerators << /RED 0 /GREEN 1 ... >>]   % enums only (L-11)
>>

% pointer / reference / rvalue-reference (a pointer-width address)
<< /kind /pointer|/reference|/rvalue-reference  /byte-size 8
   /peek-type /ulong-type ... >>

% struct / union
<< /kind /struct|/union  /name  /byte-size
   /type-offset  /resolved-type-offset
   /members [ << /name /offset /type-offset /resolved-type-offset
                 /kind /byte-size /encoding /peek-type
                 [/bit-size /bit-offset]    % bitfield members
                 [/layout << ... >>]        % nested aggregate/array member
              >> ... ] >>

% array
<< /kind /array  /count N  /counts [ d1 d2 ... ]  /byte-size
   /type-offset  /resolved-type-offset
   /element-layout << recursive layout of the element type >> >>
```

- `/peek-type` is the Trix scalar tag the value decodes to (`/integer-type`,
  `/double-type`, `/ulong-type`, ...), or `/unknown` when the
  (encoding, width) pair has no Trix counterpart.
- An **array**'s `/count` is the flat element total (row-major product of
  the per-dimension `/counts`); a flexible or unknown dimension yields 0.
- A **bitfield** member carries `/bit-size` and `/bit-offset` (the absolute
  bit anchor from the struct start); its `/offset` is then the byte holding
  the field's first bit.

### 4.4 Reading values

```
dwarf-peek       ctx name -- value | null    static (file-image) value
dwarf-peek-live  ctx name -- value | null    live (this-process) value
dwarf-peek-info  ctx info -- value | null    static value of a specific info
```

`dwarf-peek` resolves a global by name, reads its bytes, and decodes them by
its type layout into a typed Trix value:

- a **struct / union** becomes a **Record** (field name -> value); if any
  member is nameless (e.g. an anonymous union) it becomes a **Dict** instead,
  since a Record schema cannot carry an unnamed field;
- a **base type / enum / pointer / reference** becomes the matching scalar;
- an **array** becomes a Trix array of decoded elements;
- nested aggregates and array elements recurse;
- an undecodable field (an unmapped scalar, e.g. `long double`) comes back
  as `null` rather than failing the whole peek.

The byte **source** is what differs:

- **`dwarf-peek` / `dwarf-peek-info`** read the **static file-image** bytes
  -- the link-time initializer in the ELF on disk. This works on *any* ELF,
  not just self. A `.bss` global (`SHT_NOBITS`, no file bytes) reads as zero,
  which is its load-time value.
- **`dwarf-peek-live`** reads the **current** bytes from *this* process's
  memory, at the link-time address relocated by the load bias of the module
  the context was loaded from -- the running executable *or* a shared object
  loaded into this process (keyed off the context's load path). This is
  self-introspection only: if that module is not mapped in this process, the
  relocation has no valid base and the peek returns `null` rather than
  reading a bogus address.

Use `dwarf-peek-info` with an entry from `dwarf-lookup-all` to read a
*specific* one of several same-named globals, which `dwarf-peek` (by name)
would never reach because it only sees the last.

Scalar decoding (L-15 widths): a 2-byte `short` / `unsigned short` widens to
a Trix `Integer` / `UInteger`; `__int128` / `unsigned __int128` decode to the
native 16-byte wide types; an 80-/128-bit `long double` has no Trix scalar
and yields `/unknown` -> `null`.

### 4.5 Source lines

```
dwarf-line     ctx addr -- row | null     map a link-time PC to a source row
dwarf-line-fn  ctx name -- row | null     a function's source location
```

`dwarf-line` scans each CU's `.debug_line` program (reached via the CU
root's `DW_AT_stmt_list`) for the row whose `[address, next-address)` range
covers the link-time PC `addr` (a ULong), and returns:

```
<< /file (trix.cpp)   % directory-joined source path (decoded v2-v5)
   /line 113
   /column 5
   /is-stmt true
   /address 4198400ul  % the row's start PC
>>
```

It returns `null` when no row covers `addr` or the binary carries no
`.debug_line`. `dwarf-line-fn` is the convenience: it resolves a function by
name and maps its entry `low_pc` to its source row.

---

## 5. Cookbook

Complete, runnable recipes. Each assumes the library is loaded and a context
is open:

```
(lib/dwarf.trx) run
(./trix) dwarf-load /ctx exch def        % a debug build, by path
% ... recipe ...
ctx dwarf-close
```

A few conventions used below: `=` prints a value and a newline, `print`
prints without one; iteration is `for-all` (`container { body } for-all`),
where an array body sees one element and a dict/record body sees `name value`;
record and dict fields are read with `name get`.

### 5.1 Dump a type's layout (a tiny `pahole`)

Walk a global's struct layout and print each field's byte offset, name, and
kind -- the DWARF answer to `offsetof`:

```
ctx (g_state) dwarf-lookup /type-offset get /toff exch def
ctx toff dwarf-layout /lay exch def

(struct ) print  lay /name get =          % e.g. struct SensorState
lay /members get {
    /m exch def
    (  +) print  m /offset get print
    (  ) print   m /name get print
    ( : ) print  m /kind get =            % /base /pointer /struct /array ...
} for-all
```

A member whose `/kind` is `/struct`, `/union`, or `/array` also carries a
recursive `/layout`, so you can descend into nested aggregates from the same
dict without another `dwarf-layout` call.

### 5.2 Read a live struct from the running process

`dwarf-peek-live` returns the *current* value, decoded by type. A struct
comes back as a Record, so its fields read like any record:

```
ctx (g_state) dwarf-peek-live /s exch def
s null eq not {
    (temperature = ) print  s /temperature get =
    (mode        = ) print  s /mode get =          % an enum decodes to its int
}
```

Call it again later and you get the value as it is *then* -- this reads live
memory, not a cached snapshot. (`dwarf-peek` instead reads the static
file-image initializer, which works on any on-disk ELF, not only self.)

### 5.3 Cross-check addresses against `nm`

A parsed `/address` is the link-time VA, so it should equal `nm`'s symbol
value (build the target `-fno-pie -no-pie` so `DW_OP_addr` is absolute):

```
ctx (g_state) dwarf-lookup /address get =     % compare to: nm BIN | grep ' g_state$'
ctx (main)    dwarf-lookup-fn /address get =  % a function's DW_AT_low_pc
```

### 5.4 Map an address back to source (a tiny `addr2line`)

Given a link-time PC, find the source row; or go straight from a function
name to its definition site:

```
% by name (resolves the function, then maps its entry low_pc):
ctx (main) dwarf-line-fn /r exch def
r null eq not { r /file get print (:) print r /line get = }   % e.g. trix.cpp:113

% by raw PC (e.g. one captured elsewhere):
ctx 4198400ul dwarf-line /r2 exch def
r2 null eq not { r2 /file get print (:) print r2 /line get = }
```

### 5.5 List an enum's constants

`dwarf-layout` of an enum carries an `/enumerators` dict (name -> value):

```
ctx (g_mode) dwarf-lookup /type-offset get /toff exch def
ctx toff dwarf-layout /lay exch def
lay /kind get /enum eq {
    lay /enumerators get {
        /v exch def  /n exch def          % dict body: name value
        n print  ( = ) print  v =
    } for-all
}
```

### 5.6 Read an array global

An array global peeks into a Trix array of decoded elements:

```
ctx (g_table) dwarf-peek /arr exch def    % e.g. int g_table[3] = {10,20,30}
arr length =                              % 3
arr { = } for-all                         % 10 / 20 / 30
```

Multi-dimensional arrays flatten row-major; `dwarf-layout`'s `/counts` holds
the per-dimension extents and `/count` the flat total.

### 5.7 Disambiguate same-named globals

When several globals share a name (cross-CU file statics, per-function
statics), `dwarf-lookup` only reaches the last. `dwarf-lookup-all` returns
them all; `dwarf-peek-info` reads a specific one:

```
ctx (s_counter) dwarf-lookup-all {
    /i exch def
    i /cu-name get print  ( -> ) print  ctx i dwarf-peek-info =
} for-all
```

### 5.8 Enumerate a function's overloads

```
ctx (Vector::dot) dwarf-lookup-fn-all {
    /f exch def
    f /qualified-name known? { f /qualified-name get } { f /name get } if-else print
    (  @ ) print  f /address get =
} for-all
```

### 5.9 Introspect a loaded shared object

Load the `.so`'s own DWARF by path, then live-peek its globals --
`module-load-bias-for` relocates by *that* module's load base, not the
executable's, so this works for a plugin mapped into the process:

```
(/path/to/libplugin.so) dwarf-load /so exch def
so (g_plugin_state) dwarf-peek-live /st exch def
st null eq { (plugin not mapped in this process) = } { st = } if-else
so dwarf-close
```

### 5.10 Bake offsets offline (the embedded story)

Because the static reads work on any on-disk ELF, you can run the reader on
the dev host against a target binary and emit a plain Trix data file of just
the addresses and offsets you need -- then ship *that* to a device that never
parses DWARF at run time:

```
% takes the target binary as argv[0]; prints a Trix dict literal to stdout
(lib/dwarf.trx) run
command-line-args 0 get dwarf-load /ctx exch def
(<< ) print
[ (g_state) (g_config) (g_table) ] {
    /nm exch def
    /info ctx nm dwarf-lookup def
    info null eq not { (/) print nm print ( ) print info /address get print (  ) print } if
} for-all
(>>) =
ctx dwarf-close
```

---

## 6. Limits and gotchas

- **Load the target by path; the optimized binary has no `-g`.**
  `executable-path dwarf-load` only works when the running binary carries
  debug info. The release `trix.opt` does not, so introspect Trix by loading
  a debug `./trix` *by path*. `dwarf-peek-live` additionally requires that
  the loaded module be the *running* process (or a `.so` mapped into it).
- **Give big binaries room.** Indexing the ~140K-DIE engine binary churns
  through a lot of transient per-DIE dicts; run with `--vm-size=268435456`
  (256 MiB) or similar. On the *debug* (ASan, `-O0`) build a self-load is
  also slow (~5s); the optimized `trix.opt` reader is far faster.
- **Split DWARF is rejected.** A binary built with `-gsplit-dwarf` keeps its
  real DIEs in a separate `.dwo` file that is typically absent at run time,
  so `dwarf-load` detects the skeleton (a DWARF5 `DW_UT_skeleton` unit, or a
  pre-v5 root carrying `DW_AT_GNU_dwo_name`) and throws `/unsupported` with a
  clear message rather than silently returning empty lookups. Rebuild the
  target with ordinary `-g` (no `-gsplit-dwarf`).
- **`long double` is undecodable.** There is no Trix 80-/128-bit float, so it
  peeks as `null`. Integer, float (`float`/`double`), pointer, enum, struct,
  union, array, and bitfield are all supported.
- **Linux / ELF64 only.** Mach-O and PE are not supported. Under
  `-DTRIX_NO_DWARF` the operators stay registered but raise `/unsupported`.
- **Sandbox.** Under `--sandbox`, the raw-memory operators raise
  `/unsupported`, like `peek`/`poke`/`alloc`.
- **No locals, no unwinding, no `ptrace`.** Globals, functions, types, and
  source lines only -- see [1](#1-what-this-is-and-why).

---

## 7. The DWARF format: a reader's primer

This is a working map of DWARF *as this reader consumes it* -- not a
restatement of the standard. Each subsection points at the Trix procedure
that uses it. For the full specification, see
[dwarfstd.org](https://dwarfstd.org/).

### 7.1 ELF and the `.debug_*` sections

DWARF lives in named ELF sections. `dwarf-load` walks the ELF64 section-
header table (`dwarf--find-section`) and locates the ones the reader uses:

| Section              | Holds                                          |
| -------------------- | ---------------------------------------------- |
| `.debug_info`        | the DIE tree -- the heart of DWARF             |
| `.debug_abbrev`      | abbreviation tables that decode `.debug_info`  |
| `.debug_str`         | strings referenced by `DW_FORM_strp`           |
| `.debug_line_str`    | strings referenced by `DW_FORM_line_strp` (v5) |
| `.debug_str_offsets` | the indirection table for `DW_FORM_strx` (v5)  |
| `.debug_addr`        | the indirection table for `DW_FORM_addrx` (v5) |
| `.debug_line`        | the line-number programs                       |

`.debug_info` and `.debug_abbrev` are mandatory; the rest are optional and
their absence simply disables the features that need them (e.g. no
`.debug_line` -> `dwarf-line` returns `null`).

### 7.2 Compilation units and the CU header

`.debug_info` is a sequence of **compilation units**, one per translation
unit. `dwarf-load` iterates them. Each begins with a header:

- an **initial length**. If the first 4 bytes are `0xffffffff`, this is
  **64-bit DWARF**: an 8-byte length follows and intra-section offsets are
  8 bytes. Otherwise it is **32-bit DWARF**: 4-byte length, 4-byte offsets.
  (This is independent of whether the ELF is 32- or 64-bit; the reader
  requires ELF64 but handles both DWARF offset sizes -- the free variable
  `dw-offset-size` carries it.)
- a **version** (2-5).
- in **v5**: a `unit_type` byte, then `address_size`, then the abbrev-table
  offset. A `unit_type` of `DW_UT_skeleton` (4) or `DW_UT_split_compile` (5)
  marks split DWARF and carries an *extra* 8-byte `dwo_id` -- the reader
  rejects these up front (see [7.9](#79-split-dwarf-and-why-its-rejected)).
- in **v2-v4**: the abbrev-table offset, then `address_size` (no
  `unit_type`).

The DIEs start immediately after the header. Getting the header size right
per version and offset size is what positions the first DIE correctly.

### 7.3 The abbreviation table

`.debug_info` is dense: a DIE does not name its tag and attributes inline.
Instead each DIE begins with an **abbreviation code** -- an index into the
CU's abbreviation table (`.debug_abbrev`, at the CU's abbrev offset). An
abbreviation entry declares the DIE's `DW_TAG`, whether it has children, and
a list of `(attribute, form)` pairs. To decode a DIE you read its abbrev
code, find the matching entry, and then read each attribute's value in the
declared **form**. A code of 0 is a null entry -- it terminates a sibling
chain (and, by ascending, closes a subtree).

### 7.4 DIEs: tags, attributes, forms

A **DIE** (Debugging Information Entry) is the universal node. Its
`DW_TAG_*` says what it is; its `DW_AT_*` attributes carry the details; each
attribute's `DW_FORM_*` says how its value is encoded. The tags this reader
acts on include:

| Tag (number) | Meaning |
| --- | --- |
| `compile_unit` (17) | the CU root; carries `*_base` and `stmt_list` |
| `variable` (52) | a global (indexed if it has a location) |
| `subprogram` (46) | a function (indexed if it has `low_pc`) |
| `base_type` (36) | int / float / char ... |
| `enumeration_type` (4) | an enum (`enumerator` (40) children) |
| `structure_type` (19), `union_type` (23) | aggregates (`member` (13) children) |
| `array_type` (1) | arrays (`subrange_type` (33) children) |
| `pointer_type` (15), `reference_type` (16), `rvalue_reference_type` (66) | pointer-width |
| `typedef` (22), `const_type` (38), `volatile_type` (53), `restrict_type` (55), `atomic_type` (71) | qualifier chains, stripped |
| `namespace` (57), `class_type` (2) | open a lexical scope for qualified names |

Key attributes: `DW_AT_name` (3), `DW_AT_byte_size` (11),
`DW_AT_encoding` (62), `DW_AT_location` (2), `DW_AT_low_pc` (17),
`DW_AT_data_member_location` (56), `DW_AT_type` (73),
`DW_AT_specification` (71)/`DW_AT_abstract_origin` (49) (out-of-line
definitions point back to their declaration), `DW_AT_upper_bound` (47) /
`DW_AT_count` (55) (array dimensions), `DW_AT_bit_size` (13) +
`DW_AT_data_bit_offset` (107) or legacy `DW_AT_bit_offset` (12) (bitfields),
`DW_AT_const_value` (28) (enumerators), `DW_AT_stmt_list` (16) (line
program). The fused `dwarf-read-die` operator decodes a DIE's tag,
children flag, and these attributes in one call.

### 7.5 String and address indirection (the v5 tail)

Pre-v5 DWARF (and GCC at v5) inlines strings via `DW_FORM_strp` (a direct
offset into `.debug_str`) and addresses via `DW_FORM_addr`. Clang at v5
prefers **indirected** forms: `DW_FORM_strx` is an *index* into
`.debug_str_offsets` (which then points into `.debug_str`), and
`DW_FORM_addrx` is an index into `.debug_addr`. The base offsets for those
tables come from the CU root's `DW_AT_str_offsets_base` /
`DW_AT_addr_base`, which is why `dwarf--index-cu` reads the CU root *first*
and folds those bases into the per-CU descriptor before resolving any other
DIE. This indirection is the long tail of DWARF version skew, and it is
exercised specifically by the clang-v5 test leg.

### 7.6 Type description

`dwarf-layout` builds its layout dict from the type DIEs:

- **Qualifiers and typedefs** (`typedef`, `const`, `volatile`, `restrict`,
  `atomic`) are *stripped* first (`dwarf--strip-type`) to the underlying
  type -- a `typedef`'d struct resolves to the struct; a `const int` to the
  int.
- A **base type** carries `DW_AT_encoding` (5 signed, 7 unsigned, 4 float,
  2 boolean, 6/8 signed/unsigned char) and `DW_AT_byte_size`; the reader
  keys `(encoding, width)` to a Trix scalar tag.
- A **struct/union** member's `/offset` comes from
  `DW_AT_data_member_location` (a plain ULEB on modern producers, or a
  `DW_OP_plus_uconst` location expression on older ones).
- An **array**'s element count is `DW_AT_upper_bound + 1` (or `DW_AT_count`)
  from each `subrange_type` child; GCC omits the array's `DW_AT_byte_size`,
  so the reader derives it from count x element size.
- A **bitfield** uses modern `DW_AT_data_bit_offset` (byte-order
  independent) when present, else legacy `DW_AT_bit_offset`, which is
  big-endian-natural -- it counts bits to the *left* of the field's MSB
  within the storage unit. The reader normalizes both into one absolute bit
  anchor, flipping the legacy form on little-endian (see
  [8.6](#86-on-demand-type-re-parse)).
- An **enum** maps each `enumerator` child's `DW_AT_name` to its signed
  `DW_AT_const_value`.

### 7.7 The line-number program

`.debug_line` does not store a table; it stores a **bytecode program** that,
when run, emits rows of a `(address -> file:line:column)` matrix. Each CU
root's `DW_AT_stmt_list` is the offset of its program. The program header
(different between v2-v4 and v5) declares the directory and file-name tables;
v5 uses entry-format descriptors and 0-based file indices, v2-v4 use NUL-
terminated lists and 1-based indices. The program body is a state machine
over special, standard, and extended opcodes that advance an address/line
cursor and "emit a row" at intervals. The reader runs this entire loop in the
fused `dwarf-line-lookup` operator -- with no row array materialized
(constant memory) -- returning only the row that covers the requested PC.

### 7.8 Endianness

The ELF's `EI_DATA` byte selects byte order; `dwarf-load` sets the free
variable `dw-endian` to `(<)` (little) or `(>)` (big) and every multi-byte
read prefixes its `unpack` format with it. Bitfield extraction differs by
order: on big-endian the window is assembled MSB-first and the field is
right-justified; the legacy `bit_offset` anchor is also big-endian-natural.
A hand-built big-endian fixture exercises all of this (there is no assumed BE
cross-toolchain).

### 7.9 Split DWARF (and why it's rejected)

`-gsplit-dwarf` moves the bulk of the DIEs out of the main binary into a
companion `.dwo` (or a bundled `.dwp`), leaving only a thin **skeleton** CU
behind. That `.dwo` is usually not present at run time, so a reader that
parsed the skeleton would find almost no real DIEs and return empty lookups
-- a silent, confusing failure. The reader instead detects the skeleton (a
v5 `DW_UT_skeleton` / `DW_UT_split_compile` unit type, caught at the CU
header before any offset math; or a pre-v5 root carrying
`DW_AT_GNU_dwo_name`) and throws `/unsupported` with guidance to rebuild
without `-gsplit-dwarf`. Full `.dwo`/`.dwp` support is out of v1.

---

## 8. Maintainer internals

This section is the map for someone modifying `lib/dwarf.trx`.

### 8.1 The paged indices (`pmap` / `imap`)

Because one Trix dict caps at 65535 entries, both name and offset indices are
**paged** into 64 fixed sub-dictionaries:

- **`dwarf--pmap-*`** (name -> info) keys on a cheap O(1) hash of a name's
  first byte, last byte, and length. On a name **collision** the bucket
  becomes a cons link `[info prev]` whose head is the latest -- so even a
  heavily repeated name (a per-function `__PRETTY_FUNCTION__` static) stays
  O(1) per insert. `dwarf--chain-to-array` flattens a chain for the `-all`
  queries.
- **`dwarf--imap-*`** (offset -> value) keys on `offset mod buckets`. It is
  CU-local and used during indexing to record a scoped declaration's
  qualified name by its offset, so an out-of-line definition can recover it
  via `DW_AT_specification`.

### 8.2 Per-CU descriptor table and the scope array

`ctx /cus` is a small table of per-CU descriptors, each holding the CU's
section-offset range (`/cu-base`, `/hi`), its first DIE (`/die-start`), its
abbrev base, offset size, and the (possibly defaulted, then overwritten)
`str_offsets_base` / `addr_base`. `dwarf--cu-for` finds the CU containing an
offset; `dwarf--scope-for` packs a CU's descriptor into the fixed-order
array the C++ DIE parser consumes.

### 8.3 The `dwarf-read-die` contract

`dwarf-read-die` takes `(scope-array, offset)` and returns
`(die-dict, next-offset, has-children)`. The scope array's index order **is
the operator's contract** and must not be reordered without changing the C++
in lockstep:

```
0 di-base   1 ds-base   2 ls-base   3 big?   4 cu-base
5 off-size  6 abbrev-base   7 str-offsets-base   8 addr-base
```

`dwarf--die-at` is the on-demand single-DIE convenience wrapper over it.

### 8.4 Qualified-name resolution

While `dwarf--index-cu` descends the DIE tree it tracks a **lexical scope
stack** (`scope-names` / `scope-depths` / `nscopes`): a named `namespace`,
`class`, `struct`, or `union` with children pushes its name; a null sibling
terminator that returns the walk to that depth pops it. A located definition
inside a scope is qualified inline. The hard case is the **out-of-line
definition** (clang emits a namespace member's declaration in place but its
located definition at file scope, sometimes *before* the declaration is
walked): such a definition is bare-indexed immediately and **deferred** into
`pending`, then `dwarf--resolve-pending` attaches its qualified key once the
CU walk is complete and `by-decl-off` (the offset -> qualified-name `imap`)
is filled. `dwarf--spec-target` / `dwarf--die-name` / `dwarf--die-type`
follow `DW_AT_specification` / `DW_AT_abstract_origin` so a nameless
definition still gets its name and type.

### 8.5 Dynamic scoping of parse state

`dw-endian`, the section cursors, and the `*_base` values are read as **free
variables** that resolve up the dict stack from the active frame -- Trix
dynamic scoping, not module-global state. This is why, for example,
`dwarf--decode-scalar` sees the right byte order without it being threaded
through every call, and why `dwarf--peek-lay` *binds* the result of a scalar
decode rather than tail-calling (a tail call would pop the frame that holds
`dw-endian`).

### 8.6 On-demand type re-parse

Layout is never stored; it is rebuilt from the `mmap`'d bytes on each
`dwarf-layout` call by `dwarf--layout-d` (depth-bounded to 16). It dispatches
on tag and delegates: `dwarf--members-at` (struct/union members, including
bitfield anchoring and the LE/BE flip), `dwarf--array-info` (element type +
per-dimension counts), `dwarf--enumerators-at` (enum name -> value), and
`dwarf--describe-type` (the scalar/pointer/aggregate classification, via the
`dwarf--tag-kind` dispatch table). Value decoding (`dwarf--peek-lay`) then
walks the *expanded* layout -- nested members and array elements decode via
their pre-attached `/layout` / `/element-layout` with no re-derivation.

### 8.7 Extending the reader

- **A new base-type width** (a new (encoding, width) pair): add an entry to
  `dwarf--peek-type` and, if it needs a new unpack format, to
  `dwarf--peek-fmt`.
- **A new type kind / tag**: add the tag to the `dwarf--tag-kind` dispatch
  table and handle its `/kind` in `dwarf--describe-type` / `dwarf--layout-d`
  / `dwarf--peek-lay`. Because dispatch is table-driven with a `/other`
  fallback, an unmapped tag degrades gracefully rather than throwing.
- **A new attribute or form**: forms are decoded in C++ (`dwarf-read-die`);
  a genuinely new form means touching `dwarf_read_form` in `ops_dwarf.inl`
  and mapping the attribute number in `dwarf_attr_key`.

---

## 9. The C++ primitives (Layer 1)

Eight operators in `src/ops_dwarf.inl`, all POSIX-only. They are compile-
gated by `-DTRIX_NO_DWARF` (which drops the bodies and the `<sys/mman.h>` /
`<link.h>` includes, leaving registered stubs that raise `/unsupported`) and
run-time gated under `--sandbox`.

| Operator | Stack effect | What it does |
| --- | --- | --- |
| `dwarf-open` | `path -- base size` | `mmap` a file read-only; push its base Address and byte size |
| `dwarf-munmap` | `base size --` | release a `dwarf-open` mapping (`dwarf-close` wraps this) |
| `peek-bytes` | `addr n -- str` | copy `n` bytes from a host address into a fresh VM string (for `unpack`) |
| `leb128-decode` | `addr signed? -- value next-addr` | decode one ULEB128 / SLEB128 varint; push the integer and the next addr |
| `module-load-bias` | `-- bias` | the main executable's runtime slide (0 non-PIE, the ASLR slide for PIE) |
| `module-load-bias-for` | `path -- bias found?` | the load bias of the module whose basename matches `path` (exe or `.so`) |
| `dwarf-read-die` | `scope off -- die-dict next-off has-children` | parse one DIE (fused; the per-DIE loop in C++) |
| `dwarf-line-lookup` | `params addr -- row, or null` | run one CU's `.debug_line` program (fused state machine) for `addr` |

`peek-bytes` and `leb128-decode` are generally useful beyond DWARF -- a
windowed memory read and a varint decoder. `dwarf-read-die` and
`dwarf-line-lookup` are pure performance fusions of logic otherwise
expressible in Trix; see [3](#3-the-two-layer-model).

---

## 10. Testing

`tests/run_dwarf_tests.sh` is the regression suite. It builds fixtures on the
fly, runs `lib/dwarf.trx` against them, and cross-checks every parsed address
against the authoritative tools (`nm` for symbols, `addr2line` for lines). It
skips legs cleanly when a tool or build is unavailable, and reports
`/unsupported` from `dwarf-open` as "gated off" so a `-DTRIX_NO_DWARF` build
short-circuits. The legs:

- **Producer x version matrix** -- a two-TU fixture (`dwarf_fixture.cpp` +
  `dwarf_fixture_b.cpp`, two CUs) built at `-gdwarf-2/3/4/5`, plus
  `-gdwarf-5 -gdwarf64` (64-bit DWARF), under the primary compiler. When a
  distinct clang is present it adds **clang v5** (and clang v5 + dwarf64),
  which specifically exercises the `strx`/`addrx` indirection GCC does not
  emit. Each run asserts the layout assertions in `dwarf_layout.trx`, then
  cross-checks `g_probe`'s address and `dwarf_probe_fn`'s `low_pc` against
  `nm`, and `dwarf-line` against `addr2line`.
- **Big-endian** (`gen_dwarf_be_fixture.py` -> `dwarf_be.trx`) -- a
  hand-built BE ELF64 + DWARF v4 exercising every byte-order-sensitive read,
  plus typedef/pointer/enum, arrays, a nested struct, a function `low_pc`,
  an out-of-line spec/definition, and both modern and legacy bitfields.
- **Large binary** (`dwarf_fixture_big.cpp` -> `dwarf_big.trx`, on
  `trix.opt`) -- a DIE-heavy STL TU past the old scratch ceiling, proving the
  lazy reader handles a binary with far more DIEs than a single container
  holds.
- **Split-DWARF guard** -- builds `-gsplit-dwarf` fixtures at v5 (skeleton)
  and v4 (GNU) and asserts `dwarf-load` rejects both with `/unsupported`.
- **Live self-introspection** (`dwarf_self_peek.trx`) -- loads the *running*
  debug `./trix`'s own ~140K-DIE DWARF and decodes the debug-only anchor
  `g_trix_dwarf_self_probe` from live memory via `dwarf-peek-live`,
  cross-checked against the static read and a `main` function lookup +
  `dwarf-line(main)` vs `addr2line`.
- **Shared object** (`dwarf_l21_probe.cpp` -> `dwarf_so_peek.trx`) -- builds
  a tiny `.so`, `LD_PRELOAD`s it into the trix process, loads the `.so`'s own
  DWARF by path, and reads its globals live -- proving `module-load-bias-for`
  relocates by the `.so`'s base, not the executable's.

---

## 11. Build gating and the operator count

The DWARF feature is **opt-out**, mirroring `TRIX_NO_ZLIB` / `TRIX_NO_READLINE`:

- `-DTRIX_NO_DWARF` drops the operator bodies and the `<sys/mman.h>` /
  `<link.h>` includes to trim binary size for hosts that never introspect.
  The eight rows stay registered as stubs that raise `/unsupported`, so the
  dispatch table and the operator count are stable across the gate.
- Adding an operator is the usual ~4 edit points (`enums.inl`,
  `ops_dwarf.inl`, `dispatch.inl`, the `trix.h` include); a `static_assert`
  enforces table consistency, and the new rows go at `LAST_STD_OP` so the
  snapshot format is unaffected.

The eight DWARF operators are user-facing (non-`@`) rows present in the
optimized build, so they count toward the headline operator total that
`tests/check_operator_count.py` derives from `dispatch.inl` and asserts the
docs agree with. They take the documented total from **829 to 837**; the
docs that quote the headline are updated to match when the feature ships.
