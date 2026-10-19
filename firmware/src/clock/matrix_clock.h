#pragma once
// What ARC-Clock shows on the LED matrices while the lid is open: only current things, nothing that
// alternates or blinks. Everything static (date, moon, sunrise/sunset, forecast, calendar, to-do) is on the e-ink.
//   TC chain (56 columns)  : HH:MM:SS on the left, current temperature on the right
//   Data chain (72 columns): one weather animation over all 9 modules; reminders and messages scroll here
// Brightness follows the sun (day / night value with a 30 min transition, see common/daylight.h).
#include <Arduino.h>

namespace clockmatrix {

void begin();                              // registers the renderer
void scroll(const String& text);           // scrolls a message over the data chain
void sparkle(uint32_t durationMs);         // short sparkle effect on all modules
void showNext();                           // button: scrolls the next event of today
void adjustBrightness(int delta);          // encoder: changes the day or night brightness
bool isNight();

}  // namespace clockmatrix
