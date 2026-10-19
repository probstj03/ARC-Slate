#include "matrix_clock.h"
#include <ArcCore.h>
#include "clock_config.h"
#include "core/agenda.h"
#include "core/anim.h"
#include "core/civil.h"
#include "core/moon.h"
#include "data_store.h"
#include "matrix.h"

namespace {

// ── what the display needs from the data store (copied once per second) ──
struct View {
    bool wxOk = false;
    int16_t temp10 = 0, feels10 = 0;
    uint8_t wmo = 0, popNow = 0, wind = 0;
    bool isDay = true;
    int64_t sunrise = 0, sunset = 0;       // of today, local seconds (0 = unknown)
    bool haveNext = false, nextOngoing = false;
    ac::Event next;
};

View s_view;

constexpr uint32_t PAGE_MS = 4000;
constexpr uint32_t SUN_ICON_MS = 1600;
constexpr uint32_t SUN_TIME_MS = 3200;

// field layout of the data chain (the same as the slate: matrix_slate.cpp)
constexpr int F_LEFT = 0, F_MID = 3, F_RIGHT = 6;

// scrolling text on the TC chain: lead-in of one display width, the text, lead-out
constexpr int kScrollCap = 56 + 200 + 56;
uint8_t s_scrollCols[kScrollCap + 8];
int s_scrollWidth = 0;
int s_scrollPos = -1;                      // -1 = not scrolling
uint32_t s_scrollNextMs = 0;
volatile bool s_scrollRequest = false;
char s_scrollText[64];
portMUX_TYPE s_scrollMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t s_sparkleUntil = 0;
volatile uint32_t s_pageSkipMs = 0;        // button: each press jumps one page ahead
uint32_t s_blinkUntil = 0;                 // the right field blinks for a reminder

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
        v.feels10 = w.feels10;
        v.wmo = w.wmo;
        v.isDay = w.isDay;
        v.wind = w.windKmh;
        const int64_t idx = (sec - w.day0) / 3600;
        v.popNow = (idx >= 0 && idx < ac::kHours) ? w.hPop[idx] : 0;
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

bool computeNight(int64_t sec) {
    if (s_view.sunrise && s_view.sunset) return sec < s_view.sunrise || sec >= s_view.sunset;
    const int h = ac::fromSec(sec).hour;
    return h < 7 || h >= 21;
}

void applyBrightness(int64_t sec) {
    static int lastApplied = -1;
    s_night = computeNight(sec);
    const ClockConfig c = clockcfg::copy();
    const int b = s_night ? c.brightnessNight : c.brightnessDay;
    if (b != lastApplied) {
        lastApplied = b;
        mtx::setBrightness((uint8_t)b);
    }
}

// ── reminders ────────────────────────────────────────────────────────────

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
    const ac::Civil t = ac::fromSec(e.start);
    char text[64];
    if (stage == 1) snprintf(text, sizeof(text), "%02d:%02d %s IN %d MIN", t.hour, t.minute, e.summary, (int)((delta + 59) / 60));
    else if (stage == 2) snprintf(text, sizeof(text), "SOON %s", e.summary);
    else snprintf(text, sizeof(text), "NOW %s", e.summary);
    clockmatrix::scroll(text);
    s_blinkUntil = millis() + 10000;
}

// ── TC chain ─────────────────────────────────────────────────────────────

void drawClock(int64_t sec) {
    const ac::Civil c = ac::fromSec(sec);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", c.hour, c.minute, c.second);
    uint8_t cols[56];
    const int used = mtx::renderText(buf, cols, 56, 0);
    mtx::renderText(buf, cols, 56, used < 56 ? (56 - used) / 2 : 0);
    mtx::place(mtx::CHAIN_TC, cols, 56, 0, 7, false);
}

void startScroll() {
    char text[sizeof(s_scrollText)];
    portENTER_CRITICAL(&s_scrollMux);
    memcpy(text, s_scrollText, sizeof(text));
    portEXIT_CRITICAL(&s_scrollMux);
    uint8_t cols[256];
    const int used = mtx::renderText(text, cols, sizeof(cols), 0);
    const int width = used < 200 ? used + 1 : 200;
    memset(s_scrollCols, 0, sizeof(s_scrollCols));
    memcpy(s_scrollCols + 56, cols, width);
    s_scrollWidth = 56 + width + 56;
    s_scrollPos = 0;
    s_scrollNextMs = 0;
}

// ── data chain ───────────────────────────────────────────────────────────

void putCanvas(const ac::Canvas& cv, int first, bool flipped) {
    mtx::place(mtx::CHAIN_DATA, cv.col, ac::kFieldW, first, 3, flipped);
}

