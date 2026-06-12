#!/usr/bin/env python3
"""Download freely-distributable Z-machine story files for the zmachine.trx showcase.

This fetches ONLY story files their authors distribute freely -- modern Inform
community games, public-domain *Adventure* ports, Dialog-era award winners, and
interpreter conformance suites -- from the Interactive Fiction Archive
(https://ifarchive.org).  It does NOT fetch the commercial Infocom titles
(Zork, Trinity, Hitchhiker, ...): those are copyrighted by Activision.  Buy the
official collection or obtain them legally yourself, then drop them in this
directory (see CATALOG.md for the per-game ID / status table).

Wrappers are unpacked automatically:
  * `.zblorb` Blorb containers -> plain z-code via the sibling blorb-extract.py
  * `.zip` archives           -> the inner story file (or its Blorb) is pulled out

Files land in this directory under the names CATALOG.md uses, so the
interpreter's "Recognized: ..." launch splash matches them.  Downloaded story
files are git-ignored (third-party content); only this script is committed.

Usage:
    ./fetch-stories.py                 # download everything not already present
    ./fetch-stories.py --list          # print the catalog, download nothing
    ./fetch-stories.py curses photopia # download only targets matching these names
    ./fetch-stories.py --force         # re-download even if the file already exists

Stdlib only -- no pip installs.  Requires blorb-extract.py beside this script.
"""

import argparse
import io
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BLORB_EXTRACT = HERE / "blorb-extract.py"

ZCODE = "https://ifarchive.org/if-archive/games/zcode"
TOOLS = "https://www.ifarchive.org/if-archive/infocom/interpreters/tools"

# Each entry: (target-filename, url, kind, inner)
#   kind "zcode"  -- url is a bare .z3/.z5/.z8; save as target.
#   kind "zblorb" -- url is a .zblorb; blorb-extract the ZCOD chunk to target.
#   kind "zip"    -- url is a .zip; pull member `inner` out, then (if it is a
#                    .zblorb) blorb-extract it to target, else save it as target.
# All URLs verified reachable 2026-06; all titles are freely distributable.
MANIFEST = [
    # --- Modern Inform 6 community classics (bare z-code) ---
    ("curses.z5",        f"{ZCODE}/curses.z5",        "zcode", None),
    ("anchor.z8",        f"{ZCODE}/anchor.z8",        "zcode", None),
    ("LostPig.z8",       f"{ZCODE}/LostPig.z8",       "zcode", None),
    ("Mulldoon.z8",      f"{ZCODE}/Mulldoon.z8",      "zcode", None),
    ("photopia.z5",      f"{ZCODE}/photopia.z5",      "zcode", None),
    ("shrapnel.z5",      f"{ZCODE}/shrapnel.z5",      "zcode", None),
    ("dreamhold.z8",     f"{ZCODE}/dreamhold.z8",     "zcode", None),
    ("vespers.z8",       f"{ZCODE}/vespers.z8",       "zcode", None),
    ("theatre.z5",       f"{ZCODE}/theatre.z5",       "zcode", None),
    ("edifice.z5",       f"{ZCODE}/edifice.z5",       "zcode", None),
    ("weather.z5",       f"{ZCODE}/weather.z5",       "zcode", None),
    ("huntdark.z5",      f"{ZCODE}/huntdark.z5",      "zcode", None),
    ("lists.z5",         f"{ZCODE}/lists.z5",         "zcode", None),
    ("metamorp.z5",      f"{ZCODE}/metamorp.z5",      "zcode", None),
    ("pytho.z8",         f"{ZCODE}/pytho.z8",         "zcode", None),
    ("AllRoads.z5",      f"{ZCODE}/AllRoads.z5",      "zcode", None),
    ("spirit.z5",        f"{ZCODE}/spirit.z5",        "zcode", None),
    # --- Public-domain Adventure (Crowther/Woods) ports ---
    ("advent.z3",        f"{ZCODE}/advent.z3",        "zcode", None),
    ("advent.z8",        f"{ZCODE}/advent.z8",        "zcode", None),
    ("adv440.z8",        f"{ZCODE}/adv440.z8",        "zcode", None),
    ("adv550.z8",        f"{ZCODE}/adv550.z8",        "zcode", None),
    ("adv551.z8",        f"{ZCODE}/adv551.z8",        "zcode", None),
    ("zork_285.z5",      f"{ZCODE}/zork_285.z5",      "zcode", None),
    ("zdungeon.z5",      f"{ZCODE}/zdungeon.z5",      "zcode", None),
    ("Adventureland.z5", f"{ZCODE}/Adventureland.z5", "zcode", None),
    # --- Blorb-wrapped releases (unpacked to z-code) ---
    ("galatea.z8",       f"{ZCODE}/Galatea.zblorb",      "zblorb", None),
    ("bronze.z8",        f"{ZCODE}/Bronze.zblorb",       "zblorb", None),
    ("savoirfaire.z8",   f"{ZCODE}/Savoir-Faire.zblorb", "zblorb", None),
    # --- Dialog-era award winners (2021-2025 sweep) ---
    ("ImpossibleStairs.z8", f"{ZCODE}/ImpossibleStairs.z8", "zcode", None),
    ("forsaken-denizen.z8",
     "https://ifarchive.org/if-archive/games/competition2024/Games/Forsaken_Denizen.zip",
     "zip", "Forsaken Denizen.zblorb"),
    ("miss-gosling.z8",
     "https://ifarchive.org/if-archive/games/html/gosling.zip",
     "zip", "gosling.zblorb"),
    ("wise-womans-dog.z8",
     "https://ifarchive.org/if-archive/games/competition2025/Games/The_Wise_Woman_s_Dog.zip",
     "zip", "hasawa.zblorb"),
    # --- Interpreter conformance suites ---
    ("czech.z5",  f"{TOOLS}/czech_0_8.zip", "zip", "czech.z5"),
    ("praxix.z5", f"{TOOLS}/praxix.zip",    "zip", "praxix.z5"),
]


