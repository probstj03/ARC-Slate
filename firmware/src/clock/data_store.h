#pragma once
// Calendar, to-do list and weather: shared store + the background task that keeps it fresh.
//
// The fetch task runs one request at a time (TLS needs a lot of RAM) and only while a real
// WiFi network (not ARC-Net) is connected and the clock is plausible (certificate validity).
// The last good data is cached on LittleFS and shown immediately after a reboot.
#include <Arduino.h>
#include "core/data_model.h"

namespace clockdata {

void begin();                // loads the cache
void startTask();

// Access under the lock; keep it short (copy what you need).
void lock();
void unlock();
const Data& data();

void requestAll();           // fetch everything as soon as possible
void request(Source s);
// Waits until every configured source was tried since the last requestAll() (or the timeout).
bool waitForFresh(uint32_t timeoutMs);

// Fetches one source right now in the calling task and prints what it got (serial command).
void fetchNowVerbose(Source s, Print& out);

const char* sourceName(Source s);

// Automatic place (config locAuto): looked up once per start from the public IP before the first weather fetch.
// relocate() makes the next weather fetch look it up again.
void relocate();

}  // namespace clockdata
