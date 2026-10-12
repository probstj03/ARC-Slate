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
    m.phase = (int)floor(frac * 8.0 + 0.5) % 8;
    return m;
}

int64_t nextMoonEvent(int64_t utcSec, bool full) {
    const double target = full ? kSynodicMonth / 2.0 : 0.0;
    double d = target - ageAt(utcSec);
    if (d <= 0) d += kSynodicMonth;
    return utcSec + (int64_t)(d * 86400.0);
}

const char* moonPhaseName(int phase) {
    static const char* const names[] = {"Neumond", "Zunehmende Sichel", "Erstes Viertel", "Zunehmender Mond",
                                        "Vollmond", "Abnehmender Mond", "Letztes Viertel", "Abnehmende Sichel"};
    return (phase >= 0 && phase < 8) ? names[phase] : "?";
}

}  // namespace ac
