#pragma once
// 7.5" e-ink (GDEW075T7, 800x480, black/white) on the shared SPI bus.
// Common to ARC-Slate and ARC-Clock: display object, SPI bus release while the panel is busy
// (the LED matrices keep running), small text helpers, and refreshes that survive a weak supply.
// The caller holds spibus::lock() around every drawing + refresh sequence.
//
// Why the refresh helpers: every refresh starts with the panel's booster (PowerOn, ~140 ms current peak).
// The LED matrices draw pulsed current from the same 3V3 regulator. If the rail dips during PowerOn the booster
// gives up after ~2 s, nothing is refreshed and the panel stays in that state. refreshFull()/refreshWindow()
// therefore switch the matrices off around PowerOn and check that the panel really refreshed. On a failure the
// panel is put to sleep and refreshes are suspended, so the picture already shown is not washed out.
#include <Arduino.h>
#include <GxEPD2_BW.h>

namespace epddrv {

constexpr int W = 800, H = 480;

using Display = GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>;
Display& display();

// Initialises the panel (takes the SPI lock itself). Does not draw anything.
void init();

// Matrix pause around e-ink refreshes.
//   Off      matrices stay on (needs a strong supply)
//   PowerOn  matrices off for ~0.3 s while the booster starts (default, hardly visible)
//   Full     matrices off for the whole refresh (2-6 s)
enum class Pause : uint8_t { Off = 0, PowerOn = 1, Full = 2 };
void setPause(Pause mode);
Pause pause();
const char* pauseName(Pause mode);

// Refresh the frame buffer (caller holds the SPI lock). Returns false if the panel did not refresh; then
// further refreshes are suspended (the last picture stays) until resume() or a restart.
bool refreshFull();
bool refreshWindow(int x, int y, int w, int h);

struct Stats {
    uint32_t refreshes = 0;
    uint32_t failures = 0;    // refreshes where the booster did not start
    uint32_t lastMs = 0;      // duration of the last refresh call
};
Stats stats();
void resume();
bool suspended();

// Switches the LED matrices off while it lives (and back on afterwards if they were on).
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