void drawLeft(uint32_t nowMs) {
    ac::Canvas cv;
    if (s_view.wxOk) {
        ac::drawWeather(cv, ac::kindFromWmo(s_view.wmo), s_view.isDay, nowMs / 160);
    } else {                                       // no data yet: a dot runs back and forth
        const int x = (int)((nowMs / 120) % 22);
        cv.set(x < 12 ? x + 6 : 28 - x, 4);
    }
    putCanvas(cv, F_LEFT, false);
}

void tempText(char* out, size_t cap, int16_t t10) {
    snprintf(out, cap, "%d^", (int)lroundf(t10 / 10.0f));
}

void drawMiddle(uint32_t pageIdx) {
    if (!s_view.wxOk) { mtx::field(mtx::CHAIN_DATA, F_MID, 3, true, "--"); return; }
    char pages[4][10];
    int n = 0;
    tempText(pages[n++], sizeof(pages[0]), s_view.temp10);
    if (abs(s_view.feels10 - s_view.temp10) >= 20) {
        char t[8];
        tempText(t, sizeof(t), s_view.feels10);
        snprintf(pages[n++], sizeof(pages[0]), "F%s", t);
    }
    snprintf(pages[n++], sizeof(pages[0]), "%u%%", s_view.popNow);
    snprintf(pages[n++], sizeof(pages[0]), "W%u", s_view.wind);
    mtx::field(mtx::CHAIN_DATA, F_MID, 3, true, pages[pageIdx % n]);
}

enum RightPage { RP_MOON, RP_NEXT, RP_DATE, RP_MONTH, RP_SUN_ICON, RP_SUN_TIME };

uint32_t pageDuration(int p) { return p == RP_SUN_ICON ? SUN_ICON_MS : p == RP_SUN_TIME ? SUN_TIME_MS : PAGE_MS; }

void drawRight(int64_t sec, uint32_t nowMs, uint32_t elapsed, bool blink) {
    if (blink && ((nowMs / 250) & 1)) {            // reminder: the field blinks
        static const uint8_t blank[ac::kFieldW] = {0};
        mtx::place(mtx::CHAIN_DATA, blank, ac::kFieldW, F_RIGHT, 3, false);
        return;
    }
    int pages[6];
    int n = 0;
    pages[n++] = RP_MOON;
    if (s_view.haveNext) pages[n++] = RP_NEXT;
    pages[n++] = RP_DATE;
    pages[n++] = RP_MONTH;
    const int64_t nextSun = (s_view.sunrise && sec < s_view.sunrise) ? s_view.sunrise
                          : (s_view.sunset && sec < s_view.sunset) ? s_view.sunset : 0;
    if (nextSun) { pages[n++] = RP_SUN_ICON; pages[n++] = RP_SUN_TIME; }

    uint32_t total = 0;
    for (int i = 0; i < n; i++) total += pageDuration(pages[i]);
    uint32_t t = elapsed % total;
    int idx = 0;
    while (idx < n - 1 && t >= pageDuration(pages[idx])) { t -= pageDuration(pages[idx]); idx++; }

    const ac::Civil c = ac::fromSec(sec);
    char buf[16];
    switch (pages[idx]) {
        case RP_MOON: {
            const ac::MoonInfo m = ac::moonAt(ac::localToUtc(sec));
            ac::Canvas cv;
            ac::drawMoon(cv, 1, m.age / ac::kSynodicMonth);
            char pct[8];
            snprintf(pct, sizeof(pct), "%d", (int)lroundf(m.illum * 100.0));
            uint8_t cols[16];
            mtx::renderText(pct, cols, sizeof(cols), 0);
            const int w = (int)strlen(pct) * 6 - 1;
            for (int k = 0; k < w && 11 + k < ac::kFieldW; k++) cv.col[11 + k] = cols[k];
            putCanvas(cv, F_RIGHT, false);
            break;
        }
        case RP_NEXT: {
            if (s_view.nextOngoing) {
                snprintf(buf, sizeof(buf), "NOW");
            } else {
                const int64_t delta = s_view.next.start - sec;
                if (delta < 3600) snprintf(buf, sizeof(buf), "%dM", (int)((delta + 59) / 60));
                else {
                    const ac::Civil t2 = ac::fromSec(s_view.next.start);
                    snprintf(buf, sizeof(buf), "%02d%02d", t2.hour, t2.minute);
                }
            }
            mtx::field(mtx::CHAIN_DATA, F_RIGHT, 3, false, buf);
            break;
        }
        case RP_DATE:
            snprintf(buf, sizeof(buf), "%.2s%d", ac::weekdayName(ac::weekday(ac::daysOf(sec)), true), c.day);
            mtx::field(mtx::CHAIN_DATA, F_RIGHT, 3, false, buf);
            break;
        case RP_MONTH:
            snprintf(buf, sizeof(buf), "%s", ac::monthName(c.month, true));
            mtx::field(mtx::CHAIN_DATA, F_RIGHT, 3, false, buf);
            break;
        case RP_SUN_ICON: {
            ac::Canvas cv;
            ac::drawSunEvent(cv, nextSun == s_view.sunrise, t / 200);
            putCanvas(cv, F_RIGHT, false);
            break;
        }
        case RP_SUN_TIME: {
            const ac::Civil s = ac::fromSec(nextSun);
            snprintf(buf, sizeof(buf), "%02d%02d", s.hour, s.minute);
            mtx::field(mtx::CHAIN_DATA, F_RIGHT, 3, false, buf);
            break;
        }
    }
}

