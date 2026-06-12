#!/usr/bin/env python3
"""Pty-driven integration test for the --inspect output-capture pipeline.

Drives ./trix --inspect on tests/smoke/inspect_capture_target.trx under a
pseudo-tty (required: install-debugger calls raw-mode + alt-screen-enter).
Verifies that:

  1. The first cb-fire drains user-script output that already accumulated
     in the capture stream (banner-line-one appears INSIDE the rendered
     alt-screen frame, in the output pane).
  2. After 'c' (continue), the remaining prints stay captured and are
     dumped to real stdout when uninstall-debugger fires at end-of-script
     (banner-line-two / after-x / final appear AFTER the alt-screen
     leave sequence \\x1b[?1049l).
  3. The process exits 0 -- no /io-write-error from buffer overflow, no
     uncaught exception from the cb path.

Run:
    python3 tests/integration/test_inspect_capture_pty.py

Exits 0 on pass, 1 on fail.
"""

import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TRIX = REPO / "trix"
TARGET = REPO / "tests" / "smoke" / "inspect_capture_target.trx"

ALT_SCREEN_LEAVE = b"\x1b[?1049l"

passed = 0
failed = 0


def spawn():
    master, slave = pty.openpty()
    # Without TIOCSWINSZ the pty defaults to 0x0 and make-screen errors.
    winsize = struct.pack("HHHH", 24, 80, 0, 0)
    fcntl.ioctl(master, termios.TIOCSWINSZ, winsize)
    proc = subprocess.Popen(
        [str(TRIX), "--inspect", str(TARGET)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    return master, proc


def drain_until_exit(master_fd, proc, timeout):
    out = bytearray()
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
    return bytes(out)


def _has_data(fd):
    ready, _, _ = select.select([fd], [], [], 0.0)
    return bool(ready)


def run(name, fn):
    global passed, failed
    print(f"  {name} ...", end=" ", flush=True)
    try:
        fn()
        passed += 1
        print("OK")
    except AssertionError as e:
        failed += 1
        print(f"FAIL\n      {e}")


def test_continue_to_end():
    """'c' from first halt -- expect output pane content + post-exit tail."""
    master, proc = spawn()
    try:
        # Let install-debugger run + first cb fire.
        time.sleep(0.4)
        os.write(master, b"c")
        out = drain_until_exit(master, proc, timeout=4.0)
        rc = proc.wait(timeout=1.0)

        assert ALT_SCREEN_LEAVE in out, (
            f"never left alt-screen; output:\n{out!r}"
        )
        pre, post = out.split(ALT_SCREEN_LEAVE, 1)

        # First halt's render drained whatever printed before the halt.
        assert b"-- output --" in pre, (
            f"no output-pane header in render; pre:\n{pre!r}"
        )
        assert b"banner-line-one" in pre, (
            f"banner-line-one missing from rendered frame; pre:\n{pre!r}"
        )

        # After alt-screen-leave, uninstall-debugger drained the tail.
        assert b"banner-line-two" in post, (
            f"banner-line-two missing from post-exit dump; post:\n{post!r}"
        )
        assert b"after-x" in post, (
            f"after-x missing from post-exit dump; post:\n{post!r}"
        )
        assert b"final" in post, (
            f"final missing from post-exit dump; post:\n{post!r}"
        )

        assert rc == 0, f"trix exited with {rc}, output:\n{out!r}"
    finally:
        try:
            os.close(master)
        except OSError:
            pass


def test_quit_from_first_halt():
    """'q' at first halt -- uninstall path fires from inside cb."""
    master, proc = spawn()
    try:
        time.sleep(0.4)
        os.write(master, b"q")
        out = drain_until_exit(master, proc, timeout=4.0)
        rc = proc.wait(timeout=1.0)
        assert ALT_SCREEN_LEAVE in out, (
            f"quit path never left alt-screen; output:\n{out!r}"
        )
        # Quit drains the capture too -- whatever printed before the
        # halt (banner-line-one) should still be visible in the rendered
        # frame.
        pre, _post = out.split(ALT_SCREEN_LEAVE, 1)
        assert b"banner-line-one" in pre, (
            f"banner-line-one missing from quit-path render; pre:\n{pre!r}"
        )
        # quit via debug-on-event sets exit code via 0 quit -> 0
        assert rc == 0, f"trix exited with {rc}, output:\n{out!r}"
    finally:
        try:
            os.close(master)
        except OSError:
            pass


def main():
    if not TRIX.is_file():
        print(f"ERROR: trix binary not found at {TRIX}", file=sys.stderr)
        return 2
    if not TARGET.is_file():
        print(f"ERROR: target not found at {TARGET}", file=sys.stderr)
        return 2
    print("inspect-capture integration test:")
    run("continue-to-end", test_continue_to_end)
    run("quit-from-first-halt", test_quit_from_first_halt)
    print(f"  passed: {passed}, failed: {failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
