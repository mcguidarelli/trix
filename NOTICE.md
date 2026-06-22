# NOTICE

Trix
Copyright 2026 Mark Guidarelli

This product is licensed under the Apache License, Version 2.0.
See `LICENSE` for the full license text.

## Attribution

Products and projects that embed or redistribute Trix (or a derivative
work thereof) should include the following acknowledgement in their
attribution location (documentation, "About" screen, or third-party-
notices file):

> This product includes Trix (https://github.com/mcguidarelli/trix),
> Copyright 2026 Mark Guidarelli, licensed under the Apache License,
> Version 2.0.

Per Section 4(d) of the Apache License, the text above must be carried
forward into any NOTICE file distributed with a derivative work.

## Third-Party Attributions

### PCG32 Pseudo-Random Number Generator

`src/pcg32.inl` implements the PCG-32 algorithm based on research and the
reference implementation by Melissa O'Neill.

- Author: Melissa O'Neill, <oneill@pcg-random.org>
- Homepage: https://www.pcg-random.org/
- Paper: "PCG: A Family of Simple Fast Space-Efficient Statistically Good
  Algorithms for Random Number Generation" (2014)
- License of the original reference implementation: Apache License 2.0

The algorithmic constants (`DEFAULT_STATE`, `DEFAULT_INC`) and the core
state-advance / output-permutation logic are derived from that reference.

### dlmalloc (global-VM allocator design)

`src/gvm_heap.inl` implements Trix's allocator for the opt-in global
VM region.  Its data-structure design — per-block boundary tags,
segregated free lists, in-place coalescing of adjacent free blocks,
top-chunk advance, and segregated fastbins — is taken directly from
Doug Lea's "dlmalloc" research and reference implementation.

- Author: Doug Lea
- Reference: "A Memory Allocator", 1996.
  https://gee.cs.oswego.edu/dl/html/malloc.html
- License of the reference implementation: public domain
  (Creative Commons CC0 / "release into the public domain"
  per the canonical writeup).

The Trix implementation is a from-scratch rewrite over the Trix
VM heap (offset-based, snapshot/restore-aware, tagged with Trix
`ChunkKind` per block) — no source from the reference
implementation is included — but the algorithmic foundation and
much of the terminology ("fastbin", "boundary tag", "top chunk")
come directly from dlmalloc.  See `src/gvm_heap.inl`'s
"Attribution" header for additional context, and `docs/gvm-heap-gc.md`
for the user-facing maintainer reference.

## Runtime Dependencies (not redistributed)

Trix optionally links against the following libraries at build time. Both
are **enabled by default but can be compiled out** with a build flag, in
which case that library is not referenced or linked at all. Their source
and licenses are not included in this distribution; when linked, they are
obtained from the host system.

- **GNU readline** — interactive-REPL line editing and history. GPLv3+.
  Enabled by default; build with `-DTRIX_NO_READLINE` to drop the
  dependency, in which case the `Trix>` REPL degrades to a plain stdin
  prompt (no line editing or history) and no readline headers or library
  are referenced. Script and `--stdin` execution are unaffected either way.
  https://tiswww.case.edu/php/chet/readline/rltop.html
- **zlib** — DEFLATE / INFLATE for the native `deflate`, `deflate-level`,
  and `inflate` ops (§3.9 of `docs/trix-reference.md`). zlib License
  (BSD-style permissive). Authored by Jean-loup Gailly and Mark Adler.
  Enabled by default; build with `-DTRIX_NO_ZLIB` to drop the dependency,
  in which case those operators stay registered but raise `/unsupported`
  and no zlib headers or library are referenced. The hand-rolled checksum
  ops (`crc32` / `adler32` and their `-stream` variants) and snapshots do
  not use zlib and keep working regardless.
  https://zlib.net/

Linking against GNU readline at runtime does not affect the Apache-2.0
licensing of Trix's own source. Downstream distributors redistributing
a Trix binary linked to readline should review readline's licensing
terms as they apply to their distribution; a binary built with
`-DTRIX_NO_READLINE` does not link readline at all, so those terms do not
apply to it.

zlib's licence is permissive and does not impose redistribution
constraints comparable to readline; no special downstream notice is
required beyond preserving zlib's own copyright when its source is
redistributed.

## A Note on Threading

Trix's execution model is **cooperative**: concurrency is expressed
through coroutines, actors, and supervisors scheduled on a single
interpreter thread. Trix does not expose a threading API and does not
link against a threading library of its own.

However, the host embedding uses a small amount of C++ standard-library
machinery (`std::thread`, `std::mutex`, `std::condition_variable`,
`std::atomic`) to deliver asynchronous interrupts into the interpreter.
On glibc-based Linux systems, the C++ standard library lowers these
primitives onto POSIX Threads, which is why the build links `-pthread`
(or, under CMake, `Threads::Threads`). This is a transitive requirement
of the C++ runtime, not a Trix-level dependency.
