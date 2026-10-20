#pragma once
// ARC-Clock e-ink: the static overview (date, moon, weather forecast of the day, calendar, to-do list).
// Everything current (time, current weather, reminders) is on the LED matrices.
//
// Refresh policy (as rarely as possible):
//  - every 15 minutes the picture is drawn invisibly and hashed; only if it differs from the picture on the
//    panel (new event, task done, new day ...) the panel is refreshed;
//  - the weather on the panel is the forecast of the daily refresh (default 05:30) and does not change during
//    the day, so weather updates never trigger a refresh;
//  - once after every start (the slate firmware may have drawn the panel) and on request;
//  - with the lid open a change waits until it closes (at most 2 h);
//  - night rest: from 22:00 until the daily refresh nothing is redrawn unless there are events at night.
#include <Arduino.h>

namespace epdclock {

void begin();                       // loads the state from NVS
void startTask();

// Refresh as soon as possible (with fresh data), even with the lid open.
void request(bool force = false);
// The lid was closed: a change that waited for it is drawn now.
void lidClosed();

int64_t lastRefreshDay();           // days since 1970 (local) of the last daily refresh, 0 = never
String statusLine();                // for the serial status

}  // namespace epdclock
