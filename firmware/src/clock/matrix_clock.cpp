#include "matrix_clock.h"
#include <ArcCore.h>
#include "clock_config.h"
#include "core/agenda.h"
#include "assets/weather_icons_night.h"
#include "core/anim.h"
#include "core/civil.h"
#include "daylight.h"
#include "data_store.h"
#include "matrix.h"
#include "pins.h"

namespace {

// ── what the display needs from the data store (copied once per second) ──
struct View {
    bool wxOk = false;
    int16_t temp10 = 0;
    uint8_t wmo = 0, wind = 0;
    bool isDay = true;
    int64_t sunrise = 0, sunset = 0;       // of today, local seconds (0 = unknown)
    bool haveNext = false, nextOngoing = false;
    ac::Event next;
};

View s_view;

// field layout of the data chain (the same as the slate: matrix_slate.cpp); the middle row is mounted rotated
constexpr int F_LEFT = 0, F_MID = 3, F_RIGHT = 6;
constexpr int TC_W = 56;                   // TC chain: 7 modules

// scrolling text on the data chain: lead-in of one display width, the text, lead-out
constexpr int kTextCap = 64 * 6;
constexpr int kScrollCap = TC_W + kTextCap + TC_W;
uint8_t s_scrollCols[kScrollCap];
int s_scrollWidth = 0;
int s_scrollPos = -1;                      // -1 = not scrolling
int s_scrollRepeat = 0;                    // further passes of the same text
uint32_t s_scrollNextMs = 0;
volatile bool s_scrollRequest = false;
volatile int s_scrollRequestRepeat = 0;
char s_scrollText[72];
portMUX_TYPE s_scrollMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t s_sparkleUntil = 0;
volatile bool s_showNext = false;

struct Announced { int64_t start = 0; int stage = 0; } s_announced;

bool s_night = false;
volatile bool s_brightnessSaveDue = false;
volatile uint32_t s_brightnessSaveAt = 0;

int64_t nowSec() { return arc::clock::nowNs() / 1000000000LL; }

void refreshView(int64_t sec) {
    View v;
    clockdata::lock();
    const clockdata::Data& d = clockdata::data();
    if (d.weather.valid) {
        const ac::Weather& w = d.weather;
        v.wxOk = true;
        v.temp10 = w.temp10;
        v.wmo = w.wmo;
        v.wind = w.windKmh;
        v.isDay = w.isDay;
        const int di = (int)((ac::dayStart(sec) - w.day0) / 86400);
        if (di >= 0 && di < 3) { v.sunrise = w.day[di].sunrise; v.sunset = w.day[di].sunset; }
    }
    bool ongoing = false;
    const int n = d.agenda.nextTimed(sec, ongoing);
    if (n >= 0 && ac::dayStart(d.agenda.ev[n].start) <= ac::dayStart(sec)) {     // only events of today
        v.haveNext = true;
        v.nextOngoing = ongoing;
        v.next = d.agenda.ev[n];
    }
    clockdata::unlock();
    s_view = v;
}

// ── brightness: follows the sun ──────────────────────────────────────────

void applyBrightness(int64_t sec) {
    static int lastApplied = -1;
    const ac::Civil c = ac::fromSec(sec);
    const int minute = c.hour * 60 + c.minute;
    int rise, set;
    if (s_view.sunrise && s_view.sunset) {               // from the weather service
        rise = (int)((s_view.sunrise - ac::dayStart(sec)) / 60);
        set = (int)((s_view.sunset - ac::dayStart(sec)) / 60);
    } else {                                             // computed locally
        const ClockConfig cfg = clockcfg::copy();
        const int offset = (int)((sec - ac::localToUtc(sec)) / 60);
        if (!daylight::sunTimes(c.year, c.month, c.day, cfg.lat, cfg.lon, offset, rise, set)) { rise = 7 * 60; set = 19 * 60; }
    }
    s_night = minute < rise || minute >= set;
    const ClockConfig cfg = clockcfg::copy();
    const int b = daylight::level(minute, rise, set, cfg.brightnessDay, cfg.brightnessNight);
    if (b != lastApplied) {
        lastApplied = b;
        mtx::setBrightness((uint8_t)b);
    }
}

// ── flash (LD06AJSA on the slate's flash output) ─────────────────────────

void flashTask(void*) {
    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_FLASH, HIGH);
        vTaskDelay(pdMS_TO_TICKS(60));
        digitalWrite(PIN_FLASH, LOW);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelete(nullptr);
}

