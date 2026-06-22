# Security Policy

## Scope

Trix is an embeddable scripting VM.  Security-relevant issues include:

- Memory safety violations (buffer overflows, use-after-free, double-free)
- VM escape (accessing memory outside the VM heap via crafted input)
- Denial of service via crafted scripts (infinite loops without yield,
  heap exhaustion without proper error)
- Scanner/parser crashes on malformed input

Trix is compiled with AddressSanitizer and UndefinedBehaviorSanitizer
and tested against 274 automated test files (20,200+ assertions).
A libFuzzer harness covers the full interpreter (see `fuzz/`).  Trix
should not be used to execute untrusted input without additional
sandboxing.

## Reporting a Vulnerability

If you discover a security issue, please report it privately:

**Email:** guido@grumpy-cat.com

**Subject line:** `[Trix Security] <brief description>`

Please include:
- A minimal reproducing script (.trx file) or C++ code
- The expected vs actual behavior
- Your Trix version (git commit hash or release tag)

## Response

I will acknowledge receipt within 72 hours and aim to provide a fix or
mitigation within 30 days, depending on severity.

Please do not open a public GitHub issue for security vulnerabilities.

## Sandbox Mode

Use `--sandbox` to disable all filesystem, system, and raw memory operators
when running untrusted scripts:

```bash
./trix --sandbox untrusted.trx
```

Or via the host API:

```cpp
Trix::Config cfg;
cfg.m_sandbox = true;
```

When enabled, the following operators raise `Error::Unsupported`:

- Filesystem: `stream` (open), `with-stream`, `delete-file`, `rename-file`,
  `file-stat`, `getcwd`, `chdir`, `mkdir`, `rmdir`, `chmod`, `snap-shot`,
  `thaw`
- System: `run`, `system`, `shell`, `hostname`
- Raw memory: `peek`, `poke`, `alloc`, `free`

This is a representative list, not exhaustive -- see docs/cli.md §5 for the
complete set of sandbox-gated operators.

Combine with `--stream-io=none` to also disable stdin/stdout/stderr
access for maximum isolation.

## Known Limitations

- Without `--sandbox`, Trix does not restrict file system access.  The
  `stream`, `run`, `mkdir`, and `getcwd` operators access the host
  file system with the process's permissions.
- The `peek`/`poke` operators (disabled by `--sandbox`) provide raw
  memory access within the VM heap.  They cannot escape the heap
  boundary but can corrupt VM state.
- A libFuzzer harness exercises the full pipeline (scanner through interpreter) (`fuzz/`).
  Coverage-guided fuzzing has been run but is not yet integrated into CI.
