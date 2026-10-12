#include "matrix_slate.h"
#include <ArcCore.h>
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

void render(uint32_t, bool redraw, bool tcOwned, mtx::Result& out) {
    static uint32_t lastRev = 0;
    static arc::Timecode lastTc;
    bool dataDirty = redraw;
    const uint32_t rev = state::revision();
    if (rev != lastRev) { lastRev = rev; dataDirty = true; }

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

}  // namespace slatematrix
