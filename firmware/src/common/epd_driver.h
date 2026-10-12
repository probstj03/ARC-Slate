#pragma once
// 7.5" e-ink (GDEW075T7, 800x480, black/white) on the shared SPI bus.
// Common to ARC-Slate and ARC-Clock: display object, SPI bus release while the panel is busy
// (the LED matrices keep running), small text helpers. The caller holds spibus::lock()
// around every drawing + display() sequence.
#include <Arduino.h>
#include <GxEPD2_BW.h>

namespace epddrv {

constexpr int W = 800, H = 480;

using Display = GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>;
Display& display();

// Initialises the panel (takes the SPI lock itself). Does not draw anything.
void init();

void printAt(int x, int y, const GFXfont* font, const String& text);
void printCentered(int cx, int y, const GFXfont* font, const String& text);
void printRight(int xRight, int y, const GFXfont* font, const String& text);
int textWidth(const GFXfont* font, const String& text);

}  // namespace epddrv
