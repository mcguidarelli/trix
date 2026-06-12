#!/usr/bin/env python3
"""
Pty-driven integration test for read-key-byte / read-key-byte-timeout.

Spawns trix inside a pseudo-tty so raw-mode succeeds and stdin gets
O_NONBLOCK; that combination is what triggers the coroutine trampoline
path in C++.  Without it, read-key-byte's read(2) call blocks at the
kernel level and the trampoline never fires -- which is why the rt-ok
test suite (always running with stdin = file/pipe) never caught the
make_control_operator bug fixed in c456c9a.

Three scenarios cover both continuation operators:

  blocking-then-byte   -- read-key-byte blocks; harness sends a byte
                          150 ms later.  Exercises atReadKeyRetry.
  timeout-byte-arrives -- read-key-byte-timeout 1000 ms; harness
                          sends a byte after ~100 ms.  Exercises
                          atReadKeyTimeoutRetry's byte-ready resume.
  timeout-fires        -- read-key-byte-timeout 200 ms; harness
                          sends nothing.  Exercises atReadKeyTimeoutRetry's
                          EAGAIN-on-resume (timer-expired) branch.

Run:
    python3 tests/integration/test_read_key_pty.py

Exits 0 if all scenarios pass, 1 if any fails.  Requires a Linux/macOS
pty (the test is skipped on Windows).
"""

import os
import pty
import select
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TRIX = REPO / "trix"
SCRIPT_DIR = Path(__file__).resolve().parent

# OPOST translates \n -> \r\n on the way out of the pty.  The Trix
# scripts print "READY\n", which arrives at the harness as "READY\r\n".
READY = b"READY"

passed = 0
failed = 0


def spawn(trx_filename):
    """Spawn trix on the named scenario file inside a fresh pty.

    Returns (master_fd, popen).  The slave side is dup'd onto the
    child's stdin/stdout/stderr.  When the child exits, master can be
    drained for any remaining buffered output before close().
    """
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        [str(TRIX), str(SCRIPT_DIR / trx_filename)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    return master, proc


def read_until(master_fd, proc, marker, timeout):
    """Drain master_fd until `marker` appears in the cumulative output
    or the timeout expires.  Returns the accumulated bytes.  Does NOT
    raise on timeout -- caller asserts on the returned content."""
    out = b""
    deadline = time.monotonic() + timeout
    while marker not in out and time.monotonic() < deadline:
        if proc.poll() is not None and not _has_data(master_fd):
            break
        ready, _, _ = select.select([master_fd], [], [], 0.05)
        if ready:
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
    return out


def drain_until_exit(master_fd, proc, timeout):
    """Drain master_fd until the child exits (or we hit timeout).
    Returns all bytes read."""
    out = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None and not _has_data(master_fd):
            break
        ready, _, _ = select.select([master_fd], [], [], 0.05)
        if ready:
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
    if proc.poll() is None:
        proc.kill()
        proc.wait(timeout=1.0)
    return out


def _has_data(fd):
    ready, _, _ = select.select([fd], [], [], 0.0)
    return bool(ready)


def run(name, fn):
    """Run a scenario and tally pass/fail."""
    global passed, failed
    print(f"  {name} ...", end=" ", flush=True)
    try:
        fn()
        passed += 1
        print("OK")
    except AssertionError as e:
        failed += 1
        print(f"FAIL\n      {e}")


# ---- scenarios -------------------------------------------------------


def test_blocking_then_byte():
    """read-key-byte blocks; byte arrives 150 ms later."""
    master, proc = spawn("read_key_blocking.trx")
    try:
        out = read_until(master, proc, READY, timeout=2.0)
        assert READY in out, f"never saw READY (output so far: {out!r})"
        # No byte yet; coroutine should be parked in atReadKeyRetry.
        time.sleep(0.15)
        os.write(master, b"A")
        full = out + drain_until_exit(master, proc, timeout=2.0)
        assert b"BYTE: 65" in full, f"expected 'BYTE: 65' in output, got: {full!r}"
        rc = proc.wait(timeout=1.0)
        assert rc == 0, f"trix exited with {rc}, output: {full!r}"
    finally:
        os.close(master)


def test_timeout_byte_arrives():
    """read-key-byte-timeout 1000 ms; byte arrives ~100 ms in."""
    master, proc = spawn("read_key_timeout_hit.trx")
    try:
        out = read_until(master, proc, READY, timeout=2.0)
        assert READY in out, f"never saw READY (output so far: {out!r})"
        time.sleep(0.10)
        os.write(master, b"B")
        full = out + drain_until_exit(master, proc, timeout=2.0)
        assert b"BYTE: 66" in full, f"expected 'BYTE: 66' in output, got: {full!r}"
        rc = proc.wait(timeout=1.0)
        assert rc == 0, f"trix exited with {rc}, output: {full!r}"
    finally:
        os.close(master)


def test_timeout_fires():
    """read-key-byte-timeout 200 ms; no byte sent."""
    master, proc = spawn("read_key_timeout_miss.trx")
    try:
        out = read_until(master, proc, READY, timeout=2.0)
        assert READY in out, f"never saw READY (output so far: {out!r})"
        # Send nothing.  Timer should fire ~200 ms after read-key-byte-timeout.
        full = out + drain_until_exit(master, proc, timeout=2.0)
        assert b"TIMEOUT" in full, f"expected 'TIMEOUT' in output, got: {full!r}"
        rc = proc.wait(timeout=1.0)
        assert rc == 0, f"trix exited with {rc}, output: {full!r}"
    finally:
        os.close(master)


# ---- main ------------------------------------------------------------


def main():
    if not TRIX.exists():
        print(f"trix binary not found at {TRIX}", file=sys.stderr)
        sys.exit(2)
    print(f"Pty integration tests for read-key-byte family ({TRIX}):")
    run("blocking-then-byte (atReadKeyRetry)", test_blocking_then_byte)
    run("timeout-byte-arrives (atReadKeyTimeoutRetry, byte path)", test_timeout_byte_arrives)
    run("timeout-fires (atReadKeyTimeoutRetry, EAGAIN/timer path)", test_timeout_fires)
    total = passed + failed
    print(f"\n  {passed}/{total} scenarios passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
