#pragma once
// MAX7219 LED matrices on the shared hardware SPI bus (driver + display task).
//   Chain TC   : 7 modules, LOAD = GPIO7  – timecode / clock / short texts (56 columns)
//   Chain DATA : 9 modules, LOAD = GPIO14 – 3 fields of 3 modules (24 columns each)
// Module 0 of a chain is the one closest to the board (data input).
// No LedControl: its 8-modules-per-chain limit does not apply here.
//
// The firmware decides what is shown by registering a renderer (ARC-Slate: timecode and
// shot data, ARC-Clock: time and weather). A column byte has bit 7 = top row, the 5x7 font
// uses bits 6..0 (the top row stays free).
#include <Arduino.h>

namespace mtx {

enum Chain { CHAIN_TC = 0, CHAIN_DATA = 1 };

struct Result {
    bool tcDirty = false;     // the renderer changed the TC chain
    bool dataDirty = false;   // ... the data chain
};

// Called every ~8 ms from the matrix task (not while a test pattern is shown).
//   redraw      true after start, after an overlay or test pattern, and after re-enabling
//   tcOwned     true while an overlay owns the TC chain: leave that chain alone
using Renderer = void (*)(uint32_t nowMs, bool redraw, bool tcOwned, Result& out);

void begin(uint8_t brightness);
void startTask();
void setRenderer(Renderer renderer);
void setBrightness(uint8_t level);   // 0..15
void setEnabled(bool on);            // false: modules in shutdown mode (dark, ~µA)
bool enabled();

// Temporarily replaces the TC chain with a text (e.g. menu value), 0 = until cleared.
// Texts wider than the chain (56 columns) scroll from right to left.
void showOverlay(const String& text, uint32_t durationMs);
void clearOverlay();

// Hardware test: 0 = normal, 1 = all LEDs on, 2 = module numbers.
void setTestMode(uint8_t mode);

// ── drawing helpers for renderers ─────────────────────────────────────────
// Renders text into `width` display columns (left to right), returns the used width.
int renderText(const char* text, uint8_t* cols, int width, int padLeft, bool gapAfterSecond = false);
// Puts `width` columns onto `count` modules starting at `first`. A "flipped" row is mounted
// rotated by 180° and the chain runs backwards (serpentine).
void place(Chain chain, const uint8_t* cols, int width, int first, int count, bool flipped);
// Text centred in a field of `count` modules.
void field(Chain chain, int first, int count, bool flipped, const char* text, bool gapAfterSecond = false);
void clear(Chain chain);

}  // namespace mtx
