#pragma once
// Moon phase from the synodic month (pure). Accuracy about ±0.5 day, plenty for a desk clock.
#include <stdint.h>

namespace ac {

struct MoonInfo {
    double age = 0;        // days since the last new moon, 0 .. 29.53
    double illum = 0;      // illuminated fraction 0..1
    int phase = 0;         // 0 new, 1 waxing crescent, 2 first quarter, 3 waxing gibbous,
                           // 4 full, 5 waning gibbous, 6 last quarter, 7 waning crescent
};

constexpr double kSynodicMonth = 29.530588853;

// `utcSec`: seconds since 1970 UTC (a local-time value is off by an hour: irrelevant here).
MoonInfo moonAt(int64_t utcSec);

// Time of the next new moon (target 0) or full moon (target 0.5), seconds since 1970 as given.
int64_t nextMoonEvent(int64_t utcSec, bool full);

const char* moonPhaseName(int phase);   // English

}  // namespace ac
