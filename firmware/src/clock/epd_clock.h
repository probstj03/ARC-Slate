#pragma once
// ARC-Clock e-ink: a static daily overview (date, moon, weather with sunrise/sunset, calendar,
// to-do list). No clock face: the time is shown on the LED matrices.
//
// Refresh policy: one full refresh per day at the configured time (default 05:30), plus after
// a power loss if today's refresh is missing, and on request. Never while the lid is open
// (unless forced), because a full refresh flashes black and white.
#include <Arduino.h>

namespace epdclock {

void begin();                       // loads the last-refresh day from NVS
void startTask();

// Refresh as soon as possible (waits for fresh data first). `force`: even with the lid open.
void request(bool force = false);

int64_t lastRefreshDay();           // days since 1970 (local), 0 = never
String statusLine();                // for the serial status

}  // namespace epdclock
