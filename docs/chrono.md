<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Dates and Times in Trix

Trix has no dedicated date object. Calendar work is built on two ordinary
value types you already know: a **ULong** holds a point in time (an
*instant*), and a **UInteger** holds a bare calendar date (a *udate*).
The chrono operators read fields out of those values, convert between
them, do calendar arithmetic, and format/parse them as text. Because
both representations are plain scalars, they store into globals, compare
with `eq`/`lt`/`gt`, and survive `save`/`restore` with no special
handling.

This guide is the narrative tour. For the exhaustive op-by-op table
(all 32 signatures, every error code), see
[`trix-reference.md` § 3.42](trix-reference.md#342-chrono-operations),
which is authoritative for op semantics. The `strftime` format-letter
spec lives in [`format-cheatsheet.md`](format-cheatsheet.md).

---

## Table of Contents

1. [The Two Representations](#1-the-two-representations)
2. [Reading the Clock](#2-reading-the-clock)
3. [Instants: Fields, Construction, Conversion](#3-instants-fields-construction-conversion)
4. [Udates: Construction, Extraction, Arithmetic](#4-udates-construction-extraction-arithmetic)
5. [UTC vs. Local Zone](#5-utc-vs-local-zone)
6. [Formatting and Parsing](#6-formatting-and-parsing)
7. [Worked Recipes](#7-worked-recipes)
8. [The `/chrono` Convenience Dict](#8-the-chrono-convenience-dict)
9. [Errors](#9-errors)

---

## 1. The Two Representations

There are exactly two carriers, and the distinction is the whole model.

**Instant** -- a `ULong` holding **milliseconds since the Unix epoch**
(1970-01-01 00:00:00 UTC). This is a clock reading: a precise point on
the timeline, the same number everywhere on Earth. `epoch-time` produces
one; the `instant-*` accessors read fields out of one. Because a ULong is
unsigned, instants cannot represent pre-1970 times.

**Udate** -- a `UInteger` holding a packed **year / month / day** with no
clock and no zone. This is the abstract calendar date "April 30th, 2026"
divorced from any moment or place. `make-date` produces one; the `date-*`
ops read and transform it. A udate happily represents pre-1970 (and far
future) dates because it never round-trips through a ULong.

The udate bit layout (a `UInteger` is 32 bits):

```
                              MSB ----------------------- LSB
  [ reserved (9) ][ year (14) ][ month (4) ][ day (5) ]
```

Year occupies the high bits, so chronological ordering is *free* --
`eq`/`lt`/`gt` on the raw UInteger gives calendar order:

```trix
2024 1 1 make-date  2024 2 1 make-date  lt  (jan-before-feb) exch assert
```

Pick the carrier by the question you are answering. "When exactly did
this happen, to the millisecond?" is an instant. "What calendar day is
this, for a report or a birthday?" is a udate. Convert between them with
`instant-to-date` (drops the time-of-day) and `make-instant` (adds one
back); see [§3](#3-instants-fields-construction-conversion).

---

## 2. Reading the Clock

Three clock ops feed the chrono family, and they are not interchangeable.
All three return a `ULong`.

```
epoch-time    -- ulong     % wall-clock ms since 1970 UTC  (feed to instant-*)
now           -- ulong     % monotonic ms, steady epoch    (for elapsed deltas)
clock         -- ulong     % monotonic microseconds, steady epoch
```

`epoch-time` is the one chrono uses: it is a real wall-clock instant you
can break into calendar fields.

`now` and `clock` read a *monotonic* steady clock. Their epoch is
implementation-defined and meaningless on its own -- only the
**difference** between two readings is portable. They never run backward
and are immune to wall-clock jumps (NTP steps, DST), which makes them the
correct choice for "how long did this take." Do **not** pass a `now`
reading to `instant-year` and friends; the result is nonsense.

```trix
(et) epoch-time type /ulong-type eq assert
(nw) now        type /ulong-type eq assert
(ck) clock      type /ulong-type eq assert
% monotonic: a later reading is never less than an earlier one
(mono) now dup ge assert
```

For timing a block of code there is a dedicated op,
`time { proc } -- ulong`, which returns the proc's elapsed microseconds:

```trix
(timed) { 1 1 add pop } time 0ul ge assert
```

`time`, `now`, and `clock` live in the system layer rather than the
chrono family proper, but they are the clock foundation everything else
sits on.

---

## 3. Instants: Fields, Construction, Conversion

An instant is a ULong; the `instant-*` accessors take one and replace it
with a calendar field. Below, the literal `1700000000000ul` is a fixed
instant equal to **2023-11-14 22:13:20 UTC** (a Tuesday), so every result
is deterministic.

```
instant-year         ulong -- int    % e.g. 2023
instant-month        ulong -- int    % 1..12
instant-day          ulong -- int    % 1..31
instant-hour         ulong -- int    % 0..23
instant-minute       ulong -- int    % 0..59
instant-second       ulong -- int    % 0..59
instant-millisecond  ulong -- int    % 0..999  (zone-independent)
instant-weekday      ulong -- name   % /sunday .. /saturday
```

```trix
(year)    1700000000000ul instant-year         2023 eq assert
(month)   1700000000000ul instant-month           11 eq assert
(day)     1700000000000ul instant-day             14 eq assert
(hour)    1700000000000ul instant-hour            22 eq assert
(minute)  1700000000000ul instant-minute          13 eq assert
(second)  1700000000000ul instant-second          20 eq assert
(ms)      1700000123ul    instant-millisecond    123 eq assert
(weekday) 1700000000000ul instant-weekday    /tuesday eq assert
```

Note that `instant-weekday` yields a literal **Name** (`/tuesday`), not a
number -- convenient for dispatch and printing.

**Instant to udate** drops the time-of-day, keeping just the calendar
date:

```
instant-to-date      ulong -- udate
```

```trix
(i2d) 1700000000000ul instant-to-date date-day 14 eq assert
```

**Udate plus a time-of-day to instant** goes the other way.
`make-instant` takes a udate and four time fields:

```
make-instant   udate hour minute second ms -- ulong
```

The pair round-trips cleanly:

```trix
(mk) 2023 11 14 make-date 22 13 20 0 make-instant 1700000000000ul eq assert
```

Each time field is range-checked (`hour` 0..23, `minute`/`second` 0..59,
`ms` 0..999), and a udate that would land before 1970 raises
`/range-check` because the result cannot fit a ULong.

---

## 4. Udates: Construction, Extraction, Arithmetic

`make-date` builds a udate and **validates** it -- impossible dates are
rejected at construction, so a udate in hand is always a real calendar
day:

```
make-date     year month day -- udate
date-year     udate -- int
date-month    udate -- int
date-day      udate -- int
date-weekday  udate -- name      % /sunday .. /saturday
day-of-year   udate -- int       % 1..366
```

```trix
(year)  2026 4 30 make-date date-year       2026 eq assert
(month) 2026 4 30 make-date date-month         4 eq assert
(day)   2026 4 30 make-date date-day          30 eq assert
(wd)    2026 4 30 make-date date-weekday /thursday eq assert
(doy)   2026 4 30 make-date day-of-year       120 eq assert
```

**Calendar arithmetic** adds signed deltas. Day arithmetic is exact;
month and year arithmetic *clamp the day* to the last valid day of the
target month, so Jan 31 + 1 month is the end of February, not a bogus
"Feb 31":

```
date-add-days     udate delta -- udate
date-add-months   udate delta -- udate   % clamps day to month end
date-add-years    udate delta -- udate   % Feb 29 -> Feb 28 in non-leap year
date-diff-days    udate-a udate-b -- int % days(b) - days(a), signed
```

```trix
(addd)  2026 4 30 make-date 1 date-add-days   date-day  1 eq assert
(addm)  2026 1 31 make-date 1 date-add-months date-day 28 eq assert  % 2026 not leap
(leap)  2024 1 31 make-date 1 date-add-months date-day 29 eq assert  % 2024 leap
(addy)  2024 2 29 make-date 1 date-add-years  date-day 28 eq assert  % clamps Feb 29
(diff)  2024 1 1 make-date 2024 12 31 make-date date-diff-days 365 eq assert
(back)  2026 4 30 make-date 2026 4 29 make-date date-diff-days  -1 eq assert
```

`date-diff-days` is right-minus-left: positive when the second argument
is later, negative when earlier.

**Predicates** answer calendar questions directly:

```
leap-year?     year -- bool
days-in-month  year month -- int    % 28..31
```

```trix
(leap?)  2024 leap-year? assert
(nope?)  2023 leap-year? not assert
(feb24)  2024 2 days-in-month 29 eq assert
```

---

## 5. UTC vs. Local Zone

Every instant accessor and every instant/date conversion comes in two
flavors. The bare name interprets the instant in **UTC**; the `-local`
suffix interprets it through the host's local time zone:

```
instant-year ... instant-weekday          % UTC
instant-year-local ... instant-weekday-local
instant-to-date        instant-to-date-local
make-instant           make-instant-local
```

`instant-millisecond` has **no** `-local` variant: zone offsets are whole
seconds, so the millisecond field is identical in both interpretations.

A given instant is one fixed point on the timeline; the two flavors just
split it into fields differently. The UTC results are deterministic and
safe to assert on. The **local** results depend on the host's zone, so a
doc cannot claim a value -- assert *structure* instead. A local hour is
always in 0..23, a local weekday is always one of the seven names:

```trix
% local hour is in range regardless of which zone the host sits in
(hr) 1700000000000ul instant-hour-local dup 0 ge exch 24 lt and assert
% local weekday is a real weekday name
(wd) 1700000000000ul instant-weekday-local /unknown ne assert
```

`make-instant-local` interprets a wall-clock time *as if* it were on the
host's calendar and converts to a UTC instant. It resolves the awkward
edges of daylight-saving transitions deterministically: a wall-clock time
that falls in a spring-forward gap or a fall-back overlap is mapped to the
earliest matching instant rather than crashing.

If the system has no usable zone database (no `/etc/localtime`, say),
every `-local` op raises `/unsupported`. The UTC variants stay available
as a fallback, so a robust program can catch `/unsupported` and degrade to
UTC:

```trix
% fall back to UTC if the host has no zone database
(year)
<< /unsupported { pop 1700000000000ul instant-year } >>    % on /unsupported, use UTC
{ 1700000000000ul instant-year-local }                     % try local first
try-catch
2000 gt assert
```

---

## 6. Formatting and Parsing

The print-fmt / scan-fmt family gains two chrono-aware format letters.
See [`format-cheatsheet.md`](format-cheatsheet.md) for the full
format-spec grammar; the chrono additions are:

| Letter  | Carrier          | Body                                  |
| ------- | ---------------- | ------------------------------------- |
| `:I[l]` | instant (ULong)  | `strftime` template; `l` = local zone |
| `:D`    | udate (UInteger) | `strftime` template; no zone suffix   |

The template body runs from the format letter to the closing `}`, in
`strftime` syntax (`%Y` year, `%m` month, `%d` day, `%H:%M:%S` clock,
`%a`/`%b` names, `%j` day-of-year, and so on). (The unrelated `:T`
spec you may see in the cheatsheet is the *type-name* printer, not a
chrono letter.)

The examples below use explicit UTC formatting so the output is
deterministic. `asprint-fmt` formats into a string (`dst-str fmt arr --
str bool`); the trailing ok-bool is dropped with `pop`:

```trix
% ISO-8601 instant, UTC
=string ({0:I%Y-%m-%dT%H:%M:%SZ}) [ 1700000000000ul ] asprint-fmt
  pop (2023-11-14T22:13:20Z) eq (iso) exch assert

% the standard width/align/fill prefix applies to the formatted result
=string ({0:>20I%Y-%m-%d}) [ 1700000000000ul ] asprint-fmt
  pop (          2023-11-14) eq (pad) exch assert

% udates use :D (zone-independent)
=string ({0:D%Y-%m-%d}) [ 2026 4 30 make-date ] asprint-fmt
  pop (2026-04-30) eq (d1) exch assert
=string ({0:D%a %b %d}) [ 2026 4 30 make-date ] asprint-fmt
  pop (Thu Apr 30) eq (d2) exch assert
=string ({0:D%j}) [ 2026 4 30 make-date ] asprint-fmt
  pop (120) eq (d3) exch assert
```

Printing straight to stdout uses `aprint-fmt` (`fmt arr -- int bool`):

```trix
(now-utc: {0:I%Y-%m-%dT%H:%M:%SZ}) [ epoch-time ] aprint-fmt nl pop pop
```

Run that as a script and you get the current UTC time, e.g.:

```
now-utc: 2026-06-04T17:42:09Z
```

**Parsing** is symmetric. `sscan-fmt` (`input-str fmt mark slots... --
values... count`) reads typed values back out; the count lands on top,
the parsed value just beneath it:

```trix
% parse an instant back to ms-since-epoch
(2023-11-14 22:13:20) ({0:I%Y-%m-%d %H:%M:%S}) mark 0ul sscan-fmt
  1 eq (count-I) exch assert                  % one field matched
  1700000000000ul eq (val-I) exch assert      % and it round-trips

% parse a udate
(2026-04-30) ({0:D%Y-%m-%d}) mark 0u sscan-fmt
  1 eq (count-D) exch assert
  2026 4 30 make-date eq (val-D) exch assert
```

Format and parse are **whole-second** precision: sub-second ms are
dropped on output and read back as zero. The `strftime` template body is
capped at 64 characters and scan input is matched against the first 256
bytes of the remaining input; longer records surface as
`/scan-type-fail`. A scan template must yield a real calendar date (a
time-only or year-only template has no day/month and fails). For stream
records that might straddle a buffer-fill boundary, fall back to
`read-string` plus `sscan-fmt`. See
[`trix-reference.md` § 3.42](trix-reference.md#342-chrono-operations) for
the precise limits.

---

## 7. Worked Recipes

### 7.1 Age in days between two dates

Pure udate arithmetic -- `date-diff-days` does all the work, leap years
included:

```trix
/born  1990 5 15 make-date def
/today 2026 6 4  make-date def
/age-days born today date-diff-days def
(age) age-days 13169 eq assert
```

### 7.2 A timestamped log line

Format a wall-clock instant inline with the message. Using `{1:s}` for
the message keeps the string content bare:

```trix
=string ([{0:I%Y-%m-%d %H:%M:%S}] {1:s})
  [ 1700000000000ul (server started) ] asprint-fmt
  pop ([2023-11-14 22:13:20] server started) eq (log) exch assert
```

In a real program you would feed `epoch-time` (or use `{0:Il...}` for the
host zone) instead of the fixed instant, and `aprint-fmt` to write the
line straight to stdout.

### 7.3 Sleep until a wall-clock instant

Compute a wake instant from `epoch-time` (instants are ULongs, so the
ordinary `add`/`lt`/`ge` operators work on them), then yield from a
coroutine until that instant arrives. `coroutine-join` runs the scheduler
until the coroutine finishes; the scheduler real-sleeps the OS thread
between wakeups, so this is not a busy-loop. See
[`coroutines.md`](coroutines.md) for the coroutine model.

```trix
/wake epoch-time 50ul add def              % ~50 ms in the future
[ {
    { epoch-time wake lt } { 10 coroutine-sleep } while
    epoch-time                             % leave the actual wake-up instant
} coroutine-launch
coroutine-join                             % -- value bool ; drives the scheduler
(joined-ok)    exch assert                 % drop the join-ok bool
wake ge (slept) exch assert                % we did wait until at least `wake`
```

### 7.4 A recurring schedule generator

A recurrence is an *unfold*: start from a seed date and apply one step
repeatedly. The idiom is `[ seed N { dup step } repeat ]` -- `dup` leaves
each date in the array while `step` advances a copy to the next. Swapping
the step changes the cadence, and udate arithmetic (`date-add-days`,
`date-add-months`, ...) absorbs month lengths and leap years for you.

```trix
% A reusable generator: a start date, a step proc (date -- date'), and a
% count -> the array of the next N occurrence dates.
/occurrences {                              % start step n -- [dates]
    |start step n|
    [ start  n 1 sub { dup step } repeat ]
} def

% every other Friday, four of them
/fridays 2026 1 2 make-date { 14 date-add-days } 4 occurrences def
(start is a Friday)    2026 1 2 make-date date-weekday /friday eq assert
(four occurrences)     fridays length 4 eq assert
(all land on a Friday) fridays { date-weekday /friday eq } all assert
(last is 2026-02-13)   fridays 3 get 2026 2 13 make-date eq assert

% the 15th of each month, six months out -- date-add-months keeps the day
/monthly 2026 1 15 make-date { 1 date-add-months } 6 occurrences def
(six occurrences)      monthly length 6 eq assert
(all land on the 15th) monthly { date-day 15 eq } all assert
(june is the sixth)    monthly 5 get date-month 6 eq assert
```

Because `step` is a proc bound in the generator's local frame, the bare
name `step` inside the loop runs it -- so `occurrences` stays agnostic to
the cadence. [`examples/schedule.trx`](../examples/schedule.trx) carries
the idea all the way: a full recurring-event generator with daily /
weekly / monthly / yearly rules, weekday filters, "nth weekday of the
month" (e.g. the fourth Thursday in November), and table / CSV /
calendar-grid output -- all built on the `/chrono` surface.

```console
$ ./trix examples/schedule.trx --kind weekly --every-n-weeks 2 --days mon,wed,fri
$ ./trix examples/schedule.trx --kind yearly --ordinal fourth --day-type thu --month nov
```

---

## 8. The `/chrono` Convenience Dict

`/chrono` is a ReadOnly subsystem dict (the same pattern as `/pipeline`
and `/actors`) that re-exports the chrono ops under shorter keys, mostly
dropping the `instant-`/`date-` prefix where it does not collide:

```trix
% path access through the subsystem dict
(wd) 1700000000000ul //:chrono:weekday exec /tuesday eq assert
```

Or open it with `begin`/`end` to use the bare names directly -- inside the
block `year` resolves to `instant-year`, `add-days` to `date-add-days`:

```trix
//:systemdict:chrono begin
    (year)   1700000000000ul year 2023 eq assert
    (addday) 2026 4 30 make-date 1 add-days date-day 1 eq assert
end
```

The full key-to-op mapping is tabulated in
[`trix-reference.md` § 3.42](trix-reference.md#342-chrono-operations).

---

## 9. Errors

| Error | Raised when |
| --- | --- |
| `/range-check` | invalid calendar date, out-of-range field, reserved bits set, or a pre-1970 ULong target |
| `/scan-type-fail` | a `:I`/`:D` parse did not match, or input exceeded the 256-byte / buffer-straddle limit |
| `/limit-check` | `strftime` template body over 64 chars, or formatted output over 256 bytes |
| `/invalid-format-string` | empty `strftime` template, or the unsupported `%f` (fractional seconds) specifier |
| `/unsupported` | a `-local` op (or `:Il`) requested but the host has no usable time zone |
| `/type-check` | an accessor got the wrong carrier (e.g. an Integer where a udate was expected) |

`make-date` rejects impossible dates up front, and the `date-*` ops
reject any UInteger whose reserved bits are non-zero -- i.e. anything that
was not produced by `make-date`. Catch them the usual way:

```trix
(feb30)    { 2026 2 30 make-date }                            try /range-check eq assert
(bigyear)  { 99999 1 1 make-date }                            try /range-check eq assert
(notudate) { 12345678u date-day }                             try /range-check eq assert
(pre1970)  { 1960 1 1 make-date 0 0 0 0 make-instant }        try /range-check eq assert
```

See [`error-handling.md`](error-handling.md) for the `try` / `catch-error`
machinery and [`errors-cheatsheet.md`](errors-cheatsheet.md) for the full
error roster.
