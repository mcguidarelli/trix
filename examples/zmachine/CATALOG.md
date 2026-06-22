# Z-Machine story-file catalog

This directory holds story files that the [zmachine.trx](../zmachine.trx)
showcase plays end-to-end.  See
[`zmachine-manual.md`](../zmachine-manual.md) for the companion
programming + implementation guide -- VM details, encoding forms,
opcode set, save/restore design, CLI flags, and showcase commands.

**No story file is bundled** -- they're third-party
intellectual property in the Infocom case and large binaries we don't
want bloating the repo.  The `.gitignore` excludes all `*.z3` / `*.z4`
/ `*.z5` / `*.z7` / `*.z8` and `*.zblorb` files in this directory.

**Quick setup.** [`fetch-stories.py`](fetch-stories.py) downloads the
freely-distributable titles in one command -- the modern Inform community
classics, the public-domain *Adventure* ports, and the Dialog-era award
winners -- straight from the IF Archive, unpacking any Blorb or `.zip`
wrapper automatically:

```
./fetch-stories.py            # fetch all freely-distributable files (skips ones already here)
./fetch-stories.py --list     # show the catalog without downloading
./fetch-stories.py curses     # fetch only matching titles
```

It deliberately does **not** fetch the commercial Infocom titles (Zork,
Trinity, Hitchhiker, ...) -- those are copyrighted; obtain them legally and
drop them in yourself (see below).

For Inform 6/7 releases distributed as `*.zblorb`, run
[`blorb-extract.py`](blorb-extract.py) on the wrapper to pull out the
plain `.z5` / `.z8` Z-code:

```
./blorb-extract.py Galatea.zblorb galatea.z8
```

The script identifies the inner chunk type: `ZCOD` is Z-machine (extracts);
`GLUL` is Glulx (a different VM, not runnable by this interpreter).

To run any of these you need to drop the corresponding file into this
directory yourself.  Sources for each file are listed in the per-game
notes below.

## Catalog status

The interpreter exercises every entry below as a smoke test under
`--script /tmp/quit-cmds`.  Status here is "PLAY" if the launch
banner + first prompt + a clean `quit` round-trip without hitting any
not-implemented opcode or VM range-check; "FAIL" otherwise (and we'd
want to hear about it).  Files marked V6 or higher are intentionally
out of scope: V6 is the graphical Z-machine, not supported.

## Recognized showcase titles

The "Recognized: ..." splash plate matches release+serial against an
in-source IFhd table; if you drop one of the games below in, you'll
get a labelled launch.  Unknown (release, serial) pairs pass through
silently without the splash.

## Infocom catalogue

