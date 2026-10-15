#pragma once
// 7.5" e-ink (GDEW075T7, 800x480, black/white) on the shared SPI bus.
// Common to ARC-Slate and ARC-Clock: display object, SPI bus release while the panel is busy
// (the LED matrices keep running), small text helpers. The caller holds spibus::lock()
// around every drawing + display() sequence.
#include <Arduino.h>
#include <GxEPD2_BW.h>

namespace epddrv {

// Switches the LED matrices off while it lives (and back on afterwards if they were on). A full e-ink
// refresh draws a current peak from the shared 5V/3V3 supply; with the matrices lit at the same time the
// supply dips and the panel receives garbled data. Create it BEFORE taking the SPI lock: the matrix task
// needs the bus to send the shutdown command.
class MatrixPause {
public:
    explicit MatrixPause(bool active = true);   // active = false does nothing
    ~MatrixPause();

private:
    bool _was;
};

constexpr int W = 800, H = 480;

using Display = GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>;
Display& display();

// SPI clock of the panel (default 2 MHz: the matrices share MOSI/SCK and load the lines).
void setSpiHz(uint32_t hz);
uint32_t spiHz();

// Initialises the panel (takes the SPI lock itself). Does not draw anything.
void init();

void printAt(int x, int y, const GFXfont* font, const String& text);
void printCentered(int cx, int y, const GFXfont* font, const String& text);
void printRight(int xRight, int y, const GFXfont* font, const String& text);
int textWidth(const GFXfont* font, const String& text);

}  // namespace epddrv
