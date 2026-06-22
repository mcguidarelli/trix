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

//===--- Chrono Tier 1: instant accessor ops ---===//
//
// Calendar-component accessors over ULong instants (milliseconds since
// the Unix epoch, UTC).  Each accessor comes in two variants:
//
//   instant-<field>         UTC interpretation
//   instant-<field>-local   local-zone interpretation
//
// All fields:
//   year          int     (e.g. 2026)
//   month         int     (1..12)
//   day           int     (1..31)
//   hour          int     (0..23)
//   minute        int     (0..59)
//   second        int     (0..59)
//   millisecond   int     (0..999; same value for UTC and local)
//   weekday       name    (/sunday .. /saturday)
//
// Local-zone variants raise `/unsupported` if the system has no usable
// time zone (e.g. no /etc/localtime); the UTC variants stay available
// as fallback.  See chrono.inl::current_local_zone_or_raise().
//

static constexpr std::string_view kWeekdayNames[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};

// Replace the ULong on top of stack with an Integer made from `value`.
// Frees the ULong's ExtValue first.
static void replace_ulong_with_integer(Trix *trx, integer_t value) {
    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = Object::make_integer(value);
}

// Replace the ULong on top of stack with the weekday Name for `idx` (0..6).
static void replace_ulong_with_weekday(Trix *trx, integer_t idx) {
    assert((idx >= 0) && (idx < 7));

    auto name_obj = Name::make(trx, kWeekdayNames[idx]);
    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = name_obj;
}

//===--- UTC accessors ---===//

// instant-year: ulong -- int
// Returns the UTC calendar year (e.g. 2026).
// throws: type-check, opstack-underflow
static void instant_year_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.year);
}

// instant-month: ulong -- int
// Returns the UTC calendar month, 1 (January) through 12 (December).
// throws: type-check, opstack-underflow
static void instant_month_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.month);
}

// instant-day: ulong -- int
// Returns the UTC day-of-month, 1..31.
// throws: type-check, opstack-underflow
static void instant_day_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.day);
}

// instant-hour: ulong -- int
// Returns the UTC hour, 0..23.
// throws: type-check, opstack-underflow
static void instant_hour_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.hour);
}

// instant-minute: ulong -- int
// Returns the UTC minute, 0..59.
// throws: type-check, opstack-underflow
static void instant_minute_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.minute);
}

// instant-second: ulong -- int
// Returns the UTC second-of-minute, 0..59.
// throws: type-check, opstack-underflow
static void instant_second_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.second);
}

// instant-millisecond: ulong -- int
// Returns the millisecond-of-second, 0..999.  Identical for UTC and
// local-zone interpretations (zone offsets are whole-second), so no
// `-local` variant is provided.
// throws: type-check, opstack-underflow
static void instant_millisecond_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.millisecond);
}

// instant-weekday: ulong -- name
// Returns the UTC weekday as a literal Name: /sunday..saturday.
// throws: type-check, opstack-underflow, vm-full
static void instant_weekday_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_utc(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_weekday(trx, parts.weekday);
}

//===--- Local-zone accessors ---===//

// instant-year-local: ulong -- int
// Returns the local-zone calendar year.
// throws: type-check, opstack-underflow, unsupported
static void instant_year_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.year);
}

// instant-month-local: ulong -- int
// Returns the local-zone calendar month, 1..12.
// throws: type-check, opstack-underflow, unsupported
static void instant_month_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.month);
}

// instant-day-local: ulong -- int
// Returns the local-zone day-of-month, 1..31.
// throws: type-check, opstack-underflow, unsupported
static void instant_day_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.day);
}

// instant-hour-local: ulong -- int
// Returns the local-zone hour, 0..23.
// throws: type-check, opstack-underflow, unsupported
static void instant_hour_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.hour);
}

// instant-minute-local: ulong -- int
// Returns the local-zone minute, 0..59.  Differs from UTC for zones
// with a non-whole-hour offset (e.g. India, Newfoundland).
// throws: type-check, opstack-underflow, unsupported
static void instant_minute_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.minute);
}

// instant-second-local: ulong -- int
// Returns the local-zone second-of-minute, 0..59.
// throws: type-check, opstack-underflow, unsupported
static void instant_second_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_integer(trx, parts.second);
}

// instant-weekday-local: ulong -- name
// Returns the local-zone weekday as a Name.  Differs from the UTC
// weekday near midnight when the zone offset crosses a day boundary.
// throws: type-check, opstack-underflow, unsupported, vm-full
static void instant_weekday_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);
    auto parts = trx->split_instant_local(trx->m_op_ptr->ulong_value(trx));
    replace_ulong_with_weekday(trx, parts.weekday);
}