void flash() {
    const ClockConfig c = clockcfg::copy();
    if (!c.reminderFlash || s_night || !mtx::enabled()) return;   // not at night, not with the lid closed
    xTaskCreate(flashTask, "FLASH", 2048, nullptr, 1, nullptr);
}

// ── reminders ────────────────────────────────────────────────────────────

void startScrollText(const char* text, int repeat);

void eventText(const ac::Event& e, char* out, size_t cap) {
    const ac::Civil t = ac::fromSec(e.start);
    snprintf(out, cap, "%02d:%02d %s", t.hour, t.minute, e.summary);
}

void checkReminder(int64_t sec) {
    const ClockConfig c = clockcfg::copy();
    if (!c.reminderMin || !s_view.haveNext) return;
    const ac::Event& e = s_view.next;
    const int64_t delta = e.start - sec;
    int stage = 0;
    if (delta <= 0 && sec < e.start + 60) stage = 3;
    else if (delta > 0 && delta <= 60) stage = 2;
    else if (delta > 60 && delta <= (int64_t)c.reminderMin * 60) stage = 1;
    if (stage == 0) return;
    if (s_announced.start == e.start && s_announced.stage >= stage) return;
    s_announced.start = e.start;
    s_announced.stage = stage;
    char ev[64], text[72];
    eventText(e, ev, sizeof(ev));
    if (stage == 1) snprintf(text, sizeof(text), "IN %d MIN: %s", (int)((delta + 59) / 60), ev);
    else if (stage == 2) snprintf(text, sizeof(text), "SOON: %s", ev);
    else snprintf(text, sizeof(text), "NOW: %s", e.summary);
    startScrollText(text, stage == 3 ? 2 : 1);
    if (stage == 3 || stage == 1) flash();
}

// ── TC chain: the time, centred ──────────────────────────────────────────

void drawClock(int64_t sec) {
    const ac::Civil c = ac::fromSec(sec);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", c.hour, c.minute, c.second);
    uint8_t cols[56];
    const int used = mtx::renderText(buf, cols, 56, 0);
    mtx::renderText(buf, cols, 56, (56 - used) / 2);         // centred
    mtx::place(mtx::CHAIN_TC, cols, 56, 0, 7, false);
}

// ── data chain: a 24x24 square (3 rows of 3 modules) ─────────────────────
// Rows from top to bottom: F_LEFT, F_MID (mounted rotated), F_RIGHT. The top two rows show the weather icon
// (24x16, animated at 800 ms per frame; the sun stands still), the bottom row the temperature.

void putRow(const ac::Canvas& cv, int row) {
    static const int first[3] = {F_LEFT, F_MID, F_RIGHT};
    mtx::place(mtx::CHAIN_DATA, cv.col, ac::kFieldW, first[row], 3, row == 1);
}

struct Anim { const uint8_t* const* frames; int len; };

Anim animFor(bool wxOk, uint8_t wmo, bool day, uint8_t windKmh) {
    if (!wxOk) return {nullptr, 0};
    const ac::WxKind k = ac::kindFromWmo(wmo);
    const bool windy = windKmh >= 40;
    switch (k) {
        case ac::WxKind::Clear:
            if (windy) return {anim16_wind, ANIM16_WIND_LEN};
            return day ? Anim{anim16_sunny, 1} : Anim{anim16_clear_night, ANIM16_CLEAR_NIGHT_LEN};   // the sun stands still
        case ac::WxKind::PartlyCloudy:
            return day ? Anim{anim16_partly_cloudy, ANIM16_PARTLY_CLOUDY_LEN}
                       : Anim{anim16_partly_cloudy_night, ANIM16_PARTLY_CLOUDY_NIGHT_LEN};
        case ac::WxKind::Cloudy:
            if (windy) return {anim16_wind, ANIM16_WIND_LEN};
            return {anim16_cloudy, ANIM16_CLOUDY_LEN};
        case ac::WxKind::Fog: return {anim16_fog, ANIM16_FOG_LEN};
        case ac::WxKind::Drizzle:
        case ac::WxKind::Showers: return {anim16_rain, ANIM16_RAIN_LEN};
        case ac::WxKind::Rain: return {anim16_heavy_rain, ANIM16_HEAVY_RAIN_LEN};
        case ac::WxKind::Snow: return {anim16_snow, ANIM16_SNOW_LEN};
        case ac::WxKind::Thunder: return {anim16_thunderstorm, ANIM16_THUNDERSTORM_LEN};
        default: return {nullptr, 0};
    }
}

