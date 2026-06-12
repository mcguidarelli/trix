#!/usr/bin/env python3
"""Extract the ZCOD chunk from a Blorb-wrapped Z-machine story file.

Many modern Inform 6/7 Z-code releases ship as `.zblorb` -- a Blorb
container that wraps the executable plus optional resources (cover art,
sound, metadata).  The Z-machine bytecode itself is the `ZCOD` chunk.

Usage:
    ./blorb-extract.py <input.zblorb> <output.z5|.z8>

Blorb is an IFF-style container; structure (spec: ifarchive
/indexes/if-archive/specifications/blorb/):

    bytes 0..3   "FORM"
    bytes 4..7   total file size minus 8 (big-endian)
    bytes 8..11  "IFRS"   (Blorb form type)
    then chunks, each:
        4 bytes  chunk type (4-char ASCII tag)
        4 bytes  chunk size (big-endian)
        N bytes  data
        1 byte   padding if N is odd

The chunk we want is `ZCOD` (Z-machine bytecode).  If the executable
chunk is `GLUL` instead, the file is Glulx -- a different VM, not
runnable by this Z-machine interpreter.

Exit codes:
    0  ZCOD chunk extracted
    1  GLUL chunk found (Glulx; not Z-machine)
    2  malformed file or no executable chunk
"""

import sys, struct


def be32(b, o):
    return struct.unpack(">I", b[o:o+4])[0]


def main():
    if len(sys.argv) != 3:
        print("usage: blorb-extract.py <input.zblorb> <output.z5|.z8>",
              file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    if data[0:4] != b"FORM" or data[8:12] != b"IFRS":
        print(f"{src}: not a Blorb file (no FORM/IFRS header)", file=sys.stderr)
        return 2
    pos = 12
    while pos + 8 <= len(data):
        ctype = data[pos:pos+4]
        csize = be32(data, pos+4)
        cdata = data[pos+8:pos+8+csize]
        if ctype == b"ZCOD":
            ver = cdata[0] if cdata else 0
            if not (1 <= ver <= 8):
                print(f"{src}: ZCOD chunk has bogus version byte {ver}",
                      file=sys.stderr)
                return 2
            with open(dst, "wb") as out:
                out.write(cdata)
            print(f"{src}: extracted ZCOD ({csize} bytes, V{ver}) -> {dst}")
            return 0
        if ctype == b"GLUL":
            print(f"{src}: contains GLUL (Glulx, not Z-machine) -- can't run "
                  f"on this interpreter", file=sys.stderr)
            return 1
        pos += 8 + csize + (csize & 1)
    print(f"{src}: no ZCOD or GLUL chunk found", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
