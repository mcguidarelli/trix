//===----------------------------------------------------------------------===//
//                                                                            //
//    ______    _                                                             //
//   /_  __/___(_)_  __                                                       //
//    / / / __/ /\ \/ /       Stack-Based Interpreter & VM                    //
//   / / / / / /  > · <      C++23 · Single-Header Library                    //
//  /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli                    //
//                                                                            //
// Licensed under the Apache License, Version 2.0 (the "License");            //
// you may not use this file except in compliance with the License.           //
// You may obtain a copy of the License at                                    //
//                                                                            //
//     https://www.apache.org/licenses/LICENSE-2.0                            //
//                                                                            //
// Unless required by applicable law or agreed to in writing, software        //
// distributed under the License is distributed on an "AS IS" BASIS,          //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   //
// See the License for the specific language governing permissions and        //
// limitations under the License.                                             //
//                                                                            //
//===----------------------------------------------------------------------===//

//===--- Chrono Helpers ---===//
//
// Calendar/instant utilities shared by the PrintFmt `:I`/`:D` printers
// (in printfmt.inl), the ScanFmt `:I`/`:D` parsers (in scanfmt.inl),
// and the instant-* / date-* operators plus make-instant, make-date and
// day-of-year / days-in-month (all in ops_chrono.inl).  The instant-* ops
// and the :I/:D formatters work on ULong instants (milliseconds since the
// Unix epoch, UTC); sub-second precision is dropped for output and recovered
// as zero on input -- formatting is whole-second.  The date-* ops, make-date,
// day-of-year and days-in-month work on packed-UInteger udate calendar values
// (no clock or zone; pre-1970 dates work).
//
// The local-zone variants honor `chrono::current_zone()`.  Resolution is
// cached process-wide via a function-static; if the system has no
// `/etc/localtime` (or equivalent) the cache holds nullptr and every
// local-zone op raises Error::Unsupported (`/unsupported`), which user
// code can catch and fall back to UTC.
//
public:
// Resolve the system's local time zone, caching the result process-wide.
// On systems without a usable zone database this returns nullptr on the
// first attempt and raises Error::Unsupported on every subsequent call.
[[nodiscard]] const std::chrono::time_zone *current_local_zone_or_raise() {
    static const std::chrono::time_zone *cached_zone = []() -> const std::chrono::time_zone * {
        try {
            return std::chrono::current_zone();
        }
        catch (...) {
            return nullptr;
        }
    }();
    if (cached_zone == nullptr) {
        error(Error::Unsupported, "system local time zone is unavailable");
    } else {
        return cached_zone;
    }
}

// "C" locale handle for strptime-driven :I/:D scanning, cached process-wide.
// strptime reads LC_TIME from the active locale; pinning to C makes month
// names and AM/PM tokens parse independently of the user's environment so
// trix templates have stable behavior across machines.  Returns
// (locale_t)0 on rare systems where newlocale fails -- callers fall back
// to the active locale (correct, just env-dependent).
[[nodiscard]] static locale_t c_time_locale() {
    static locale_t cached = newlocale(LC_TIME_MASK | LC_NUMERIC_MASK, "C", static_cast<locale_t>(nullptr));
    return cached;
}

// Get monotonic time in nanoseconds.
[[nodiscard]] static uint64_t monotonic_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(ts.tv_nsec));
}

// Calendar fields derived from a chrono instant.  Used by the
// instant-* accessor ops.  All fields are integer_t (32-bit signed)
// because the surface ops return Integer.  weekday is 0=Sunday .. 6=Saturday.
struct CalendarParts {
    integer_t year;
    integer_t month;
    integer_t day;
    integer_t hour;
    integer_t minute;
    integer_t second;
    integer_t millisecond;
    integer_t weekday;
};

// Extract calendar fields from a seconds-precision time_point.  Works
// uniformly for sys_time<seconds> (UTC interpretation) and
// local_time<seconds> (local-zone interpretation): year_month_day and
// hh_mm_ss treat the time_point as days-since-epoch + time-of-day,
// independent of which clock the time_point belongs to.
template<class TimePoint>
[[nodiscard]] static CalendarParts components_of(TimePoint tp_sec, int64_t ms_remainder) {
    using std::chrono::days;
    using std::chrono::floor;
    using std::chrono::hh_mm_ss;
    using std::chrono::seconds;
    using std::chrono::weekday;
    using std::chrono::year_month_day;

    auto dp = floor<days>(tp_sec);
    const year_month_day ymd{dp};
    const hh_mm_ss<seconds> hms{tp_sec - dp};
    const weekday wd{dp};

    return CalendarParts{static_cast<integer_t>(static_cast<int>(ymd.year())),
                         static_cast<integer_t>(static_cast<unsigned>(ymd.month())),
                         static_cast<integer_t>(static_cast<unsigned>(ymd.day())),
                         static_cast<integer_t>(hms.hours().count()),
                         static_cast<integer_t>(hms.minutes().count()),
                         static_cast<integer_t>(hms.seconds().count()),
                         static_cast<integer_t>(ms_remainder),
                         static_cast<integer_t>(wd.c_encoding())};
}

// Split a ULong instant (ms since 1970 UTC) into UTC calendar fields.
[[nodiscard]] CalendarParts split_instant_utc(ulong_t ms) {
    using std::chrono::seconds;
    using std::chrono::sys_time;

    auto signed_ms = static_cast<int64_t>(ms);
    auto utc_tp = sys_time<seconds>(seconds(signed_ms / 1000));
    return components_of(utc_tp, signed_ms % 1000);
}

