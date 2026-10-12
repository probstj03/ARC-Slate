#pragma once
// Clap detection (hall sensor on GPIO2) and sync flash (LD06AJSA on GPIO15).
//
// Closing: the ISR switches the flash on immediately (no task latency) and
// timestamps the edge with esp_timer. The event task converts it into RTC
// time + timecode + position inside the frame, sends it to ARC-Studio and
// appends it to /events.jsonl. Opening: take + 1.
#include <Arduino.h>

namespace clap {

void begin();
void simulate();                 // serial test: close + open
void testFlash();                // flash for one frame
bool closed();
uint32_t eventCount();

}  // namespace clap
