#pragma once
// Weather data model and the Open-Meteo parser (pure: ArduinoJson only, no Arduino).
#include <stddef.h>
#include <stdint.h>

namespace ac {

enum class WxKind : uint8_t { Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Showers, Snow, Thunder, Unknown };

WxKind kindFromWmo(int code);          // WMO weather interpretation code -> icon kind
const char* kindName(WxKind kind);     // German, ASCII

constexpr int kHours = 48;             // today and tomorrow, indexed from midnight of day 0

struct DayInfo {
    int64_t sunrise = 0;               // local seconds (0 = unknown)
    int64_t sunset = 0;
    int16_t tMin10 = 0, tMax10 = 0;    // 1/10 degree
    uint8_t popMax = 0;                // max precipitation probability %
    uint8_t wmo = 0;
};

struct Weather {
    bool valid = false;
    int64_t fetched = 0;               // local seconds
    int64_t day0 = 0;                  // local midnight of the first hourly entry
    // current conditions
    int16_t temp10 = 0, feels10 = 0;
    uint8_t wmo = 0;
    bool isDay = true;
    uint8_t humidity = 0;
    uint8_t windKmh = 0;
    // hourly (index = hours since day0)
    int16_t hTemp10[kHours] = {0};
    uint8_t hPop[kHours] = {0};
    uint8_t hWmo[kHours] = {0};
    // daily: 0 = day0, 1 = next day, 2 = day after
    DayInfo day[3];
};

// Parses an Open-Meteo forecast response (current + hourly + daily, timezone=Europe/Berlin,
// timeformat iso8601). Returns false if the document is unusable.
bool parseOpenMeteo(const char* json, size_t len, int64_t nowLocal, Weather& out);

}  // namespace ac
