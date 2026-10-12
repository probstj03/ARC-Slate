#pragma once
// Layout of the ARC-Clock e-ink overview, drawn onto any Adafruit_GFX surface (GxEPD2 on the
// device, a bitmap in the host preview: test_host/render_epd.cpp).
#include <Adafruit_GFX.h>
#include <stdint.h>
#include "../core/data_model.h"

namespace epdlayout {

constexpr int W = 800, H = 480;

struct Options {
    const char* place = "";                // shown next to the wind
    const char* version = "ARC-Clock";     // bottom right
};

// Draws the whole screen (background included). Black = 0x0000, white = 0xFFFF.
void draw(Adafruit_GFX& g, const clockdata::Data& snap, int64_t now, const Options& opt);

}  // namespace epdlayout
