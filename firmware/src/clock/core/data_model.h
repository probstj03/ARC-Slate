#pragma once
// The data the clock shows (pure, no Arduino): calendar, to-do list, weather and the state of
// each source. The store (data_store.cpp) fills it, matrices and e-ink read it.
#include <stdint.h>
#include "agenda.h"
#include "todo.h"
#include "weather.h"

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

}  // namespace clockdata
