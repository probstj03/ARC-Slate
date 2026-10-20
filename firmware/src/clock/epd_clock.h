#pragma once
// ARC-Clock e-ink: the static overview (date, moon, weather forecast of the day, calendar, to-do list).
// Everything current (time, current weather, reminders) is on the LED matrices.
//
// Refresh policy (as rarely as possible):
//  - every 15 minutes the picture is drawn invisibly and hashed; only if it differs from the picture on the
//    panel (new event, task done, new day ...) the panel is refreshed;
//  - the weather on the panel is the forecast of the day (high / low, rain, symbol, sun times); when it changes
//    (every weather fetch, 30 min) only the weather zone is refreshed partially;
//  - a small change (one or two zones: header, weather, calendar, dashboard) is refreshed partially, only in that area;
//    the daily refresh, the first one after a start, big changes and every 12th one are full refreshes (ghosting);
//  - once after every start (the slate firmware may have drawn the panel) and on request;
//  - with the lid open a change waits until it closes (at most 2 h);
//  - night rest: from 22:00 until the daily refresh nothing is redrawn unless there are events at night.
#include <Arduino.h>

namespace epdclock {

void begin();                       // loads the state from NVS
void startTask();

// Looks for changes now instead of at the next quarter hour (serial `epdcheck`); draws only if something changed.
void checkNow();

// Partial refresh of the dashboard area (serial `epdpart`), to check that path.
void requestPartialTest();

// Refresh as soon as possible (with fresh data), even with the lid open.
void request(bool force = false);
// The lid was closed: a change that waited for it is drawn now.
void lidClosed();

int64_t lastRefreshDay();           // days since 1970 (local) of the last daily refresh, 0 = never
String statusLine();                // for the serial status

}  // namespace epdclock