//===--- Tier 2: udate constructor + accessors ---===//
//
// A udate is a 32-bit packed UInteger holding year/month/day.  See
// chrono.inl for the bit layout and pack/unpack helpers.

// make-date: year month day -- udate
// Validates calendar correctness; raises /range-check on out-of-range
// fields or invalid calendar dates (e.g. Feb 30, year > 16383).
// throws: type-check, opstack-underflow, range-check
static void make_date_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyInteger);

    auto day = trx->m_op_ptr->integer_value();
    auto month = (trx->m_op_ptr - 1)->integer_value();
    auto year = (trx->m_op_ptr - 2)->integer_value();

    auto udate = trx->pack_udate(year, month, day);

    trx->m_op_ptr -= 2;
    *trx->m_op_ptr = Object::make_uinteger(udate);
}

// date-year: udate -- int
// Returns the calendar year (0..16383).
// throws: type-check, opstack-underflow, range-check
static void date_year_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);
    auto ymd = trx->unpack_udate(trx->m_op_ptr->uinteger_value());
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(static_cast<int>(ymd.year())));
}

// date-month: udate -- int
// Returns the calendar month (1..12).
// throws: type-check, opstack-underflow, range-check
static void date_month_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);
    auto ymd = trx->unpack_udate(trx->m_op_ptr->uinteger_value());
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(static_cast<unsigned>(ymd.month())));
}

// date-day: udate -- int
// Returns the day-of-month (1..31).
// throws: type-check, opstack-underflow, range-check
static void date_day_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);
    auto ymd = trx->unpack_udate(trx->m_op_ptr->uinteger_value());
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(static_cast<unsigned>(ymd.day())));
}

// date-weekday: udate -- name
// Returns the weekday as a Name: /sunday .. /saturday.
// throws: type-check, opstack-underflow, range-check, vm-full
static void date_weekday_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);
    auto ymd = trx->unpack_udate(trx->m_op_ptr->uinteger_value());
    const std::chrono::sys_days dp{ymd};
    auto idx = std::chrono::weekday{dp}.c_encoding();
    assert(idx < 7);
    *trx->m_op_ptr = Name::make(trx, kWeekdayNames[idx]);
}

// day-of-year: udate -- int
// Returns the 1-based day of the calendar year (1..366).
// throws: type-check, opstack-underflow, range-check
static void day_of_year_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);
    auto ymd = trx->unpack_udate(trx->m_op_ptr->uinteger_value());
    const std::chrono::sys_days dp{ymd};
    const std::chrono::sys_days jan1{ymd.year() / std::chrono::January / 1};
    auto doy = (dp - jan1).count() + 1;
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(doy));
}

//===--- Tier 2: calendar arithmetic + predicates ---===//

// Helper: clamp `target` to a valid calendar date by clamping its day
// to the last day of the target month if the constructed date is
// invalid (e.g. Jan 31 + 1 month = Feb 31 -> Feb 28/29).
static std::chrono::year_month_day clamp_to_valid_ymd(std::chrono::year_month_day target) {
    if (target.ok()) {
        return target;
    } else {
        auto last = std::chrono::year_month_day_last{target.year() / target.month() / std::chrono::last};
        return std::chrono::year_month_day{target.year(), target.month(), last.day()};
    }
}

// date-add-days: udate delta -- udate
// Adds a signed number of days to a udate.
// throws: type-check, opstack-underflow, range-check
static void date_add_days_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyUInteger);

    auto delta = trx->m_op_ptr->integer_value();
    auto udate_ptr = (trx->m_op_ptr - 1);
    auto ymd = trx->unpack_udate(udate_ptr->uinteger_value());

    std::chrono::sys_days dp{ymd};
    dp += std::chrono::days{delta};
    auto result = trx->pack_udate_from_ymd(std::chrono::year_month_day{dp});

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_uinteger(result);
}

// date-add-months: udate delta -- udate
// Adds a signed number of months.  Day is clamped to the last valid
// day of the target month (e.g. Jan 31 + 1 month -> Feb 28/29).
// throws: type-check, opstack-underflow, range-check
static void date_add_months_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyUInteger);

    auto delta = trx->m_op_ptr->integer_value();
    auto udate_ptr = (trx->m_op_ptr - 1);
    auto ymd = trx->unpack_udate(udate_ptr->uinteger_value());

    auto target = clamp_to_valid_ymd(ymd + std::chrono::months{delta});
    auto result = trx->pack_udate_from_ymd(target);

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_uinteger(result);
}

// date-add-years: udate delta -- udate
// Adds a signed number of years.  Feb 29 in a leap year clamps to
// Feb 28 in a non-leap target year.
// throws: type-check, opstack-underflow, range-check
static void date_add_years_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyUInteger);

    auto delta = trx->m_op_ptr->integer_value();
    auto udate_ptr = (trx->m_op_ptr - 1);
    auto ymd = trx->unpack_udate(udate_ptr->uinteger_value());

    auto target = clamp_to_valid_ymd(ymd + std::chrono::years{delta});
    auto result = trx->pack_udate_from_ymd(target);

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_uinteger(result);
}

// date-diff-days: udate-a udate-b -- int
// Returns days(b) - days(a), signed.  Positive means b is later.
// throws: type-check, opstack-underflow, range-check
static void date_diff_days_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger, VerifyUInteger);

    auto b_ymd = trx->unpack_udate(trx->m_op_ptr->uinteger_value());
    auto a_ymd = trx->unpack_udate((trx->m_op_ptr - 1)->uinteger_value());

    const std::chrono::sys_days a_dp{a_ymd};
    const std::chrono::sys_days b_dp{b_ymd};
    auto diff = (b_dp - a_dp).count();

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(diff));
}

// leap-year?: int -- bool
// Returns true if `year` is a Gregorian leap year.
// throws: type-check, opstack-underflow
static void is_leap_year_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);

    auto year = trx->m_op_ptr->integer_value();
    if ((year < 0) || (year > UDATE_YEAR_MAX)) {
        // std::chrono::year stores a short and silently truncates mod 2^16; reject
        // out-of-domain years rather than reporting the leap-ness of a wrapped value.
        trx->error(Error::RangeCheck, "leap-year?: year {} out of udate range 0..{}", year, UDATE_YEAR_MAX);
    } else {
        const bool leap = std::chrono::year{static_cast<int>(year)}.is_leap();

        *trx->m_op_ptr = Object::make_boolean(leap);
    }
}

// days-in-month: year month -- int
// Returns the number of days in (year, month).  28..31.
// throws: type-check, opstack-underflow, range-check
static void days_in_month_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger);

    auto month = trx->m_op_ptr->integer_value();
    auto year_ptr = (trx->m_op_ptr - 1);
    auto year = year_ptr->integer_value();

    if ((month < 1) || (month > 12)) {
        trx->error(Error::RangeCheck, "days-in-month: month {} out of range 1..12", month);
    } else if ((year < 0) || (year > UDATE_YEAR_MAX)) {
        // std::chrono::year stores a short and silently truncates mod 2^16; reject
        // out-of-domain years rather than returning a wrapped year's month length.
        trx->error(Error::RangeCheck, "days-in-month: year {} out of udate range 0..{}", year, UDATE_YEAR_MAX);
    } else {
        auto last = std::chrono::year_month_day_last{std::chrono::year{static_cast<int>(year)} /
                                                     std::chrono::month{static_cast<unsigned>(month)} / std::chrono::last};
        auto days = static_cast<unsigned>(last.day());

        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(days));
    }
}

//===--- Tier 2: instant <-> udate conversion ---===//

// instant-to-date: ulong -- udate
// Converts a ULong instant to its UTC calendar date.  Time-of-day is
// dropped.  Pre-1970 instants are not representable as ULong; this op
// only sees non-negative ms.
// throws: type-check, opstack-underflow, range-check
static void instant_to_date_op(Trix *trx) {
    trx->verify_operands(VerifyULong);

    auto ms = trx->m_op_ptr->ulong_value(trx);
    auto signed_ms = static_cast<int64_t>(ms);
    auto utc_tp = std::chrono::sys_time<std::chrono::seconds>{std::chrono::seconds{signed_ms / 1000}};
    auto dp = std::chrono::floor<std::chrono::days>(utc_tp);
    auto udate = trx->pack_udate_from_ymd(std::chrono::year_month_day{dp});

    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = Object::make_uinteger(udate);
}

// instant-to-date-local: ulong -- udate
// Same as instant-to-date but interprets the instant in the local
// time zone.  Raises /unsupported if the local zone is unavailable.
// throws: type-check, opstack-underflow, range-check, unsupported
static void instant_to_date_local_op(Trix *trx) {
    trx->verify_operands(VerifyULong);

    auto ms = trx->m_op_ptr->ulong_value(trx);
    auto signed_ms = static_cast<int64_t>(ms);
    auto utc_tp = std::chrono::sys_time<std::chrono::seconds>{std::chrono::seconds{signed_ms / 1000}};

    const auto *zone = trx->current_local_zone_or_raise();
    const std::chrono::zoned_time<std::chrono::seconds> zt{zone, utc_tp};
    auto local_tp = zt.get_local_time();
    auto dp = std::chrono::floor<std::chrono::days>(local_tp);
    auto udate = trx->pack_udate_from_ymd(std::chrono::year_month_day{std::chrono::sys_days{dp.time_since_epoch()}});

    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = Object::make_uinteger(udate);
}

// make-instant: udate hour minute second ms -- ulong
// Combines a calendar date and a time-of-day into a UTC instant.
// throws: type-check, opstack-underflow, range-check
static void make_instant_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyInteger, VerifyInteger, VerifyUInteger);

    auto ms_val = trx->m_op_ptr->integer_value();
    auto second = (trx->m_op_ptr - 1)->integer_value();
    auto minute = (trx->m_op_ptr - 2)->integer_value();
    auto hour = (trx->m_op_ptr - 3)->integer_value();
    auto udate_obj = (trx->m_op_ptr - 4);
    auto ymd = trx->unpack_udate(udate_obj->uinteger_value());

    if ((hour < 0) || (hour > 23)) {
        trx->error(Error::RangeCheck, "make-instant: hour {} out of range 0..23", hour);
    } else if ((minute < 0) || (minute > 59)) {
        trx->error(Error::RangeCheck, "make-instant: minute {} out of range 0..59", minute);
    } else if ((second < 0) || (second > 59)) {
        trx->error(Error::RangeCheck, "make-instant: second {} out of range 0..59", second);
    } else if ((ms_val < 0) || (ms_val > 999)) {
        trx->error(Error::RangeCheck, "make-instant: ms {} out of range 0..999", ms_val);
    } else {
        const std::chrono::sys_days dp{ymd};
        auto sec_total =
                dp.time_since_epoch() + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sec_total).count() + ms_val;

        if (total_ms < 0) {
            trx->error(Error::RangeCheck, "make-instant: pre-1970 instants are not representable as ULong");
        } else {
            trx->m_op_ptr -= 4;
            *trx->m_op_ptr = Object::make_ulong(trx, static_cast<ulong_t>(total_ms));
        }
    }
}

// make-instant-local: udate hour minute second ms -- ulong
// Combines a calendar date and a time-of-day, interpreting them in
// the local time zone, into a UTC ULong instant.  Raises
// /unsupported if the local zone is unavailable.
// throws: type-check, opstack-underflow, range-check, unsupported
static void make_instant_local_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyInteger, VerifyInteger, VerifyUInteger);

    auto ms_val = trx->m_op_ptr->integer_value();
    auto second = (trx->m_op_ptr - 1)->integer_value();
    auto minute = (trx->m_op_ptr - 2)->integer_value();
    auto hour = (trx->m_op_ptr - 3)->integer_value();
    auto udate_obj = (trx->m_op_ptr - 4);
    auto ymd = trx->unpack_udate(udate_obj->uinteger_value());

    if ((hour < 0) || (hour > 23)) {
        trx->error(Error::RangeCheck, "make-instant-local: hour {} out of range 0..23", hour);
    } else if ((minute < 0) || (minute > 59)) {
        trx->error(Error::RangeCheck, "make-instant-local: minute {} out of range 0..59", minute);
    } else if ((second < 0) || (second > 59)) {
        trx->error(Error::RangeCheck, "make-instant-local: second {} out of range 0..59", second);
    } else if ((ms_val < 0) || (ms_val > 999)) {
        trx->error(Error::RangeCheck, "make-instant-local: ms {} out of range 0..999", ms_val);
    } else {
        const std::chrono::local_days dp{ymd};
        auto local_tp = std::chrono::local_time<std::chrono::seconds>{dp.time_since_epoch() + std::chrono::hours{hour} +
                                                                      std::chrono::minutes{minute} + std::chrono::seconds{second}};
        const auto *zone = trx->current_local_zone_or_raise();
        // Resolve DST gaps/overlaps deterministically: the 1-arg to_sys throws
        // nonexistent_local_time / ambiguous_local_time, which the interpreter treats as an
        // uncatchable std::exception and tears the VM down -- a valid-looking wall-clock time
        // in a spring-forward gap or fall-back overlap must not crash the process. choose::earliest
        // picks the pre-transition offset (gap) / earlier instant (overlap).
        auto sys_tp = zone->to_sys(local_tp, std::chrono::choose::earliest);
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sys_tp.time_since_epoch()).count() + ms_val;

        if (total_ms < 0) {
            trx->error(Error::RangeCheck, "make-instant-local: pre-1970 instants are not representable as ULong");
        } else {
            trx->m_op_ptr -= 4;
            *trx->m_op_ptr = Object::make_ulong(trx, static_cast<ulong_t>(total_ms));
        }
    }
}
