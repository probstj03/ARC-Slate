#pragma once
// Calendar, to-do list and weather: shared store + the background task that keeps it fresh.
//
// The fetch task runs one request at a time (TLS needs a lot of RAM) and only while a real
// WiFi network (not ARC-Net) is connected and the clock is plausible (certificate validity).
// The last good data is cached on LittleFS and shown immediately after a reboot.
#include <Arduino.h>
#include "core/agenda.h"
#include "core/todo.h"
#include "core/weather.h"

namespace clockdata {

constexpr int kMaxTasks = 32;

enum Source : uint8_t { SRC_CALENDAR = 0, SRC_TODO = 1, SRC_WEATHER = 2, SRC_COUNT = 3 };

struct SourceState {
    bool ok = false;           // the data shown is from a successful fetch (possibly from the cache)
    int64_t at = 0;            // local seconds of the last success
    int64_t tried = 0;         // local seconds of the last attempt
    uint32_t revision = 0;     // bumped when the content changed
    char error[48] = {0};      // reason of the last failure ("" = none)
};

struct Data {
    ac::Agenda agenda;
    ac::Task tasks[kMaxTasks];
    int taskCount = 0;
    ac::Weather weather;
    SourceState src[SRC_COUNT];
};

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

}  // namespace clockdata
