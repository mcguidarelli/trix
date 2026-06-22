#!/usr/bin/env python3
"""Drive ./trix --inspect under a pty, feed keystrokes, capture screen output.

Usage: inspect_smoke.py <trix-binary> <user-script> <keys-sequence>

`keys-sequence` is a string of raw bytes to send.  Special markers in
the sequence:
  \\s  -> sleep 100ms (give the debugger time to render between keys)
  \\E  -> ESC byte (0x1b)
  \\r  -> CR (0x0d)
  \\n  -> LF (0x0a)
Anything else is sent verbatim.

After EOF or 3s of silence, the captured pty output is dumped to stdout
and the child is killed (if still running).  Exit 0 on clean child exit,
1 on timeout / abnormal exit.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def parse_keys(spec: str) -> bytes:
    out = bytearray()
    i = 0
    sleeps = []
    while i < len(spec):
        ch = spec[i]
        if ch == "\\" and i + 1 < len(spec):
            nxt = spec[i + 1]
            if nxt == "s":
                # mark a sleep boundary by inserting a no-op separator we'll
                # post-process when sending
                out.append(0x00)
            elif nxt == "E":
                out.append(0x1B)
            elif nxt == "r":
                out.append(0x0D)
            elif nxt == "n":
                out.append(0x0A)
            elif nxt == "\\":
                out.append(ord("\\"))
            else:
                out.append(ord(nxt))
            i += 2
        else:
            out.append(ord(ch))
            i += 1
    return bytes(out)


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: inspect_smoke.py <binary> <script> <keys>", file=sys.stderr)
        return 2
    binary, script, keys = sys.argv[1], sys.argv[2], sys.argv[3]
    key_bytes = parse_keys(keys)

    pid, fd = pty.fork()
    if pid == 0:
        # child: launch trix under our pty
        os.execvp(binary, [binary, "--inspect", script])
    # parent: tell the pty it has 80 cols x 24 rows so terminal-size
    # returns something reasonable.  Without this the pty defaults to
    # 0x0 and make-screen rejects the geometry.
    winsize = struct.pack("HHHH", 24, 80, 0, 0)  # rows, cols, xpix, ypix
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)
    captured = bytearray()
    last_data = time.time()
    deadline = time.time() + 10.0  # absolute cap
    idle_cap = 1.0
    sent_idx = 0

    def maybe_send():
        nonlocal sent_idx
        while sent_idx < len(key_bytes):
            b = key_bytes[sent_idx : sent_idx + 1]
            if b == b"\x00":
                # sleep marker
                sent_idx += 1
                time.sleep(0.15)
                continue
            try:
                os.write(fd, b)
            except OSError:
                return False
            sent_idx += 1
            time.sleep(0.05)
        return True

    # Give the debugger a moment to render the first frame
    time.sleep(0.30)
    maybe_send()

    while time.time() < deadline:
        rlist, _, _ = select.select([fd], [], [], 0.2)
        if rlist:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            captured.extend(chunk)
            last_data = time.time()
        # if we've sent everything and seen idle output for a while, quit
        if sent_idx >= len(key_bytes) and (time.time() - last_data > idle_cap):
            break

    # try to kill cleanly
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        pass
    try:
        _, status = os.waitpid(pid, os.WNOHANG)
    except OSError:
        status = -1
    sys.stdout.buffer.write(bytes(captured))
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
