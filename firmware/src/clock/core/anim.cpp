#include "anim.h"
#include <math.h>
#include <string.h>

namespace ac {

namespace {

constexpr double kPi = 3.14159265358979323846;

const char* const kCloud[4] = {
    "..XX.XXX...",
    ".XXXXXXXXX.",
    "XXXXXXXXXXX",
    ".XXXXXXXXX.",
};
constexpr int kCloudW = 11, kCloudH = 4;

// Cloud with its left edge at x (may be negative / beyond the field: clipped).
void cloud(Canvas& c, int x, int y, bool clearBelow = false) {
    for (int r = 0; r < kCloudH; r++)
        for (int k = 0; k < kCloudW; k++)
            if (kCloud[r][k] == 'X') c.set(x + k, y + r);
    (void)clearBelow;
}

// Sun: 4x4 disc with its top-left corner at (ox, oy), single-pixel rays that alternate between
// the "+" and the "x" pattern.
void sun(Canvas& c, int ox, int oy, uint32_t frame, bool rays = true) {
    static const char* const disc[4] = {".XX.", "XXXX", "XXXX", ".XX."};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            if (disc[r][k] == 'X') c.set(ox + k, oy + r);
    if (!rays) return;
    if (frame & 1) {
        c.set(ox - 2, oy - 2); c.set(ox + 5, oy - 2); c.set(ox - 2, oy + 5); c.set(ox + 5, oy + 5);
        c.set(ox - 1, oy - 1); c.set(ox + 4, oy - 1); c.set(ox - 1, oy + 4); c.set(ox + 4, oy + 4);
    } else {
        c.set(ox + 1, oy - 2); c.set(ox + 2, oy - 2); c.set(ox + 1, oy + 5); c.set(ox + 2, oy + 5);
        c.set(ox - 2, oy + 1); c.set(ox - 2, oy + 2); c.set(ox + 5, oy + 1); c.set(ox + 5, oy + 2);
    }
}

void crescent(Canvas& c, int ox, uint32_t frame) {
    // 8x8 moon (waxing crescent), stars twinkle around it
    drawMoon(c, ox, 0.18);
    static const int stars[][2] = {{2, 1}, {20, 2}, {4, 6}, {18, 6}, {22, 4}};
    for (int i = 0; i < 5; i++)
        if (((frame / 3) + i * 2) % 5 != 0) c.set(stars[i][0], stars[i][1]);
}

void drops(Canvas& c, int count, int slow, uint32_t frame, bool heavy) {
    static const int xs[] = {6, 9, 12, 15, 8, 11, 14, 17};
    for (int i = 0; i < count && i < 8; i++) {
        const int y = 4 + (int)(((frame / slow) + i * 3) % 4);
        c.set(xs[i], y);
        if (heavy && y > 4) c.set(xs[i], y - 1);
    }
}

void flakes(Canvas& c, uint32_t frame) {
    static const int xs[] = {7, 10, 13, 16, 8, 14};
    for (int i = 0; i < 6; i++) {
        const int y = 4 + (int)(((frame / 2) + i * 2) % 4);
        const int x = xs[i] + (int)(((frame / 2) + i) & 1);
        c.set(x, y);
    }
}

}  // namespace

void drawMoon(Canvas& c, int x0, double phaseFrac) {
    const double cx = 3.5, cy = 3.5, r = 3.5;
    const double k = cos(2.0 * kPi * phaseFrac);          // 1 new, -1 full
    const bool waxing = phaseFrac < 0.5;
    for (int y = 0; y < 8; y++) {
        const double dy = y - cy;
        const double hw2 = r * r - dy * dy;
        if (hw2 < 0) continue;
        const double hw = sqrt(hw2);
        for (int x = 0; x < 8; x++) {
            const double dx = x - cx;
            const double dist = sqrt(dx * dx + dy * dy);
            if (dist > r + 0.3) continue;
            const bool lit = waxing ? (dx > k * hw - 0.01) : (dx < -k * hw + 0.01);
            const bool edge = dist > r - 0.7;
            if (lit && dist <= r + 0.3) c.set(x0 + x, y);
            else if (edge) { if (((x + y) & 1) == 0 || dist > r - 0.4) c.set(x0 + x, y); }   // dim outline of the dark part
        }
    }
}

void drawSunEvent(Canvas& c, bool rise, uint32_t t) {
    const int steps = 8;
    const int tt = (int)(t > (uint32_t)steps ? steps : t);
    const int pos = rise ? tt : steps - tt;                 // 0 below the horizon .. steps high above
    const int top = 6 - (pos * 5) / steps;                  // top row of the sun disc: 6 (hidden) .. 1
    static const char* const disc[4] = {".XX.", "XXXX", "XXXX", ".XX."};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            if (disc[r][k] == 'X' && top + r < 6) c.set(10 + k, top + r);
    for (int x = 3; x < 21; x++) c.set(x, 6);               // horizon
    // arrow: up for sunrise, down for sunset
    if (rise) { c.set(21, 3); c.set(20, 4); c.set(22, 4); c.set(21, 4); c.set(21, 5); }
    else { c.set(21, 5); c.set(20, 4); c.set(22, 4); c.set(21, 4); c.set(21, 3); }
    c.set(2, 7); c.set(21, 7);
}

void drawWeather(Canvas& c, WxKind kind, bool day, uint32_t frame) {
    c.clear();
    switch (kind) {
        case WxKind::Clear:
            if (day) sun(c, 10, 2, frame);
            else crescent(c, 8, frame);
            break;
        case WxKind::PartlyCloudy: {
            if (day) sun(c, 2, 2, frame); else drawMoon(c, 1, 0.18);
            const int drift = (int)((frame / 4) % 6) - 3;               // -3 .. 2, back and forth by wrapping
            cloud(c, 10 + (drift < 0 ? -drift : drift), 3);
            break;
        }
        case WxKind::Cloudy: {
            // Two clouds, one drifting right and one left, half a period apart so the sky is never empty.
            const int period = kFieldW + kCloudW;
            const int a = (int)((frame / 2) % period);
            cloud(c, a - kCloudW + 1, 0);                               // moves right
            const int b = (int)(((frame / 3) + period / 2) % period);
            cloud(c, kFieldW - b, 4);                                   // moves left
            break;
        }
        case WxKind::Fog:
            for (int r = 1; r < 8; r += 2) {
                const int shift = (int)((frame / 2) % 6);
                const int s = (r % 4 == 1) ? shift : 5 - shift;
                for (int x = 0; x < kFieldW; x++)
                    if (((x + s) % 6) < 4) c.set(x, r);
            }
            break;
        case WxKind::Drizzle:
            cloud(c, 6, 0);
            drops(c, 3, 2, frame, false);
            break;
        case WxKind::Rain:
            cloud(c, 6, 0);
            drops(c, 8, 1, frame, true);
            break;
        case WxKind::Showers:
            cloud(c, 6 + (int)((frame / 6) % 2), 0);
            drops(c, 6, 1, frame, false);
            break;
        case WxKind::Snow:
            cloud(c, 6, 0);
            flakes(c, frame);
            break;
        case WxKind::Thunder: {
            cloud(c, 6, 0);
            const uint32_t f = frame % 16;
            if (f < 6) { c.set(13, 4); c.set(12, 5); c.set(13, 5); c.set(11, 6); c.set(12, 6); c.set(11, 7); }
            if (f == 0) c.fill();                                       // flash
            break;
        }
        default:
            c.set(11, 1); c.set(12, 1); c.set(13, 2); c.set(13, 3); c.set(12, 4); c.set(12, 6);
            break;
    }
}

void dump(const Canvas& c, char* out, int cap) {
    int n = 0;
    for (int y = 0; y < kFieldH && n + kFieldW + 2 < cap; y++) {
        for (int x = 0; x < kFieldW; x++) out[n++] = c.get(x, y) ? '#' : '.';
        out[n++] = '\n';
    }
    out[n] = 0;
}

}  // namespace ac
