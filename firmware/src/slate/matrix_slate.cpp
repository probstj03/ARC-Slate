#include "matrix_slate.h"
#include <ArcCore.h>
#include "daylight.h"
#include "matrix.h"
#include "state.h"

namespace {

// Data chain layout: which modules show which field. A "flipped" row is mounted
// rotated by 180° with the chain running backwards (serpentine).
struct Field { int firstModule; int count; bool flipped; };
constexpr Field FIELD_ROLL       = { 0, 3, false };
constexpr Field FIELD_SHOT       = { 3, 3, true };
constexpr Field FIELD_SCENE_TAKE = { 6, 3, false };

void drawField(const Field& f, const char* text, bool gapAfterSecond = false) {
    mtx::field(mtx::CHAIN_DATA, f.firstModule, f.count, f.flipped, text, gapAfterSecond);
}

void renderData() {
    state::lock();
    const ShotInfo& s = state::shot();
    char roll[8], shot[8], sceneTake[8];
    snprintf(roll, sizeof(roll), "%c%03d", s.cam, s.rollNum);
    snprintf(shot, sizeof(shot), "%s", s.shot.c_str());
    snprintf(sceneTake, sizeof(sceneTake), "%2d%2d", s.scene % 100, s.take % 100);
    state::unlock();
    drawField(FIELD_ROLL, roll);
    drawField(FIELD_SHOT, shot);
    drawField(FIELD_SCENE_TAKE, sceneTake, true);
}

void renderTimecode(const arc::Timecode& tc) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u:%02u", tc.h, tc.m, tc.s, tc.f);
    uint8_t cols[56];
    mtx::renderText(buf, cols, 56, 2);   // 2 + 8x(5+1) + 3x(1+1) = 56 columns
    mtx::place(mtx::CHAIN_TC, cols, 56, 0, 7, false);
}

// Brightness follows the sun: day / night value from the settings, 30 min transition (common/daylight.h).
bool s_night = false;

void applyBrightness() {
    static int lastApplied = -1;
    static int sunDay = -1, rise = 7 * 60, set = 19 * 60;
    static double lat = 0, lon = 0;
    const DateTime now = arc::clock::now();                   // local time
    if (now.year() < 2024) return;
    if (sunDay != now.day()) {                                // once a day (and at start)
        sunDay = now.day();
        daylight::place(lat, lon);
        const int offset = daylight::cetOffsetMin(now.year(), now.month(), now.day(), now.hour());
        if (!daylight::sunTimes(now.year(), now.month(), now.day(), lat, lon, offset, rise, set)) { rise = 7 * 60; set = 19 * 60; }
    }
    const int minute = now.hour() * 60 + now.minute();
    s_night = minute < rise || minute >= set;
    state::lock();
    const uint8_t day = state::settings().brightness, night = state::settings().brightnessNight;
    state::unlock();
    const int b = daylight::level(minute, rise, set, day, night);
    if (b != lastApplied) {
        lastApplied = b;
        mtx::setBrightness((uint8_t)b);
    }
}

void render(uint32_t nowMs, bool redraw, bool tcOwned, mtx::Result& out) {
    static uint32_t lastRev = 0;
    static uint32_t lastBrightMs = 0;
    static arc::Timecode lastTc;
    bool dataDirty = redraw;
    const uint32_t rev = state::revision();
    if (rev != lastRev) { lastRev = rev; dataDirty = true; lastBrightMs = nowMs - 1000; }
    if (nowMs - lastBrightMs >= 1000) {
        lastBrightMs = nowMs;
        applyBrightness();
    }

    if (!tcOwned) {
        const arc::Timecode tc = arc::engine::current();
        if (redraw || tc.f != lastTc.f || tc.s != lastTc.s || tc.m != lastTc.m || tc.h != lastTc.h) {
            lastTc = tc;
            renderTimecode(tc);
            out.tcDirty = true;
        }
    }
    if (dataDirty) {
        renderData();
        out.dataDirty = true;
    }
}

}  // namespace

namespace slatematrix {

void begin() { mtx::setRenderer(render); }

bool isNight() { return s_night; }

}  // namespace slatematrix
