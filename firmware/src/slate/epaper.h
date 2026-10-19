#pragma once
// 7.5" e-ink (GDEW075T7, 800x480) on the shared SPI bus.
// Layout: left = production data, right = QR code, bottom = status bar.
// The status bar uses partial refresh (~0.5 s); the slate card uses a full refresh (~4 s).
// The menu is shown on the LED matrices (ui.cpp), not here. While the panel is busy the SPI bus
// is released, so the LED timecode keeps running.
#include <Arduino.h>

namespace epd {

void begin();          // init + boot screen
void startTask();

void requestCard();    // full redraw of the slate card
void requestStatus();  // partial redraw of the status bar
void setPartial(bool on);   // partial refresh for the status bar
bool partialEnabled();
void probePins();         // serial: epdpins (BUSY / RST check)
void requestLab();           // serial: epdlab (same picture, different SPI clocks / matrices on/off)
void requestTest();    // hardware test picture (serial: epdtest)

void setStatusProvider(String (*provider)());

}  // namespace epd
