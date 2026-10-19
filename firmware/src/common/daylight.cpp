#include "daylight.h"
#include <Preferences.h>
#include <math.h>

namespace daylight {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

int dayOfYear(int y, int m, int d) {
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return cum[m - 1] + d + (leap && m > 2 ? 1 : 0);
}

// 1 = Monday .. 7 = Sunday (Zeller-like, Gregorian)
int weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    const int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   // 0 = Sunday
    return w == 0 ? 7 : w;
}

int lastSunday(int y, int m) {
    const int wd = weekday(y, m, 31);
    return 31 - (wd % 7);
}

}  // namespace

bool sunTimes(int year, int month, int day, double lat, double lon, int utcOffsetMin, int& riseMin, int& setMin) {
    const int n = dayOfYear(year, month, day);
    const double g = 2.0 * kPi / 365.0 * (n - 1);          // fractional year at noon is good enough
    const double eqTime = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
    const double decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g) -
                        0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
    const double cosH = (cos(90.833 * kDeg) - sin(lat * kDeg) * sin(decl)) / (cos(lat * kDeg) * cos(decl));
    if (cosH > 1.0 || cosH < -1.0) return false;
    const double ha = acos(cosH) / kDeg;                    // degrees
    const double noonUtc = 720.0 - 4.0 * lon - eqTime;      // minutes
    riseMin = (int)lround(noonUtc - 4.0 * ha) + utcOffsetMin;
    setMin = (int)lround(noonUtc + 4.0 * ha) + utcOffsetMin;
    return true;
}

int cetOffsetMin(int year, int month, int day, int hourLocal) {
    if (month < 3 || month > 10) return 60;
    if (month > 3 && month < 10) return 120;
    const int ls = lastSunday(year, month);
    if (month == 3) return (day > ls || (day == ls && hourLocal >= 2)) ? 120 : 60;
    return (day < ls || (day == ls && hourLocal < 3)) ? 120 : 60;
}

uint8_t level(int minuteOfDay, int riseMin, int setMin, uint8_t day, uint8_t night) {
    auto mix = [&](int fromNight) -> uint8_t {              // 0 .. 2*TWILIGHT: night .. day
        if (fromNight <= 0) return night;
        if (fromNight >= 2 * TWILIGHT_MIN) return day;
        return (uint8_t)lround(night + (double)(day - night) * fromNight / (2.0 * TWILIGHT_MIN));
    };
    const int t = minuteOfDay;
    if (t < (riseMin + setMin) / 2) return mix(t - (riseMin - TWILIGHT_MIN));
    return mix((setMin + TWILIGHT_MIN) - t);
}

void place(double& lat, double& lon) {
    lat = 49.4756;
    lon = 11.7994;
    Preferences p;
    if (p.begin("clock", true)) {
        lat = p.getFloat("lat", (float)lat);
        lon = p.getFloat("lon", (float)lon);
        p.end();
    }
}

}  // namespace daylight
