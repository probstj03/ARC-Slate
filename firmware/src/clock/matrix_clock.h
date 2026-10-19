#pragma once
// What ARC-Clock shows on the LED matrices while the lid is open: only current things, nothing that
// alternates or blinks. Everything static (date, moon, sunrise/sunset, forecast, calendar, to-do) is on the e-ink.
//   TC chain (56 columns)  : HH:MM:SS, centred
//   Data chain (3x3 modules = 24x24): animated weather icon in the top two rows (assets/weather_icons_24x16.h),
//                            the current temperature (12°C) in the bottom row; reminders scroll over the TC chain
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