def http_get(url):
    """Fetch url and return its bytes.  Sends a UA header (the archive sits
    behind Cloudflare, which rejects the default urllib agent)."""
    req = urllib.request.Request(url, headers={"User-Agent": "trix-zmachine-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def blorb_to_zcode(zblorb_bytes, target):
    """Run blorb-extract.py on the in-memory Blorb, writing z-code to target.
    Returns True on success, False (with a message) on Glulx / malformed."""
    with tempfile.NamedTemporaryFile(suffix=".zblorb", delete=False) as tmp:
        tmp.write(zblorb_bytes)
        tmp_path = Path(tmp.name)
    try:
        proc = subprocess.run(
            [sys.executable, str(BLORB_EXTRACT), str(tmp_path), str(target)],
            capture_output=True, text=True)
        if proc.returncode == 0:
            return True
        sys.stderr.write(f"    blorb-extract failed ({proc.returncode}): "
                         f"{proc.stdout.strip()} {proc.stderr.strip()}\n")
        return False
    finally:
        tmp_path.unlink(missing_ok=True)


def member_bytes(zip_bytes, inner):
    """Return the bytes of the zip member whose basename == inner."""
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
        for name in zf.namelist():
            if Path(name).name == inner:
                return zf.read(name)
    raise KeyError(f"member {inner!r} not found in archive "
                   f"(has: {', '.join(zipfile.ZipFile(io.BytesIO(zip_bytes)).namelist())})")


def fetch_one(target, url, kind, inner, dest_dir, force):
    out = dest_dir / target
    if out.exists() and not force:
        print(f"  skip   {target} (already present)")
        return "skip"
    print(f"  fetch  {target}  <-  {url}")
    try:
        data = http_get(url)
        if kind == "zip":
            data = member_bytes(data, inner)
            kind = "zblorb" if inner.lower().endswith(".zblorb") else "zcode"
        if kind == "zblorb":
            if not blorb_to_zcode(data, out):
                return "fail"
        else:  # zcode
            out.write_bytes(data)
        print(f"         -> {out.name} ({out.stat().st_size} bytes)")
        return "ok"
    except Exception as exc:  # noqa: BLE001 -- report any fetch/unpack failure, keep going
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

    if not BLORB_EXTRACT.exists():
        sys.exit(f"error: {BLORB_EXTRACT} not found (needed to unpack .zblorb files)")

    entries = MANIFEST
    if args.names:
        wanted = [n.lower() for n in args.names]
        entries = [e for e in MANIFEST if any(w in e[0].lower() for w in wanted)]
        if not entries:
            sys.exit(f"no catalog entries match: {' '.join(args.names)}")

    if args.list:
        print(f"{len(entries)} freely-distributable story files "
              f"(commercial Infocom titles are NOT included):\n")
        for target, url, kind, _inner in entries:
            print(f"  {target:<22} {kind:<7} {url}")
        return

    print(f"Fetching {len(entries)} freely-distributable Z-machine story file(s) "
          f"into {HERE}\n(commercial Infocom titles are excluded -- see CATALOG.md)\n")
    tally = {"ok": 0, "skip": 0, "fail": 0}
    for target, url, kind, inner in entries:
        tally[fetch_one(target, url, kind, inner, HERE, args.force)] += 1

    print(f"\nDone: {tally['ok']} downloaded, {tally['skip']} already present, "
          f"{tally['fail']} failed.")
    if tally["fail"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