// Split a ULong instant (ms since 1970 UTC) into local-zone calendar
// fields.  Raises Error::Unsupported if the system's local zone is
// unavailable; UTC variants always remain available for caller fallback.
[[nodiscard]] CalendarParts split_instant_local(ulong_t ms) {
    using std::chrono::seconds;
    using std::chrono::sys_time;
    using std::chrono::zoned_time;

    auto signed_ms = static_cast<int64_t>(ms);
    auto utc_tp = sys_time<seconds>(seconds(signed_ms / 1000));

    const auto *zone = current_local_zone_or_raise();
    const zoned_time<seconds> zt{zone, utc_tp};
    auto local_tp = zt.get_local_time();
    return components_of(local_tp, signed_ms % 1000);
}

//===--- udate: packed UInteger date encoding ---===//
//
// udate is a 32-bit UInteger holding a year/month/day calendar value
// in the layout below.  No clock/zone semantics -- a udate is the
// abstract date itself, not an instant.  Year is 0..16383 (14 bits)
// so any historical or near-future date fits; pre-1970 dates work
// here even though instants do not.
//
//   bits  0..4   day      (1..31)        5 bits
//   bits  5..8   month    (1..12)        4 bits
//   bits  9..22  year     (0..16383)    14 bits
//   bits 23..31  reserved (must be 0)    9 bits
//
// Chronological ordering is free: UInteger comparison is calendar
// order because year is in the high bits, then month, then day.
//
// All conversions go through these three helpers; ops never touch
// the bit layout directly.

static constexpr uinteger_t UDATE_DAY_MASK = 0x1F;             // 5 bits
static constexpr uinteger_t UDATE_MONTH_MASK = 0xF;            // 4 bits
static constexpr uinteger_t UDATE_YEAR_MASK = 0x3FFF;          // 14 bits
static constexpr uinteger_t UDATE_RESERVED_MASK = 0xFF800000;  // bits 23..31
static constexpr int UDATE_DAY_SHIFT = 0;
static constexpr int UDATE_MONTH_SHIFT = 5;
static constexpr int UDATE_YEAR_SHIFT = 9;
static constexpr int UDATE_YEAR_MAX = 16383;

// Encode pre-validated year/month/day fields into the udate bit layout -- the single
// site that touches the packing layout (pack_udate and pack_udate_from_ymd route here).
[[nodiscard]] static uinteger_t encode_udate(uinteger_t year, uinteger_t month, uinteger_t day) {
    return ((year << UDATE_YEAR_SHIFT) | (month << UDATE_MONTH_SHIFT) | day);
}

// Pack year/month/day into a udate.  Validates calendar correctness
// (Feb 30 etc.) via std::chrono::year_month_day::ok().  Raises
// Error::RangeCheck on out-of-range fields or invalid calendar.
[[nodiscard]] uinteger_t pack_udate(integer_t year, integer_t month, integer_t day) {
    if ((year < 0) || (year > UDATE_YEAR_MAX)) {
        error(Error::RangeCheck, "year {} out of udate range 0..{}", year, UDATE_YEAR_MAX);
    } else if ((month < 1) || (month > 12)) {
        error(Error::RangeCheck, "month {} out of range 1..12", month);
    } else if ((day < 1) || (day > 31)) {
        error(Error::RangeCheck, "day {} out of range 1..31", day);
    } else {
        const std::chrono::year_month_day ymd{std::chrono::year{static_cast<int>(year)},
                                              std::chrono::month{static_cast<unsigned>(month)},
                                              std::chrono::day{static_cast<unsigned>(day)}};
        if (ymd.ok()) {
            return encode_udate(static_cast<uinteger_t>(year), static_cast<uinteger_t>(month), static_cast<uinteger_t>(day));
        } else {
            error(Error::RangeCheck, "{}-{:02}-{:02} is not a valid calendar date", year, month, day);
        }
    }
}

// Unpack a udate into a chrono::year_month_day.  Verifies the
// reserved bits (23..31) are zero -- a non-zero reserved field means
// the UInteger was not produced by pack_udate and is therefore not a
// valid udate.  Raises Error::RangeCheck.
[[nodiscard]] std::chrono::year_month_day unpack_udate(uinteger_t udate) {
    if ((udate & UDATE_RESERVED_MASK) != 0) {
        error(Error::RangeCheck, "udate 0x{:08x} has non-zero reserved bits", udate);
    } else {
        auto year = static_cast<int>((udate >> UDATE_YEAR_SHIFT) & UDATE_YEAR_MASK);
        auto month = static_cast<unsigned>((udate >> UDATE_MONTH_SHIFT) & UDATE_MONTH_MASK);
        auto day = static_cast<unsigned>(udate & UDATE_DAY_MASK);
        if ((month < 1) || (month > 12) || (day < 1) || (day > 31)) {
            error(Error::RangeCheck, "udate 0x{:08x} has invalid month/day fields", udate);
        } else {
            std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
            if (ymd.ok()) {
                return ymd;
            } else {
                error(Error::RangeCheck, "udate 0x{:08x} encodes invalid calendar date", udate);
            }
        }
    }
}

// Repack a year_month_day produced by chrono arithmetic back into a
// udate.  Year is range-checked; month/day come from a valid ymd so
// they are trusted.
[[nodiscard]] uinteger_t pack_udate_from_ymd(std::chrono::year_month_day ymd) {
    auto year = static_cast<int>(ymd.year());
    if ((year < 0) || (year > UDATE_YEAR_MAX)) {
        error(Error::RangeCheck, "year {} out of udate range 0..{}", year, UDATE_YEAR_MAX);
    } else {
        auto month = static_cast<unsigned>(ymd.month());
        auto day = static_cast<unsigned>(ymd.day());
        return encode_udate(static_cast<uinteger_t>(year), static_cast<uinteger_t>(month), static_cast<uinteger_t>(day));
    }
}
private:
