#pragma once
// Calendar arithmetic without libc time zones (pure, host-testable).
//
// "Local seconds" = seconds since 1970-01-01 00:00:00 of the wall clock (what the DS3231 and
// arc::clock::nowNs() hold: local time, no UTC). Europe/Berlin is the only zone: UTC values
// from CalDAV are converted with the EU summer time rule.
#include <stdint.h>

namespace ac {

struct Civil {
    int year = 1970, month = 1, day = 1;
    int hour = 0, minute = 0, second = 0;
};

int64_t daysFromCivil(int y, int m, int d);
void civilFromDays(int64_t days, int& y, int& m, int& d);

int64_t toSec(const Civil& c);       // wall-clock fields -> local seconds
Civil fromSec(int64_t sec);          // local seconds -> wall-clock fields
int64_t dayStart(int64_t sec);       // midnight of that day
int64_t daysOf(int64_t sec);         // whole days since the epoch

int weekday(int64_t days);           // 1 = Monday ... 7 = Sunday
int isoWeek(int y, int m, int d);

// English names. wd/m as below. wd: 1..7 Monday..Sunday, m: 1..12
const char* weekdayName(int wd, bool shortName);
const char* monthName(int m, bool shortName);

// Europe/Berlin
int utcOffsetBerlin(int64_t utcSec);
int64_t utcToLocal(int64_t utcSec);
int64_t localToUtc(int64_t localSec);

// "20261013", "20261013T143000", "20261013T143000Z" -> fields. Returns false if malformed.
bool parseIcalDateTime(const char* s, Civil& out, bool& hasTime, bool& utc);
// "2026-10-13" and "2026-10-13T07:12" (Open-Meteo, local). Time optional.
bool parseIsoDateTime(const char* s, Civil& out, bool& hasTime);

}  // namespace ac