// ── sparkle ──────────────────────────────────────────────────────────────

void drawSparkle(uint32_t remainingMs, uint32_t totalMs) {
    // random pixels, fewer and fewer (out of 1024 per pixel)
    const uint32_t density = 30 + (remainingMs * 200) / (totalMs ? totalMs : 1);
    uint8_t cols[72];
    for (int chain = 0; chain < 2; chain++) {
        const int width = chain == 0 ? 56 : 72;
        for (int x = 0; x < width; x++) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++)
                if ((esp_random() % 1024) < density) b |= (uint8_t)(1 << bit);
            cols[x] = b;
        }
        if (chain == 0) {
            mtx::place(mtx::CHAIN_TC, cols, 56, 0, 7, false);
        } else {
            mtx::place(mtx::CHAIN_DATA, cols, 24, F_LEFT, 3, false);
            mtx::place(mtx::CHAIN_DATA, cols + 24, 24, F_MID, 3, true);
            mtx::place(mtx::CHAIN_DATA, cols + 48, 24, F_RIGHT, 3, false);
        }
    }
}

// ── renderer ─────────────────────────────────────────────────────────────

void render(uint32_t nowMs, bool redraw, bool tcOwned, mtx::Result& out) {
    static int64_t lastSec = -1;
    static uint32_t lastLeftFrame = ~0u, lastRightTick = ~0u, lastSparkle = 0;
    static bool sparkling = false;
    static const uint32_t pageBase = millis();

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
    if (s_scrollRequest) {
        s_scrollRequest = false;
        startScroll();
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
        lastLeftFrame = lastRightTick = ~0u;
    }

    // TC chain: clock, or a scrolling message
    if (!tcOwned) {
        if (s_scrollPos >= 0) {
            if ((int32_t)(nowMs - s_scrollNextMs) >= 0) {
                s_scrollNextMs = nowMs + 55;
                mtx::place(mtx::CHAIN_TC, s_scrollCols + s_scrollPos, 56, 0, 7, false);
                out.tcDirty = true;
                if (++s_scrollPos > s_scrollWidth - 56) {
                    s_scrollPos = -1;
                    redraw = true;                      // clock again right away
                }
            }
        }
        if (s_scrollPos < 0 && (redraw || secChanged)) {
            drawClock(sec);
            out.tcDirty = true;
        }
    }

    // data chain: weather animation at about 6 fps, info pages re-evaluated at 8 Hz
    const uint32_t leftFrame = nowMs / 160;
    const uint32_t rightTick = nowMs / 125;
    if (redraw || secChanged || leftFrame != lastLeftFrame || rightTick != lastRightTick) {
        lastLeftFrame = leftFrame;
        lastRightTick = rightTick;
        const uint32_t elapsed = nowMs - pageBase + s_pageSkipMs;
        drawLeft(nowMs);
        drawMiddle(elapsed / PAGE_MS);
        drawRight(sec, nowMs, elapsed, (int32_t)(s_blinkUntil - nowMs) > 0);
        out.dataDirty = true;
    }
}

}  // namespace

namespace clockmatrix {

void begin() { mtx::setRenderer(render); }

void scroll(const String& text) {
    portENTER_CRITICAL(&s_scrollMux);
    strlcpy(s_scrollText, text.c_str(), sizeof(s_scrollText));
    portEXIT_CRITICAL(&s_scrollMux);
    s_scrollRequest = true;
}

void sparkle(uint32_t durationMs) { s_sparkleUntil = millis() + durationMs; }

void nextPage() { s_pageSkipMs += PAGE_MS; }

void adjustBrightness(int delta) {
    ClockConfig& c = clockcfg::get();
    uint8_t& b = s_night ? c.brightnessNight : c.brightnessDay;
    const int v = constrain((int)b + delta, 0, 15);
    b = (uint8_t)v;
    mtx::setBrightness(b);
    mtx::showOverlay(String("BRT ") + v, 900);
    s_brightnessSaveDue = true;
    s_brightnessSaveAt = millis() + 2000;
}

bool isNight() { return s_night; }

}  // namespace clockmatrix