These are the original 1980s Infocom titles, distributed via the
[Microsoft / Activision Infocom Collection](https://www.infocom-if.org/)
and historical IF Archive mirrors.  IFhd ("release : serial number")
in the **ID** column.

| File | V | ID | Game | Year | Status |
| --- | --- | --- | --- | --- | --- |
| `zork1.z5` | V3 | 88 : 840726 | Zork I -- The Great Underground Empire | 1984 | PLAY |
| `zork2.z5` | V3 | 48 : 840904 | Zork II -- The Wizard of Frobozz | 1984 | PLAY |
| `zork3.z5` | V3 | 17 : 840727 | Zork III -- The Dungeon Master | 1984 | PLAY |
| `enchanter.z5` | V3 | 29 : 860820 | Enchanter | 1986 | PLAY |
| `sorcerer.z5` | V3 | 15 : 851108 | Sorcerer | 1985 | PLAY |
| `spellbreaker.z5` | V3 | 87 : 860904 | Spellbreaker | 1986 | PLAY |
| `planetfall.z5` | V3 | 37 : 851003 | Planetfall | 1985 | PLAY |
| `stationfall.z5` | V3 | 107 : 870430 | Stationfall | 1987 | PLAY |
| `hitchhiker.z5` | V5 | 31 : 871119 | The Hitchhiker's Guide to the Galaxy | 1987 | PLAY |
| `trinity.z5` | V4 | 12 : 860926 | Trinity | 1986 | PLAY |
| `amfv.z5` | V4 | 77 : 850814 | A Mind Forever Voyaging | 1985 | PLAY |
| `wishbringer.z5` | V3 | 69 : 850920 | Wishbringer | 1985 | PLAY |
| `lurking.z5` | V3 | 221 : 870918 | The Lurking Horror | 1987 | PLAY |
| `infidel.z5` | V3 | 22 : 830916 | Infidel | 1983 | PLAY |
| `ballyhoo.z5` | V3 | 97 : 851218 | Ballyhoo | 1985 | PLAY |
| `bureaucracy.z5` | V4 | 116 : 870602 | Bureaucracy (Adams / Howarth / Meretzky) | 1987 | PLAY |
| `cutthroat.z5` | V3 | 23 : 840809 | Cutthroats | 1984 | PLAY |
| `deadline.z5` | V3 | 27 : 831005 | Deadline | 1983 | PLAY |
| `hollywood.z5` | V3 | 37 : 861215 | Hollywood Hijinx | 1986 | PLAY |
| `lgop.z5` | V3 | 59 : 860730 | Leather Goddesses of Phobos | 1986 | PLAY |
| `moonmist.z5` | V3 | 9 : 861022 | Moonmist | 1986 | PLAY |
| `plundered.z5` | V3 | 26 : 870730 | Plundered Hearts | 1987 | PLAY |
| `seastalker.z5` | V3 | 16 : 850603 | Seastalker | 1985 | PLAY |
| `sherlock.z5` | V5 | 26 : 880127 | Sherlock -- The Riddle of the Crown Jewels | 1988 | PLAY |
| `starcros.z5` | V3 | 17 : 821021 | Starcross | 1982 | PLAY |
| `suspect.z5` | V3 | 14 : 841005 | Suspect | 1984 | PLAY |
| `suspended.z5` | V3 | 8 : 840521 | Suspended | 1984 | PLAY |
| `witness.z5` | V3 | 22 : 840924 | The Witness | 1984 | PLAY |
| `borderzone.z5` | V5 | 9 : 871008 | Border Zone | 1987 | PLAY |
| `nordandber.z5` | V4 | 19 : 870722 | Nord and Bert Couldn't Make Head or Tail of It | 1987 | PLAY |
| `beyondzork.z5` | V5 | 57 : 871221 | Beyond Zork -- The Coconut of Quendor | 1987 | PLAY |
| `ztuu.z5` | V5 | 16 : 970828 | Zork: The Undiscovered Underground (Activision teaser, Lebling + Anderson) | 1997 | PLAY |

## Modern Inform 6 community titles

These are post-Infocom games written in Inform 6 and compiled to
Z-machine bytecode.  Sources are the
[Interactive Fiction Archive](https://www.ifarchive.org/) and the
[Interactive Fiction Database](https://ifdb.org/).  All are
freely-distributable per their authors.

| File | V | ID | Game | Author | Year | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `curses.z5` | V5 | 16 : 951024 | Curses | Graham Nelson | 1995 | PLAY |
| `anchor.z8` | V8 | 5 : 990206 | Anchorhead | Michael S. Gentry | 1998 | PLAY |
| `LostPig.z8` | V8 | 2 : 080406 | Lost Pig | Admiral Jota | 2007 | PLAY |
| `Mulldoon.z8` | V8 | 6 : 000724 | The Mulldoon Legacy | Jon Ingold | 1999 | PLAY |
| `Mullmurd.z5` | V5 | 3 : 020214 | The Mulldoon Murders | Jon Ingold | 2002 | PLAY |
| `curves.z8` | V8 | 9 : 010613 | All Things Devours | half sick of shadows | 2004 | PLAY |
| `christminster.z5` | V5 | 4 : 961117 | Christminster -- An Interactive Conspiracy | Gareth Rees | 1995 | PLAY |
| `jigsaw.z5` | V5 | 3 : 951129 | Jigsaw | Graham Nelson | 1995 | PLAY |
| `theatre.z5` | V5 | 2 : 951203 | Theatre | Brendon Wyber | 1995 | PLAY |
| `sofar.z8` | V8 | 6 : 961218 | So Far | Andrew Plotkin | 1996 | PLAY |
| `edifice.z5` | V5 | 2 : 980206 | The Edifice | Lucian Smith | 1997 | PLAY |
| `spider.z5` | V5 | 4 : 980226 | Spider And Web | Andrew Plotkin | 1998 | PLAY |
| `photopia.z5` | V5 | 1 : 120416 | Photopia | Adam Cadre | 1998 | PLAY |
| `aisle.z5` | V5 | 1 : 990528 | Aisle | Sam Barlow | 1999 | PLAY |
| `varicella.z8` | V8 | 1 : 990831 | Varicella | Adam Cadre | 1999 | PLAY |
| `shrapnel.z5` | V5 | 1 : 000212 | Shrapnel | Adam Cadre | 2000 | PLAY |
| `ninefive.z5` | V5 | 1 : 120724 | 9:05 | Adam Cadre | 2000 | PLAY |
| `dreamhold.z8` | V8 | 5 : 041231 | The Dreamhold | Andrew Plotkin | 2004 | PLAY |
| `vespers.z8` | V8 | 1 : 051128 | Vespers | Peter Nepstad | 2005 | PLAY |
| `makeitgood.z8` | V8 | 15 : 091227 | Make It Good | Jon Ingold | 2009 | PLAY |
| `zork_285.z5` | V5 | 1 : 211010 | Zork (mainframe MDL replica) | Anderson/Blank/Daniels/Lebling, ZIL port by Henrik Åsman | 1977/2021 | PLAY |
| `zdungeon.z5` | V5 | 13 : 040826 | Dungeon (PDP-10 mainframe Zork) | Anderson/Blank/Daniels/Lebling, Inform port by Ethan Dicks | 1981/2004 | PLAY (slow startup -- ~30s on `./trix`, 2s on `./trix.opt`; old Inform 6.14 library does heavy runtime init) |
| `I-0.z5` | V5 | 5 : 140603 | I-0 (or A Brief Survey of the State of Being) | Adam Cadre | 1997 | PLAY |
| `weather.z5` | V5 | 6 : 960613 | A Change in the Weather | Andrew Plotkin | 1995 | PLAY |
| `bedlam.z5` | V5 | 1 : 970711 | Bedlam | Mikko Vuorinen | 1997 | PLAY |
| `mimesis.z5` | V5 | 3 : 980110 | Sins Against Mimesis | G. Kevin Wilson | 1998 | PLAY |
| `Andromeda_Genesis.z8` | V8 | 1 : 130701 | Andromeda Genesis | Marco Innocenti | 2013 | PLAY |
| `AllRoads.z5` | V5 | 1 : 011119 | All Roads | Jon Ingold | 2001 | PLAY (XYZZY Best Game 2001) |
| `nameless.z8` | V8 | 1 : 131206 | Endless, Nameless | Adam Cadre | 2013 | PLAY |
| `pytho.z8` | V8 | 3 : 020223 | Pytho's Mask | Emily Short | 2002 | PLAY |
| `awaken.z5` | V5 | 1 : 980726 | The Awakening (Lovecraftian horror) | Dennis Matheson | 1998 | PLAY |
| `galatea.z8` | V8 | 3 : 040208 | Galatea | Emily Short | 2000 | PLAY (extracted from `Galatea.zblorb`) |
| `bronze.z8` | V8 | 11 : 060503 | Bronze (a Beauty and the Beast retelling) | Emily Short | 2006 | PLAY (`--vm-size=2M`; extracted from `Bronze.zblorb`) |
| `savoirfaire.z8` | V8 | 8 : 040205 | Savoir-Faire | Emily Short | 2002 | PLAY (extracted from `Savoir-Faire.zblorb`) |
| `rameses.z5` | V5 | 3 : 061023 | Rameses (school anxiety) | Stephen Bond | 2000 | PLAY (extracted from `rameses.zblorb`) |
| `slouch.z5` | V5 | 1 : 030925 | Slouching Towards Bedlam | Foster + Ravipinto | 2003 | PLAY (XYZZY Best Game 2003) |
| `huntdark.z5` | V5 | 4 : 991119 | Hunter, in Darkness | Andrew Plotkin | 1999 | PLAY (a cave-crawl Adventure homage) |
| `lists.z5` | V5 | 3 : 960823 | Lists and Lists | Andrew Plotkin | 1996 | PLAY (Inform tutorial; classic intro to IF programming) |
| `coldiron.z8` | V8 | 6 : 111119 | Cold Iron | Plotkin as "Lyman Clive Charles" | 2011 | PLAY |
| `sutwin.z5` | V5 | 2 : 970402 | The Space Under the Window | Andrew Plotkin | 1997 | PLAY |
| `praser5.z5` | V5 | 4 : 050509 | Praser 5 | Andrew Plotkin | 2005 | PLAY |
| `golf.z5` | V5 | 1 : 010114 | Textfire Golf | Cadre as "J.T. Adams" | 2001 | PLAY |
| `Peacock.z5` | V5 | 1 : 000208 | Not Made With Hands | Emily Short | 2000 | PLAY |
| `metamorp.z5` | V5 | 4 : 020222 | Metamorphoses | Emily Short | 2000 | PLAY |
| `FailSafe.z5` | V5 | 1 : 001218 | Fail-Safe | Jon Ingold | 2000 | PLAY |
| `break-in.z5` | V5 | 9 : 000926 | Break-In | Jon Ingold | 2000 | PLAY |
| `insight.z5` | V5 | 4 : 030209 | Insight | Jon Ingold | 2003 | PLAY |
| `aasmasters.z5` | V5 | 1 : 030410 | AAS Masters | Granade as "David Banner" | 2003 | PLAY |
| `spirit.z5` | V5 | 3 : 960606 | SpiritWrak (Zork-style fantasy) | Daniel S. Yu | 1996 | PLAY |
| `tess.z5` | V5 | 2 : 031227 | Beyond The Tesseract | David Lo, Inform port by Plotkin | 1983/2003 | PLAY |
| `jewel.z5` | V5 | 2 : 990710 | The Jewel of Knowledge | Francesco Bova | 1999 | PLAY |
| `sherbet.z5` | V5 | 2 : 961216 | The Meteor, the Stone and a Long Glass of Sherbet | Nelson as "Angela M. Horns" | 1996 | PLAY |
| `heroes.z5` | V5 | 1 : 010928 | Heroes | Sean Barrett | 2001 | PLAY |
| `Adventureland.z5` | V5 | 4 : 970902 | Adventureland (Scott Adams 1978) | Scott Adams, Inform port by Stuart Moore | 1978/1997 | PLAY (one of the first commercial text adventures) |
| `zorkian1.z8` | V8 | 1 : 121014 | Zorkian Stories 1: G.U.E. | Andrew R. Pontious | 2012 | PLAY (extracted from `ZorkianStories1-GUE.zblorb`) |

## Crowther/Woods / Adventure ports

Many ports of the original 1976 *Adventure* exist; these are the
ones we sweep through every release so the V8 packed-address path
gets exercised on small-but-deep dungeons.

| File           | V   | ID         | Notes                          | Status |
| -------------- | --- | ---------- | ------------------------------ | ------ |
| `advent.z3`    | V3  | 1 : 151001 | Adventure (V3 port)            | PLAY   |
| `advent.z5`    | V5  | 9 : 060321 | Adventure (V5 port)            | PLAY   |
| `advent.z8`    | V8  | 1 : 160307 | Adventure (V8 port)            | PLAY   |
| `advent350.z8` | V8  | 1 : 121127 | Adventure -- 350-point version | PLAY   |
| `advent550.z8` | V8  | 1 : 121127 | Adventure -- 550-point version | PLAY   |
| `adv440.z8`    | V8  | 1 : 160307 | Adventure -- 440-point version | PLAY   |
| `adv550.z8`    | V8  | 1 : 160307 | Adventure -- 550-point alt     | PLAY   |
| `adv551.z8`    | V8  | 1 : 171110 | Adventure -- 551-point         | PLAY   |

## Modern award winners (2021-2025 sweep)

A 2026-06-05 review of XYZZY Awards, IFComp, ParserComp and Spring
Thing results for award years 2021-2025 found that most highly-placed
modern IF is Glulx/Twine/TADS (out of scope), with genuine z-code
clustering in 2024-2025 around the Dialog language (which compiles to
z-code) and ParserComp's Inform 6 / PunyInform revival.  The z-code
winners below were tested and added; the Dialog `.zblorb` releases
wrap V8 z-code that `blorb-extract.py` unpacks.  Note: Dialog games
encode their typographic punctuation as ZSCII extra characters via a
custom unicode translation table (header-extension word 3); since
2026-06-06 the interpreter translates those -- and `print_unicode`
codepoints -- to real UTF-8.  '?' remains only for codepoints no
table names and for invalid codepoints; stream-3 memory capture
stays raw ZSCII per spec.

| File | V | ID | Game | Author | Award | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `ImpossibleStairs.z8` | V8 | 3 : 241006 | The Impossible Stairs (Dialog) | Brian Rushton | ParserComp 2022 1st; XYZZY 2022 finalist | PLAY (`--vm-size=2M`) |
| `forsaken-denizen.z8` | V8 | 17 : 240821 | Forsaken Denizen (Dialog; from `ForsakenDenizen.zblorb`) | C.E.J. Pacian | IFComp 2024 3rd | PLAY (`--vm-size=2M`) |
| `miss-gosling.z8` | V8 | 3 : 241016 | Miss Gosling's Last Case (Dialog; from `gosling.zblorb`) | Daniel M. Stelzer | IFComp 2024 6th | PLAY (`--vm-size=2M`) |
| `wise-womans-dog.z8` | V8 | 103 : 250928 | The Wise-Woman's Dog (Dialog; from `hasawa.zblorb`) | Daniel M. Stelzer | IFComp 2025 2nd | PLAY (`--vm-size=2M`; 505 KB story) |

Sources: `ImpossibleStairs.z8` direct from
[the IF Archive](https://ifarchive.org/if-archive/games/zcode/ImpossibleStairs.z8);
`Forsaken_Denizen.zip` from
[games/competition2024](https://ifarchive.org/if-archive/games/competition2024/Games/Forsaken_Denizen.zip);
`gosling.zip` from
[games/html](https://ifarchive.org/if-archive/games/html/gosling.zip);
`The_Wise_Woman_s_Dog.zip` from
[games/competition2025](https://ifarchive.org/if-archive/games/competition2025/Games/The_Wise_Woman_s_Dog.zip).

Verified z-code award winners distributed only through itch.io (no
stable IF Archive bare file; fetch manually):

- *The Samurai and the Kappa* -- Garry Francis, ParserComp 2024 4th
  (Classic); ships `.z5` AND `.z3` (PunyInform).
  <https://warrigal.itch.io/samurai-and-kappa>
- *A Taste of Terror* -- Garry Francis, ParserComp 2025 4th (Classic);
  `.z5`.  <https://warrigal.itch.io/taste-of-terror>
- *Witchever* -- Charles Moore, Jr., ParserComp 2025 3rd (Classic);
  Dialog `.z8`.  <https://improvmonster.itch.io/witchever>

## Compliance test suites

These aren't games -- they're conformance checkers that an interpreter
runs against, then verifies the output against a reference text file.
They're how we caught the `inc_chk`/`dec_chk` 16-bit signed-wrap bug,
the `loadb` address-wrap bug (caught earlier by anchor.z8 first), the
`art_shift` floor-vs-truncated-division bug, the indirect-`store`
peek-vs-push bug, the `random` seeded-sequence bug, the file-`verify`
checksum-against-original-bytes bug, the `copy_table` direction-of-copy
bug, and the missing `output_stream 3` (memory output) implementation.

| File | V | ID | Author | Status |
| --- | --- | --- | --- | --- |
| `czech.z5` | V5 | 1 : 040902 | Evin Robertson, [czech](https://www.ifarchive.org/if-archive/infocom/interpreters/tools/czech_0_8.zip) | 425 / 425 PASS |
| `praxix.z5` | V5 | 1 : 180329 | Andrew Plotkin, [praxix](https://www.ifarchive.org/if-archive/infocom/interpreters/tools/praxix.zip) | 2 fails (multiundo only; single-level @save_undo / @restore_undo passes) |

Run them via (both fit the default VM since the interpreter's
global-VM + periodic-gc retrofit):

```
./trix ../zmachine.trx czech.z5     # no input needed
printf 'all\nquit\n' > /tmp/cmds
./trix ../zmachine.trx --script /tmp/cmds praxix.z5
```

`czech` runs unattended and reports `Performed N tests. Passed: P, Failed: F`.
`praxix` is interactive but accepts `all` as an input to run every test
category in one go.

## Out of scope

| File | V | Notes |
| --- | --- | --- |
| `advent.z6` | V6 | V6 is the graphical Z-machine.  Pictures, mouse, fonts, windowed UI -- a different surface from V1-V5/V7/V8 prose interpreters.  Out of scope for this showcase. |

## Where to get the files

* **Infocom titles**: install the Microsoft / Activision *Infocom
  Collection* (released 2018, freely distributed), or grab individual
  story files from [if-archive/games/infocom](https://www.ifarchive.org/indexes/if-archive/games/infocom/).
  The MS GitHub release ships canonical `.z3` / `.z5` files; copy them
  here under the names listed above.
* **Inform community titles**: most are at
  [if-archive/games/zcode/](https://www.ifarchive.org/indexes/if-archive/games/zcode/)
  under the name in the "File" column above.  A few use different
  archive filenames (rename after download):
  - `spider.z5` is `Tangle.z5` upstream (the original release name was
    a spoiler-free alias)
  - `varicella.z8` is `vgame.z8` upstream
  - `christminster.z5` is `minster.z5` upstream
  - `jigsaw.z5` is `Jigsaw_Game.z5` upstream
  - `ninefive.z5` is `905.z5` upstream
  
  IFDB pages with metadata + reviews:
  [Curses](https://ifdb.org/viewgame?id=plvzam05bmz3enh8),
  [Anchorhead](https://ifdb.org/viewgame?id=op0uw1gn1tjqmjt7),
  [Lost Pig](https://ifdb.org/viewgame?id=mohwfk47yjzii14w),
  [The Mulldoon Legacy](https://ifdb.org/viewgame?id=eb9ai5zfw6n0aolg),
  [Mulldoon Murders](https://ifdb.org/viewgame?id=zqbp17xy3opy7s6r),
  [All Things Devours](https://ifdb.org/viewgame?id=u88shqas41check2g),
  [Christminster](https://ifdb.org/viewgame?id=fq26p07f48ckfror),
  [Jigsaw](https://ifdb.org/viewgame?id=oexa8love2bo7zb1),
  [Theatre](https://ifdb.org/viewgame?id=2qckjrldldn3soyu),
  [So Far](https://ifdb.org/viewgame?id=qwbskm0ux3hkat0a),
  [The Edifice](https://ifdb.org/viewgame?id=86eua25j8wdhwa83),
  [Spider And Web](https://ifdb.org/viewgame?id=2xyccw3pe0uovfad),
  [Photopia](https://ifdb.org/viewgame?id=ou207fwwqbcvzmsw),
  [Aisle](https://ifdb.org/viewgame?id=rcix9aoy0lw684wg),
  [Varicella](https://ifdb.org/viewgame?id=eyc5y5ovk0r4xnxs),
  [Shrapnel](https://ifdb.org/viewgame?id=ehbct4xlvc8kwcef),
  [9:05](https://ifdb.org/viewgame?id=qzftg3j8nh5f34i2),
  [The Dreamhold](https://ifdb.org/viewgame?id=usdmt63zlt5z2lvk),
  [Vespers](https://ifdb.org/viewgame?id=dz9hzpp48hmh50fl),
  [Make It Good](https://ifdb.org/viewgame?id=4ic4ouv3qakmrhf3).
* **Adventure ports**: see [if-archive/games/zcode](https://www.ifarchive.org/indexes/if-archive/games/zcode/).

## Adding a new game

Drop the file in this directory (won't be staged by git -- it's
gitignored) and run:

```
./trix ../zmachine.trx <yourfile>
```

Classic stories fit the default VM; opcode-hungry Dialog-era V8
titles want `--vm-size=2M` or more (the live working set plus the
story banks must fit between gc sweeps).

If the launch banner renders cleanly, the interpreter handled it.
The `--script` flag plus a one-line `quit\ny\n` file is the easiest
way to verify a clean shutdown without poking at the keyboard.

To get a "Recognized: ..." splash for a known title, add the
`(release : serial)` -> title pair to `/known-games` in
`zmachine.trx` §14.  IFhd values are visible via:

```
./trix ../zmachine.trx --header <yourfile>
```
