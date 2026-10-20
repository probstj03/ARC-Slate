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
    // Is there a timed event on one of the `days` days from `today` (local midnight) that counts as night?
    bool hasNightEvents(int64_t today, int days, int fromH, int toH) const;
    // Hash of what is shown (change detection).
    uint32_t hash() const;
};

// A timed (not all-day) event of the local day `day` that starts before fromH or at / after toH o'clock. The start is
// clipped to the day: an event that continues past midnight counts at 00:00 of the next day.
bool isNightEvent(const Event& e, int64_t day, int fromH, int toH);

// "Today", "Tomorrow", otherwise "Mon 13.10."
void dayLabel(int64_t day, int64_t today, char* out, size_t cap);
// "14:30" / "14:30-15:30"; all-day: "all day"
void timeLabel(const Event& e, char* out, size_t cap);

}  // namespace ac
