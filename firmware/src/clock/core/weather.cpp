#include "weather.h"
#include <ArduinoJson.h>
#include <math.h>
#include "civil.h"

namespace ac {

WxKind kindFromWmo(int c) {
    if (c == 0) return WxKind::Clear;
    if (c == 1 || c == 2) return WxKind::PartlyCloudy;
    if (c == 3) return WxKind::Cloudy;
    if (c == 45 || c == 48) return WxKind::Fog;
    if (c >= 51 && c <= 57) return WxKind::Drizzle;
    if ((c >= 61 && c <= 67)) return WxKind::Rain;
    if (c >= 71 && c <= 77) return WxKind::Snow;
    if (c >= 80 && c <= 82) return WxKind::Showers;
    if (c == 85 || c == 86) return WxKind::Snow;
    if (c >= 95 && c <= 99) return WxKind::Thunder;
    return WxKind::Unknown;
}

const char* kindName(WxKind k) {
    switch (k) {
        case WxKind::Clear: return "Clear";
        case WxKind::PartlyCloudy: return "Partly cloudy";
        case WxKind::Cloudy: return "Cloudy";
        case WxKind::Fog: return "Fog";
        case WxKind::Drizzle: return "Drizzle";
        case WxKind::Rain: return "Rain";
        case WxKind::Showers: return "Showers";
        case WxKind::Snow: return "Snow";
        case WxKind::Thunder: return "Thunderstorm";
        default: return "?";
    }
}

namespace {

int16_t tenths(float v) { return (int16_t)lroundf(v * 10.0f); }

int64_t isoLocal(const char* s) {
    Civil c;
    bool hasTime;
    if (!s || !parseIsoDateTime(s, c, hasTime)) return 0;
    return toSec(c);
}

uint8_t clampU8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)lroundf(v); }

}  // namespace

bool parseOpenMeteo(const char* json, size_t len, int64_t nowLocal, Weather& out) {
    JsonDocument filter;
    filter["current"] = true;
    JsonObject h = filter["hourly"].to<JsonObject>();
    h["time"] = true;
    h["temperature_2m"] = true;
    h["precipitation_probability"] = true;
    h["weather_code"] = true;
    JsonObject d = filter["daily"].to<JsonObject>();
    d["time"] = true;
    d["weather_code"] = true;
    d["temperature_2m_max"] = true;
    d["temperature_2m_min"] = true;
    d["sunrise"] = true;
    d["sunset"] = true;
    d["precipitation_probability_max"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return false;

    Weather w;
    JsonObject cur = doc["current"];
    if (cur.isNull() || doc["hourly"]["time"].isNull()) return false;

    w.temp10 = tenths(cur["temperature_2m"] | 0.0f);
    w.feels10 = tenths(cur["apparent_temperature"] | (cur["temperature_2m"] | 0.0f));
    w.wmo = (uint8_t)(cur["weather_code"] | 0);
    w.isDay = (cur["is_day"] | 1) != 0;
    w.humidity = clampU8(cur["relative_humidity_2m"] | 0.0f);
    w.windKmh = clampU8(cur["wind_speed_10m"] | 0.0f);

    JsonArray times = doc["hourly"]["time"];
    JsonArray temps = doc["hourly"]["temperature_2m"];
    JsonArray pops = doc["hourly"]["precipitation_probability"];
    JsonArray codes = doc["hourly"]["weather_code"];
    if (times.size() == 0) return false;
    w.day0 = dayStart(isoLocal(times[0] | ""));
    if (w.day0 == 0) return false;
    for (size_t i = 0; i < times.size(); i++) {
        const int64_t t = isoLocal(times[i] | "");
        if (t == 0) continue;
        const int64_t idx = (t - w.day0) / 3600;
        if (idx < 0 || idx >= kHours) continue;
        w.hTemp10[idx] = tenths(temps[i] | 0.0f);
        w.hPop[idx] = clampU8(pops[i] | 0.0f);
        w.hWmo[idx] = (uint8_t)(codes[i] | 0);
    }

    JsonArray dTimes = doc["daily"]["time"];
    for (size_t i = 0; i < dTimes.size() && i < 3; i++) {
        DayInfo& di = w.day[i];
        di.sunrise = isoLocal(doc["daily"]["sunrise"][i] | "");
        di.sunset = isoLocal(doc["daily"]["sunset"][i] | "");
        di.tMax10 = tenths(doc["daily"]["temperature_2m_max"][i] | 0.0f);
        di.tMin10 = tenths(doc["daily"]["temperature_2m_min"][i] | 0.0f);
        di.popMax = clampU8(doc["daily"]["precipitation_probability_max"][i] | 0.0f);
        di.wmo = (uint8_t)(doc["daily"]["weather_code"][i] | 0);
    }

    w.fetched = nowLocal;
    w.valid = true;
    out = w;
    return true;
}

}  // namespace ac
