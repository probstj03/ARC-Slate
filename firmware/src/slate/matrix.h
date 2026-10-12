#pragma once
// MAX7219 LED matrices on the shared hardware SPI bus.
//   Chain TC   : 7 modules, LOAD = GPIO7  – timecode / short texts
//   Chain DATA : 9 modules, LOAD = GPIO14 – roll / shot / scene + take (3x3)
// Module 0 of a chain is the one closest to the board (data input).
// No LedControl: its 8-modules-per-chain limit does not apply here.
#include <Arduino.h>

namespace mtx {

enum Chain { CHAIN_TC = 0, CHAIN_DATA = 1 };

void begin();
void startTask();
void setBrightness(uint8_t level);   // 0..15

// Temporarily replaces the timecode with a text (e.g. menu value), 0 = until cleared.
void showOverlay(const String& text, uint32_t durationMs);
void clearOverlay();

// Hardware test: 0 = normal, 1 = all LEDs on, 2 = module numbers.
void setTestMode(uint8_t mode);

}  // namespace mtx
