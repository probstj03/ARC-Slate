#pragma once
// Event list for the next days: collect, de-duplicate, sort, ask "what is next" (pure).
#include <stddef.h>
#include <stdint.h>
#include "ics.h"

namespace ac {

constexpr int kMaxEvents = 48;

struct Agenda {
    Event ev[kMaxEvents];
    int n = 0;

    // Adds an event overlapping [from, to) (local seconds). Returns false if dropped.
    bool add(const Event& e, int64_t from, int64_t to);
    // Sort by start (all-day first on the same day) and remove duplicates.
    void finalize();
    // Index of the first timed event that has not ended yet; ongoing = it has started.
    // -1 if there is none today or later.
    int nextTimed(int64_t now, bool& ongoing) const;
    // Number of events (incl. all-day) overlapping the given local day.
    int countOnDay(int64_t dayStart) const;
    // Hash of what is shown (change detection).
    uint32_t hash() const;
};

// "Heute", "Morgen", otherwise "Mo 13.10."
void dayLabel(int64_t day, int64_t today, char* out, size_t cap);
// "14:30" / "14:30-15:30"; all-day: "ganztags"
void timeLabel(const Event& e, char* out, size_t cap);

}  // namespace ac
