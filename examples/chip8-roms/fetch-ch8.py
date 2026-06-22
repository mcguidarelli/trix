#!/usr/bin/env python3
"""Download public-domain CHIP-8 / Super-CHIP ROMs for the chip8.trx showcase.

This fetches ONLY genuinely free-to-redistribute ROMs: every entry below comes
from John Earnest's chip8Archive (https://github.com/JohnEarnest/chip8Archive),
whose contents are explicitly placed under the Creative Commons 0 ("No Rights
Reserved") public-domain dedication, with contributors agreeing to that on
submission.  It does NOT fetch the "classic" 1990s ROM packs (Pong, Brix,
Space Invaders, the Super-CHIP game ports, ...): those are clones of
trademarked franchises whose "public domain" status is asserted but legally
murky, so they are not ours to redistribute.  This mirrors the zmachine
fetcher, which likewise pulls only freely-distributable story files.

Downloaded ROMs are git-ignored (third-party content); only this script is
committed.  Files land beside it under the chip8Archive's own names, so the
no-argument menu in chip8.trx lists them.

Usage:
    ./fetch-ch8.py                 # download everything not already present
    ./fetch-ch8.py --list          # print the catalog, download nothing
    ./fetch-ch8.py snek br8kout    # download only targets matching these names
    ./fetch-ch8.py --force         # re-download even if the file already exists

Stdlib only -- no pip installs.  After fetching, run e.g.
    ../../trix ../chip8.trx snek.ch8
"""

import argparse
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROMS = "https://raw.githubusercontent.com/JohnEarnest/chip8Archive/master/roms"

# Each entry: (filename, archive-key, platform, title, author).
# All are CC0; the file is fetched from {ROMS}/{key}.ch8.  Verified reachable
# 2026-06.  A curated cross-section: small demos, classic-genre remakes, and a
# few Super-CHIP hi-res showpieces.
MANIFEST = [
    # --- CHIP-8 ---
    ("octojam1title.ch8", "octojam1title", "chip8", "Octojam 1 Title",   "John Earnest"),
    ("snek.ch8",          "snek",          "chip8", "Snek (Snake, 65 B)", "John Earnest"),
    ("br8kout.ch8",       "br8kout",       "chip8", "Br8kout (Breakout)", "SharpenedSpoon"),
    ("superpong.ch8",     "superpong",     "chip8", "Super Pong",         "offstatic"),
    ("flightrunner.ch8",  "flightrunner",  "chip8", "Flight Runner",      "TodPunk"),
    ("1dcell.ch8",        "1dcell",        "chip8", "1D Cellular Automata", "SharpenedSpoon"),
    ("fuse.ch8",          "fuse",          "chip8", "Fuse",               "John Earnest"),
    ("outlaw.ch8",        "outlaw",        "chip8", "Outlaw",             "John Earnest"),
    ("danm8ku.ch8",       "danm8ku",       "chip8", "Danm8ku (bullet hell)", "buffi"),
    ("chipquarium.ch8",   "chipquarium",   "chip8", "Chipquarium",        "mattmik"),
    ("tank.ch8",          "tank",          "chip8", "Tank!",              "Rectus"),
    ("glitchGhost.ch8",   "glitchGhost",   "chip8", "Glitch Ghost",       "Jackie Kircher"),
    ("caveexplorer.ch8",  "caveexplorer",  "chip8", "Cave Explorer",      "John Earnest"),
    # --- Super-CHIP (hi-res 128x64) ---
    ("dodge.ch8",         "dodge",         "schip", "Dodge (asteroids, 56 B)", "John Earnest"),
    ("supersquare.ch8",   "supersquare",   "schip", "Super Square",       "tann"),
    ("rockto.ch8",        "rockto",        "schip", "Rockto (Boulder Dash)", "SupSuper"),
    ("octopeg.ch8",       "octopeg",       "schip", "Octopeg (Peggle)",   "Chromatophore"),
    ("octovore.ch8",      "octovore",      "schip", "Octovore",           "Jackie Kircher"),
    ("eaty.ch8",          "eaty",          "schip", "Eaty The Alien",     "John Earnest"),
    ("binding.ch8",       "binding",       "schip", "The Binding of COSMAC", "buffi"),
]


def http_get(url):
    """Fetch url and return its bytes (UA header; raw.githubusercontent is
    fronted by a CDN that rejects the default urllib agent)."""
    req = urllib.request.Request(url, headers={"User-Agent": "trix-chip8-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def fetch_one(filename, key, dest_dir, force):
    out = dest_dir / filename
    if out.exists() and not force:
        print(f"  skip   {filename} (already present)")
        return "skip"
    url = f"{ROMS}/{key}.ch8"
    print(f"  fetch  {filename}  <-  {url}")
    try:
        data = http_get(url)
        out.write_bytes(data)
        print(f"         -> {out.name} ({out.stat().st_size} bytes)")
        return "ok"
    except Exception as exc:  # noqa: BLE001 -- report any fetch failure, keep going
        sys.stderr.write(f"    ERROR: {exc}\n")
        return "fail"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*",
                    help="only fetch targets whose name contains one of these substrings")
    ap.add_argument("--list", action="store_true", help="list the catalog and exit")
    ap.add_argument("--force", action="store_true", help="re-download even if present")
    args = ap.parse_args()

    entries = MANIFEST
    if args.names:
        wanted = [n.lower() for n in args.names]
        entries = [e for e in MANIFEST if any(w in e[0].lower() for w in wanted)]
        if not entries:
            sys.exit(f"no catalog entries match: {' '.join(args.names)}")

    if args.list:
        print(f"{len(entries)} public-domain (CC0) chip8Archive ROMs "
              f"-- classic trademarked-franchise packs are NOT included:\n")
        for filename, _key, platform, title, author in entries:
            print(f"  {filename:20} {platform:6} {title:24} {author}")
        return

    print(f"Fetching {len(entries)} CC0 CHIP-8 ROM(s) into {HERE}\n"
          f"(source: JohnEarnest/chip8Archive, all CC0 public domain)\n")
    tally = {"ok": 0, "skip": 0, "fail": 0}
    for filename, key, _platform, _title, _author in entries:
        tally[fetch_one(filename, key, HERE, args.force)] += 1

    print(f"\nDone: {tally['ok']} downloaded, {tally['skip']} already present, "
          f"{tally['fail']} failed.")
    if tally["fail"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