// 24x16 bitmap (3 bytes per row, MSB left) -> the two upper rows. `still`: the top-left area (the sun of
// "partly cloudy", columns 0..10 of the top half) is taken from that frame, so only the clouds move.
void drawIcon(const uint8_t* bmp, const uint8_t* still = nullptr) {
    for (int half = 0; half < 2; half++) {
        ac::Canvas cv;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 24; x++) {
                const uint8_t* src = (still && half == 0 && x <= 10) ? still : bmp;
                if ((src[(half * 8 + y) * 3 + x / 8] >> (7 - x % 8)) & 1) cv.set(x, y);
            }
        putRow(cv, half);
    }
}

void drawTemperature() {
    char tt[12];
    const int t = (int)lroundf(s_view.temp10 / 10.0f);
    if (s_view.wxOk) snprintf(tt, sizeof(tt), "%d^C", t);
    else snprintf(tt, sizeof(tt), "--^C");
    uint8_t cols[48];
    int w = mtx::renderText(tt, cols, sizeof(cols), 0);
    if (w > 24) {                                        // -10 and below: without the C
        snprintf(tt, sizeof(tt), "%d^", t);
        w = mtx::renderText(tt, cols, sizeof(cols), 0);
    }
    ac::Canvas cv;
    const int x0 = w < 24 ? (24 - w) / 2 : 0;
    for (int k = 0; k < w && x0 + k < 24; k++) cv.col[x0 + k] = cols[k];
    putRow(cv, 2);
}

// ── scrolling messages (reminders) on the TC chain ───────────────────────

void startScrollText(const char* text, int repeat) {
    portENTER_CRITICAL(&s_scrollMux);
    strlcpy(s_scrollText, text, sizeof(s_scrollText));
    s_scrollRequestRepeat = repeat;
    portEXIT_CRITICAL(&s_scrollMux);
    s_scrollRequest = true;
}

void beginScroll() {
    char text[sizeof(s_scrollText)];
    portENTER_CRITICAL(&s_scrollMux);
    memcpy(text, s_scrollText, sizeof(text));
    s_scrollRepeat = s_scrollRequestRepeat > 0 ? s_scrollRequestRepeat - 1 : 0;
    portEXIT_CRITICAL(&s_scrollMux);
    static uint8_t cols[kTextCap];
    const int used = mtx::renderText(text, cols, kTextCap, 0);
    memset(s_scrollCols, 0, sizeof(s_scrollCols));
    memcpy(s_scrollCols + TC_W, cols, used);
    s_scrollWidth = TC_W + used + TC_W;
    s_scrollPos = TC_W / 2;                   // start with the text already half way in
    s_scrollNextMs = 0;
}

// ── sparkle ──────────────────────────────────────────────────────────────

void drawSparkle(uint32_t remainingMs, uint32_t totalMs) {
    // random pixels, fewer and fewer (out of 1024 per pixel)
    const uint32_t density = 30 + (remainingMs * 200) / (totalMs ? totalMs : 1);
    auto randomCol = [&]() {
        uint8_t b = 0;
        for (int bit = 0; bit < 8; bit++)
            if ((esp_random() % 1024) < density) b |= (uint8_t)(1 << bit);
        return b;
    };
    uint8_t cols[56];
    for (int x = 0; x < 56; x++) cols[x] = randomCol();
    mtx::place(mtx::CHAIN_TC, cols, 56, 0, 7, false);
    for (int row = 0; row < 3; row++) {
        ac::Canvas cv;
        for (int x = 0; x < 24; x++) cv.col[x] = randomCol();
        putRow(cv, row);
    }
}

// ── renderer ─────────────────────────────────────────────────────────────

