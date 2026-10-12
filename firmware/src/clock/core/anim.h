#pragma once
// Pixel animations for one 24x8 field of the LED matrix (3 MAX7219 modules). Pure, host-testable.
// Column bytes: bit 7 = top row, bit 0 = bottom row (the layout the matrix driver expects).
#include <stdint.h>
#include "weather.h"

namespace ac {

constexpr int kFieldW = 24, kFieldH = 8;

struct Canvas {
    uint8_t col[kFieldW] = {0};

    void clear() { for (auto& c : col) c = 0; }
    void set(int x, int y) { if (x >= 0 && x < kFieldW && y >= 0 && y < kFieldH) col[x] |= (uint8_t)(0x80 >> y); }
    void unset(int x, int y) { if (x >= 0 && x < kFieldW && y >= 0 && y < kFieldH) col[x] &= (uint8_t)~(0x80 >> y); }
    bool get(int x, int y) const { return x >= 0 && x < kFieldW && y >= 0 && y < kFieldH && (col[x] & (0x80 >> y)); }
    void fill() { for (auto& c : col) c = 0xFF; }
};

// Weather: `frame` counts at about 6 per second. `day` picks sun or moon for clear skies.
void drawWeather(Canvas& c, WxKind kind, bool day, uint32_t frame);

// 8x8 moon disc at column x0; `phaseFrac` = age / synodic month (0 new, 0.5 full).
void drawMoon(Canvas& c, int x0, double phaseFrac);

// Sunrise (rise = true) or sunset icon; `t` = frames since the icon appeared (animates once).
void drawSunEvent(Canvas& c, bool rise, uint32_t t);

// Plain-text dump for tests / debugging: '#' lit, '.' dark, 8 lines.
void dump(const Canvas& c, char* out, int cap);

}  // namespace ac
