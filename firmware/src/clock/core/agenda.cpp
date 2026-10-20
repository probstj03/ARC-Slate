#include "agenda.h"
#include <stdio.h>
#include <string.h>
#include "civil.h"

namespace ac {

bool Agenda::add(const Event& e, int64_t from, int64_t to) {
    const int64_t end = e.end > e.start ? e.end : e.start + 1;
    if (end <= from || e.start >= to) return false;
    if (n >= kMaxEvents) return false;
    ev[n++] = e;
    return true;
}

void Agenda::finalize() {
    auto before = [](const Event& a, const Event& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.allDay != b.allDay) return a.allDay;
        return strcmp(a.summary, b.summary) < 0;
    };
    for (int i = 1; i < n; i++) {
        Event key = ev[i];
        int j = i - 1;
        while (j >= 0 && before(key, ev[j])) { ev[j + 1] = ev[j]; j--; }
        ev[j + 1] = key;
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (w > 0 && ev[i].start == ev[w - 1].start && ev[i].allDay == ev[w - 1].allDay && strcmp(ev[i].summary, ev[w - 1].summary) == 0)
            continue;       // the same event reported by two calendars
        if (w != i) ev[w] = ev[i];
        w++;
    }
    n = w;
}

int Agenda::nextTimed(int64_t now, bool& ongoing) const {
    ongoing = false;
    for (int i = 0; i < n; i++) {
        if (ev[i].allDay) continue;
        const int64_t end = ev[i].end > ev[i].start ? ev[i].end : ev[i].start + 60;
        if (end <= now) continue;
        ongoing = ev[i].start <= now;
        return i;
    }
    return -1;
}

int Agenda::countOnDay(int64_t day) const {
    int c = 0;
    for (int i = 0; i < n; i++) {
        const int64_t end = ev[i].end > ev[i].start ? ev[i].end : ev[i].start + 1;
        if (ev[i].start < day + 86400 && end > day) c++;
    }
    return c;
}

bool isNightEvent(const Event& e, int64_t day, int fromH, int toH) {
    if (e.allDay) return false;
    const int64_t end = e.end > e.start ? e.end : e.start + 1800;
    if (!(e.start < day + 86400 && end > day)) return false;
    const int m = (int)((e.start > day ? e.start - day : 0) / 60);
    return m < fromH * 60 || m >= toH * 60;
}

bool Agenda::hasNightEvents(int64_t today, int days, int fromH, int toH) const {
    for (int d = 0; d < days; d++)
        for (int i = 0; i < n; i++)
            if (isNightEvent(ev[i], today + d * 86400LL, fromH, toH)) return true;
    return false;
}

uint32_t Agenda::hash() const {
    uint32_t h = 2166136261u;
    auto mix = [&h](const void* p, size_t len) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < len; i++) { h ^= b[i]; h *= 16777619u; }
    };
    for (int i = 0; i < n; i++) {
        mix(&ev[i].start, sizeof(ev[i].start));
        mix(&ev[i].end, sizeof(ev[i].end));
        mix(&ev[i].allDay, sizeof(ev[i].allDay));
        mix(ev[i].summary, strlen(ev[i].summary));
        mix(ev[i].location, strlen(ev[i].location));
    }
    return h;
}

void dayLabel(int64_t day, int64_t today, char* out, size_t cap) {
    const int64_t diff = (day - today) / 86400;
    if (diff == 0) { snprintf(out, cap, "Today"); return; }
    if (diff == 1) { snprintf(out, cap, "Tomorrow"); return; }
    const Civil c = fromSec(day);
    snprintf(out, cap, "%s %d.%d.", weekdayName(weekday(daysOf(day)), true), c.day, c.month);
}

void timeLabel(const Event& e, char* out, size_t cap) {
    if (e.allDay) { snprintf(out, cap, "all day"); return; }
    const Civil a = fromSec(e.start);
    if (e.end > e.start) {
        const Civil b = fromSec(e.end);
        snprintf(out, cap, "%02d:%02d-%02d:%02d", a.hour, a.minute, b.hour, b.minute);
    } else {
        snprintf(out, cap, "%02d:%02d", a.hour, a.minute);
    }
}

}  // namespace ac