void render(uint32_t nowMs, bool redraw, bool tcOwned, mtx::Result& out) {
    static int64_t lastSec = -1;
    static uint32_t lastFrame = ~0u, lastSparkle = 0;
    static int lastTemp = -10000;
    static bool sparkling = false;

    const int64_t sec = nowSec();
    const bool secChanged = sec != lastSec;
    if (secChanged) {
        lastSec = sec;
        refreshView(sec);
        applyBrightness(sec);
        checkReminder(sec);
    }
    if (s_brightnessSaveDue && (int32_t)(nowMs - s_brightnessSaveAt) >= 0) {
        s_brightnessSaveDue = false;
        clockcfg::save();
    }
    if (s_showNext) {
        s_showNext = false;
        char ev[64], text[72];
        if (s_view.haveNext) {
            eventText(s_view.next, ev, sizeof(ev));
            snprintf(text, sizeof(text), "%s%s", s_view.nextOngoing ? "NOW: " : "NEXT: ", ev);
        } else {
            snprintf(text, sizeof(text), "NO MORE EVENTS TODAY");
        }
        startScrollText(text, 1);
    }
    if (s_scrollRequest) {
        s_scrollRequest = false;
        beginScroll();
    }

    // the sparkle effect owns both chains while it runs
    if ((int32_t)(s_sparkleUntil - nowMs) > 0) {
        if (nowMs - lastSparkle >= 70) {
            lastSparkle = nowMs;
            drawSparkle(s_sparkleUntil - nowMs, 1500);
            out.tcDirty = out.dataDirty = true;
        }
        sparkling = true;
        return;
    }
    if (sparkling) {
        sparkling = false;
        redraw = true;
    }

    // TC chain: a scrolling message, else the time
    if (!tcOwned) {
        if (s_scrollPos >= 0 && (int32_t)(nowMs - s_scrollNextMs) >= 0) {
            s_scrollNextMs = nowMs + 45;
            mtx::place(mtx::CHAIN_TC, s_scrollCols + s_scrollPos, TC_W, 0, 7, false);
            out.tcDirty = true;
            if (++s_scrollPos > s_scrollWidth - TC_W) {
                if (s_scrollRepeat > 0) { s_scrollRepeat--; s_scrollPos = TC_W / 2; }
                else { s_scrollPos = -1; redraw = true; }
            }
        }
        if (s_scrollPos < 0 && (redraw || secChanged)) {
            drawClock(sec);
            out.tcDirty = true;
        }
    }

    // data chain: weather icon (0.8 s per frame, calm) and temperature
    const Anim a = animFor(s_view.wxOk, s_view.wmo, s_view.isDay, s_view.wind);
    const uint32_t frame = nowMs / 800;
    if (redraw || frame != lastFrame) {
        lastFrame = frame;
        if (a.len) {
            const bool sunStill = a.frames == anim16_partly_cloudy;
            drawIcon(a.frames[frame % (uint32_t)a.len], sunStill ? a.frames[0] : nullptr);
        } else {                                         // no weather yet: a dot runs along
            ac::Canvas top;
            top.set((int)(frame % 24), 4);
            putRow(top, 0);
            putRow(ac::Canvas(), 1);
        }
        out.dataDirty = true;
    }
    const int temp = s_view.wxOk ? s_view.temp10 : -9999;
    if (redraw || temp != lastTemp) {
        lastTemp = temp;
        drawTemperature();
        out.dataDirty = true;
    }
}

}  // namespace

namespace clockmatrix {

void begin() {
    pinMode(PIN_FLASH, OUTPUT);
    digitalWrite(PIN_FLASH, LOW);
    mtx::setRenderer(render);
}

void scroll(const String& text) { startScrollText(text.c_str(), 1); }

void sparkle(uint32_t durationMs) { s_sparkleUntil = millis() + durationMs; }

void showNext() { s_showNext = true; }

void adjustBrightness(int delta) {
    ClockConfig& c = clockcfg::get();
    uint8_t& b = s_night ? c.brightnessNight : c.brightnessDay;
    const int v = constrain((int)b + delta, 0, 15);
    b = (uint8_t)v;
    mtx::setBrightness(b);
    mtx::showOverlay(String(s_night ? "NIGHT " : "DAY ") + v, 900);
    s_brightnessSaveDue = true;
    s_brightnessSaveAt = millis() + 2000;
}

bool isNight() { return s_night; }

}  // namespace clockmatrix
