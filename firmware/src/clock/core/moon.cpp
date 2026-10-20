#include "moon.h"
#include <math.h>

namespace ac {

namespace {

// New moon 2000-01-06 18:14 UTC.
constexpr double kRefNewMoonUnix = 947182440.0;
constexpr double kPi = 3.14159265358979323846;

double ageAt(int64_t utcSec) {
    const double days = ((double)utcSec - kRefNewMoonUnix) / 86400.0;
    double a = fmod(days, kSynodicMonth);
    if (a < 0) a += kSynodicMonth;
    return a;
}

}  // namespace

MoonInfo moonAt(int64_t utcSec) {
    MoonInfo m;
    m.age = ageAt(utcSec);
    const double frac = m.age / kSynodicMonth;
    m.illum = (1.0 - cos(2.0 * kPi * frac)) / 2.0;
    // Phase name from the lit fraction, so that it fits the picture (an octant by age called a 68 % moon "first
    // quarter"): new < 6 %, crescent < 38 %, quarter < 62 %, gibbous < 94 %, full above; waxing in the first half.
    const bool waxing = frac < 0.5;
    if (m.illum < 0.06) m.phase = 0;
    else if (m.illum >= 0.94) m.phase = 4;
    else if (m.illum < 0.38) m.phase = waxing ? 1 : 7;
    else if (m.illum < 0.62) m.phase = waxing ? 2 : 6;
    else m.phase = waxing ? 3 : 5;
    return m;
}

int64_t nextMoonEvent(int64_t utcSec, bool full) {
    const double target = full ? kSynodicMonth / 2.0 : 0.0;
    double d = target - ageAt(utcSec);
    if (d <= 0) d += kSynodicMonth;
    return utcSec + (int64_t)(d * 86400.0);
}

const char* moonPhaseName(int phase) {
    static const char* const names[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                        "Full moon", "Waning gibbous", "Last quarter", "Waning crescent"};
    return (phase >= 0 && phase < 8) ? names[phase] : "?";
}

}  // namespace ac
