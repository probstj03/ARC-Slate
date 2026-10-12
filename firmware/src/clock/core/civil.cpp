#include "civil.h"
#include <ctype.h>
#include <string.h>

namespace ac {

// Howard Hinnant's days_from_civil / civil_from_days (proleptic Gregorian).
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}

int64_t toSec(const Civil& c) {
    return daysFromCivil(c.year, c.month, c.day) * 86400 + c.hour * 3600 + c.minute * 60 + c.second;
}

int64_t daysOf(int64_t sec) {
    return sec >= 0 ? sec / 86400 : -((-sec + 86399) / 86400);
}

int64_t dayStart(int64_t sec) { return daysOf(sec) * 86400; }

Civil fromSec(int64_t sec) {
    Civil c;
    const int64_t days = daysOf(sec);
    const int64_t rem = sec - days * 86400;
    civilFromDays(days, c.year, c.month, c.day);
    c.hour = (int)(rem / 3600);
    c.minute = (int)((rem % 3600) / 60);
    c.second = (int)(rem % 60);
    return c;
}

int weekday(int64_t days) {
    // 1970-01-01 was a Thursday
    int64_t w = (days + 3) % 7;      // 0 = Monday
    if (w < 0) w += 7;
    return (int)w + 1;
}

int isoWeek(int y, int m, int d) {
    const int64_t days = daysFromCivil(y, m, d);
    const int wd = weekday(days);                    // 1..7
    const int64_t thursday = days + (4 - wd);        // the Thursday of this ISO week
    int ty, tm, td;
    civilFromDays(thursday, ty, tm, td);
    const int64_t jan1 = daysFromCivil(ty, 1, 1);
    return (int)((thursday - jan1) / 7) + 1;
}

const char* weekdayName(int wd, bool shortName) {
    static const char* const full[] = {"Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag"};
    static const char* const abbr[] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
    if (wd < 1 || wd > 7) return "?";
    return shortName ? abbr[wd - 1] : full[wd - 1];
}

const char* monthName(int m, bool shortName) {
    static const char* const full[] = {"Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli",
                                       "August", "September", "Oktober", "November", "Dezember"};
    static const char* const abbr[] = {"Jan", "Feb", "Mar", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
    if (m < 1 || m > 12) return "?";
    return shortName ? abbr[m - 1] : full[m - 1];
}

namespace {

// Day of month of the last Sunday.
int lastSunday(int year, int month) {
    const int64_t last = daysFromCivil(year, month, 31);
    return 31 - (weekday(last) % 7);       // Sunday (7) -> 0 days back
}

}  // namespace

int utcOffsetBerlin(int64_t utcSec) {
    const Civil c = fromSec(utcSec);
    // EU rule: summer time from last Sunday of March 01:00 UTC to last Sunday of October 01:00 UTC
    const int64_t start = daysFromCivil(c.year, 3, lastSunday(c.year, 3)) * 86400 + 3600;
    const int64_t end = daysFromCivil(c.year, 10, lastSunday(c.year, 10)) * 86400 + 3600;
    return (utcSec >= start && utcSec < end) ? 7200 : 3600;
}

int64_t utcToLocal(int64_t utcSec) { return utcSec + utcOffsetBerlin(utcSec); }

int64_t localToUtc(int64_t localSec) {
    int64_t guess = localSec - 3600;
    guess = localSec - utcOffsetBerlin(guess);
    return localSec - utcOffsetBerlin(guess);
}

namespace {

bool digits(const char* s, int n, int& out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

bool valid(const Civil& c) {
    return c.month >= 1 && c.month <= 12 && c.day >= 1 && c.day <= 31 && c.hour <= 23 && c.minute <= 59 && c.second <= 60;
}

}  // namespace

bool parseIcalDateTime(const char* s, Civil& out, bool& hasTime, bool& utc) {
    hasTime = utc = false;
    Civil c;
    if (!s || strlen(s) < 8) return false;
    if (!digits(s, 4, c.year) || !digits(s + 4, 2, c.month) || !digits(s + 6, 2, c.day)) return false;
    if (s[8] == 'T') {
        if (strlen(s) < 15) return false;
        if (!digits(s + 9, 2, c.hour) || !digits(s + 11, 2, c.minute) || !digits(s + 13, 2, c.second)) return false;
        hasTime = true;
        utc = s[15] == 'Z';
    }
    if (!valid(c)) return false;
    out = c;
    return true;
}

bool parseIsoDateTime(const char* s, Civil& out, bool& hasTime) {
    hasTime = false;
    Civil c;
    if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return false;
    if (!digits(s, 4, c.year) || !digits(s + 5, 2, c.month) || !digits(s + 8, 2, c.day)) return false;
    if ((s[10] == 'T' || s[10] == ' ') && strlen(s) >= 16 && s[13] == ':') {
        if (!digits(s + 11, 2, c.hour) || !digits(s + 14, 2, c.minute)) return false;
        if (strlen(s) >= 19 && s[16] == ':') digits(s + 17, 2, c.second);
        hasTime = true;
    }
    if (!valid(c)) return false;
    out = c;
    return true;
}

}  // namespace ac
