#pragma once
// 7.5" e-ink (GDEW075T7, 800x480, black/white) on the shared SPI bus.
// Common to ARC-Slate and ARC-Clock: display object, SPI bus release while the panel is busy
// (the LED matrices keep running), small text helpers, and refreshes that check the panel's booster.
// The caller holds spibus::lock() around every drawing + refresh sequence.
//
// Every refresh starts with the panel's booster (PowerOn, ~140 ms current peak). On a weak 3V3 supply it gives up
// after ~2 s and nothing is refreshed; refreshFull()/refreshWindow() detect that, count it and log it.
#include <Arduino.h>
#include <GxEPD2_BW.h>

namespace epddrv {

constexpr int W = 800, H = 480;

using Display = GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>;
Display& display();

// Initialises the panel (takes the SPI lock itself). Does not draw anything.
void init();

// Refresh the frame buffer (caller holds the SPI lock). Returns false if the panel did not refresh.
bool refreshFull();
bool refreshWindow(int x, int y, int w, int h);

struct Stats {
    uint32_t refreshes = 0;
    uint32_t failures = 0;    // refreshes where the booster did not start
    uint32_t lastMs = 0;      // duration of the last refresh call
};
Stats stats();

// Switches the LED matrices off while it lives (and back on afterwards if they were on). Diagnostics only.
// Create it BEFORE taking the SPI lock: the matrix task needs the bus to send the shutdown command.
class MatrixPause {
public:
    explicit MatrixPause(bool active = true);   // active = false does nothing
    ~MatrixPause();

private:
    bool _was;
};

// SPI clock of the panel (default 2 MHz: the matrices share MOSI/SCK and load the lines).
void setSpiHz(uint32_t hz);
uint32_t spiHz();

void printAt(int x, int y, const GFXfont* font, const String& text);
void printCentered(int cx, int y, const GFXfont* font, const String& text);
void printRight(int xRight, int y, const GFXfont* font, const String& text);
int textWidth(const GFXfont* font, const String& text);

}  // namespace epddrv
