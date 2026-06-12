#!/usr/bin/env python3
"""patch_image.py -- adversarial snap-shot image generator.

Used by tests/run_snapshot_tests.sh ("adversarial images" group) to turn
a pristine .img into precisely-corrupted variants that exercise thaw's
validation arms one at a time.

Layout facts this relies on (snapshot.inl SnapShotHeader + thaw_op):
  - The file is [header][memory-stream blocks][user-file-stream blocks]
    [VM blob][global blob], with no startup tail for images produced by
    a --stdin run.
  - The header starts with the 4-byte magic 'TRIX'; the VM blob starts
    with the VM-base sentinel, also 'TRIX'.  For a fixture with small,
    TRIX-free section payloads, the second occurrence of b'TRIX' is the
    header size.
  - The overall CRC-32 (IEEE/zlib) covers the whole file with the
    header's checksum field zeroed; the checksum is the last header
    field (trailing struct padding may follow it).

`calibrate` derives header size and checksum offset and VERIFIES them by
recomputing the pristine image's checksum -- if the layout assumptions
ever drift, the harness fails loudly here rather than testing the wrong
bytes.

Commands:
  calibrate <img>
      print "<header_size> <checksum_offset>" or exit 1.  Only valid
      for images with NO memory/user-file stream sections (the VM-blob
      sentinel must directly follow the header); the derived offsets
      are structural constants reusable for any image of this build.
  verify <img> <checksum_offset>
      recompute the overall CRC at the given offset and compare with
      the stored value; exit 0 on match (validates section-bearing
      images against the calibrated layout).
  apply <in> <out> <checksum_offset> [--fixcrc] <op>...
      ops: patch:<off>:<hexbytes>   overwrite bytes
           xor:<off>:<hexbyte>      flip bits (guaranteed change)
           p32:<off>:<value>        overwrite little-endian uint32
           p64:<off>:<value>        overwrite little-endian uint64
           trunc:<len>              truncate file to <len> bytes
      --fixcrc recomputes the overall checksum after patching (needed
      only for corruption that must SURVIVE the overall CRC check, e.g.
      the VM-base sentinel and the per-section CRC arms).
"""
import sys
import zlib


def overall_crc(data: bytes, cs_off: int) -> int:
    zeroed = data[:cs_off] + b"\x00\x00\x00\x00" + data[cs_off + 4:]
    return zlib.crc32(zeroed) & 0xFFFFFFFF


def calibrate(path: str) -> int:
    data = open(path, "rb").read()
    hdr = data.find(b"TRIX", 4)
    if hdr <= 4:
        print(f"calibrate: no VM-base sentinel found in {path}", file=sys.stderr)
        return 1
    for cs_off in (hdr - 4, hdr - 8):
        stored = int.from_bytes(data[cs_off:cs_off + 4], "little")
        if stored == overall_crc(data, cs_off):
            print(f"{hdr} {cs_off}")
            return 0
    print(f"calibrate: stored checksum does not match recomputation "
          f"(layout drift?) in {path}", file=sys.stderr)
    return 1


def apply(argv: list) -> int:
    src, dst, cs_off = argv[0], argv[1], int(argv[2])
    ops = argv[3:]
    fixcrc = "--fixcrc" in ops
    ops = [o for o in ops if o != "--fixcrc"]

    data = bytearray(open(src, "rb").read())
    for op in ops:
        kind, _, rest = op.partition(":")
        if kind == "trunc":
            data = data[:int(rest)]
        else:
            off_s, _, val_s = rest.partition(":")
            off = int(off_s)
            if kind == "xor":
                data[off] ^= int(val_s, 16)
                continue
            if kind == "patch":
                blob = bytes.fromhex(val_s)
            elif kind == "p32":
                blob = int(val_s, 0).to_bytes(4, "little")
            elif kind == "p64":
                blob = int(val_s, 0).to_bytes(8, "little")
            else:
                print(f"apply: unknown op {op}", file=sys.stderr)
                return 1
            data[off:off + len(blob)] = blob
    if fixcrc:
        crc = overall_crc(bytes(data), cs_off)
        data[cs_off:cs_off + 4] = crc.to_bytes(4, "little")
    open(dst, "wb").write(bytes(data))
    return 0


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 1
    if sys.argv[1] == "calibrate":
        return calibrate(sys.argv[2])
    if sys.argv[1] == "verify":
        data = open(sys.argv[2], "rb").read()
        cs_off = int(sys.argv[3])
        stored = int.from_bytes(data[cs_off:cs_off + 4], "little")
        if stored == overall_crc(data, cs_off):
            return 0
        print(f"verify: checksum mismatch in {sys.argv[2]}", file=sys.stderr)
        return 1
    if sys.argv[1] == "apply":
        return apply(sys.argv[2:])
    print(f"unknown command {sys.argv[1]}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
