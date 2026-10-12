#pragma once
// What ARC-Clock shows on the LED matrices while the lid is open.
//   TC chain (56 columns)   : HH:MM:SS with a seconds bar in the top row; scrolling text for reminders
//   Data chain, left field  : weather animation
//   Data chain, middle field: temperature, feels-like, rain probability, wind (rotating)
//   Data chain, right field : moon, next event, date, sunrise/sunset (rotating)
#include <Arduino.h>

namespace clockmatrix {

void begin();                              // registers the renderer
void scroll(const String& text);           // scrolls a message over the TC chain
void sparkle(uint32_t durationMs);         // short sparkle effect on all modules
void nextPage();                           // skip to the next info page (button)
void adjustBrightness(int delta);          // encoder: changes the day or night brightness
bool isNight();

}  // namespace clockmatrix
