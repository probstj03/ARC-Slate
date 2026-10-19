// Host tests for the pure ARC-Clock modules (src/clock/core). No Arduino, no ESP.
//   py test_host/run.py        (needs a C++ compiler: g++, clang++, or `pip install ziglang`)
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <stdlib.h>
#include "agenda.h"
#include "anim.h"
#include "civil.h"
#include "fold.h"
#include "ics.h"
#include "moon.h"
#include "todo.h"
#include "weather.h"

using namespace ac;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        g_checks++;                                                                    \
        if (strcmp((a), (b)) != 0) { g_fail++; printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } \
    } while (0)

static int64_t L(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    Civil c; c.year = y; c.month = mo; c.day = d; c.hour = h; c.minute = mi; c.second = s;
    return toSec(c);
}

static void testCivil() {
    CHECK(daysFromCivil(1970, 1, 1) == 0);
    CHECK(daysFromCivil(2000, 3, 1) == 11017);
    int y, m, d;
    civilFromDays(daysFromCivil(2026, 10, 12), y, m, d);
    CHECK(y == 2026 && m == 10 && d == 12);
    CHECK(weekday(daysFromCivil(2026, 10, 12)) == 1);      // Monday
    CHECK(weekday(daysFromCivil(1970, 1, 1)) == 4);        // Thursday
    CHECK(weekday(daysFromCivil(2026, 3, 29)) == 7);       // Sunday
    CHECK(isoWeek(2026, 10, 12) == 42);
    CHECK(isoWeek(2026, 1, 1) == 1);
    CHECK(isoWeek(2027, 1, 1) == 53);                      // Friday 1 Jan 2027 belongs to week 53 of 2026
    CHECK(isoWeek(2024, 12, 30) == 1);

    const Civil c = fromSec(L(2026, 10, 12, 14, 32, 9));
    CHECK(c.year == 2026 && c.month == 10 && c.day == 12 && c.hour == 14 && c.minute == 32 && c.second == 9);
    CHECK(dayStart(L(2026, 10, 12, 23, 59, 59)) == L(2026, 10, 12));
    CHECK(dayStart(L(1969, 12, 31, 12)) == L(1969, 12, 31));   // negative seconds floor correctly

    // EU summer time 2026: 29 March 01:00 UTC .. 25 October 01:00 UTC
    const int64_t mar = L(2026, 3, 29, 1, 0, 0);
    CHECK(utcOffsetBerlin(mar - 1) == 3600);
    CHECK(utcOffsetBerlin(mar) == 7200);
    const int64_t oct = L(2026, 10, 25, 1, 0, 0);
    CHECK(utcOffsetBerlin(oct - 1) == 7200);
    CHECK(utcOffsetBerlin(oct) == 3600);
    CHECK(utcToLocal(L(2026, 7, 1, 12)) == L(2026, 7, 1, 14));
    CHECK(utcToLocal(L(2026, 12, 1, 12)) == L(2026, 12, 1, 13));
    CHECK(localToUtc(L(2026, 7, 1, 14)) == L(2026, 7, 1, 12));
    CHECK(localToUtc(L(2026, 12, 1, 13)) == L(2026, 12, 1, 12));

    Civil p;
    bool hasTime, utc;
    CHECK(parseIcalDateTime("20261013T143000Z", p, hasTime, utc) && hasTime && utc && p.hour == 14 && p.minute == 30);
    CHECK(parseIcalDateTime("20261013", p, hasTime, utc) && !hasTime && p.day == 13);
    CHECK(!parseIcalDateTime("2026-10-13", p, hasTime, utc));
    CHECK(!parseIcalDateTime("20261313", p, hasTime, utc));
    CHECK(parseIsoDateTime("2026-10-13T07:12", p, hasTime) && hasTime && p.hour == 7 && p.minute == 12);
    CHECK(parseIsoDateTime("2026-10-13", p, hasTime) && !hasTime);
    CHECK_STR(weekdayName(1, false), "Monday");
    CHECK_STR(monthName(3, false), "March");
}

static void testFold() {
    char out[64];
    foldAscii(out, sizeof(out), "M\xC3\xBCller Stra\xC3\x9F" "e \xC3\x84\xC3\x96\xC3\x9C \xC3\xA9");
    CHECK_STR(out, "M\xFCller Stra\xDF" "e \xC4\xD6\xDC \xE9");   // Latin-1: one byte per umlaut
    foldAscii(out, sizeof(out), "a \xE2\x80\x93 b \xE2\x80\x9Cx\xE2\x80\x9D \xE2\x80\xA6 5\xE2\x82\xAC");
    CHECK_STR(out, "a - b \"x\" ... 5EUR");
    foldAscii(out, sizeof(out), "Party \xF0\x9F\x8E\x89 jetzt");        // emoji dropped
    CHECK_STR(out, "Party  jetzt");
    char small[8];
    foldAscii(small, sizeof(small), "abcdefghijkl");
    CHECK_STR(small, "abcdefg");                                          // truncated, terminated
}

static void testMoon() {
    // Full moon 2025-10-07 03:47 UTC, new moon 2025-10-21 12:25 UTC (mean-cycle accuracy: ±1 day).
    const int64_t full = daysFromCivil(2025, 10, 7) * 86400 + 3 * 3600 + 47 * 60;
    const int64_t nm = daysFromCivil(2025, 10, 21) * 86400 + 12 * 3600 + 25 * 60;
    MoonInfo f = moonAt(full), n = moonAt(nm);
    CHECK(f.illum > 0.95);
    CHECK(f.phase == 4);
    CHECK(n.illum < 0.05);
    CHECK(n.phase == 0);
    // a week after full moon: waning, about half lit
    MoonInfo q = moonAt(full + 7 * 86400);
    CHECK(q.illum > 0.3 && q.illum < 0.7);
    CHECK(q.phase == 5 || q.phase == 6);
    // next full moon after a new moon is about 14.8 days later
    const int64_t nf = nextMoonEvent(nm + 3600, true);
    CHECK(nf - nm > 14 * 86400 && nf - nm < 16 * 86400);
    CHECK_STR(moonPhaseName(4), "Full moon");
}

// ── iCalendar / CalDAV ───────────────────────────────────────────────────

static std::vector<Event> g_events;
static void collect(const Event& e, void*) { g_events.push_back(e); }

static const char* kMultistatus =
    "<?xml version=\"1.0\"?>\r\n"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:cal=\"urn:ietf:params:xml:ns:caldav\">\r\n"
    "<d:response><d:href>/remote.php/dav/calendars/julian/personal/a.ics</d:href><d:propstat><d:prop>\r\n"
    "<d:getetag>&quot;abc&quot;</d:getetag>\r\n"
    "<cal:calendar-data>BEGIN:VCALENDAR&#13;\n"
    "VERSION:2.0&#13;\n"
    "BEGIN:VTIMEZONE&#13;\n"
    "TZID:Europe/Berlin&#13;\n"
    "BEGIN:DAYLIGHT&#13;\n"
    "DTSTART:19700329T020000&#13;\n"
    "END:DAYLIGHT&#13;\n"
    "END:VTIMEZONE&#13;\n"
    "BEGIN:VEVENT&#13;\n"
    "UID:1&#13;\n"
    "DTSTART:20261013T123000Z&#13;\n"
    "DTEND:20261013T133000Z&#13;\n"
    "SUMMARY:Zahnarzt \\, Dr. M\xC3\xBCller &amp; Kollegen&#13;\n"
    "LOCATION:Hauptstra\xC3\x9F" "e 5&#13;\n"
    "BEGIN:VALARM&#13;\n"
    "ACTION:DISPLAY&#13;\n"
    "SUMMARY:ALARM DARF NICHT ERSCHEINEN&#13;\n"
    "TRIGGER:-PT15M&#13;\n"
    "END:VALARM&#13;\n"
    "END:VEVENT&#13;\n"
    "END:VCALENDAR&#13;\n"
    "</cal:calendar-data>\r\n"
    "</d:prop></d:propstat></d:response>\r\n"
    "<d:response><d:href>b.ics</d:href><d:propstat><d:prop>\r\n"
    "<cal:calendar-data>BEGIN:VCALENDAR\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Berlin:20261014T090000\r\n"
    "DURATION:PT1H30M\r\n"
    "SUMMARY:Ein sehr langer Titel der ueber mehrere Zeilen gefaltet wurde und \r\n"
    " trotzdem zusammengesetzt werden muss\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART;VALUE=DATE:20261015\r\n"
    "DTEND;VALUE=DATE:20261017\r\n"
    "SUMMARY:Urlaub\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART:20261016T080000Z\r\n"
    "STATUS:CANCELLED\r\n"
    "SUMMARY:Abgesagt\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n"
    "</cal:calendar-data>\r\n"
    "</d:prop></d:propstat></d:response></d:multistatus>\r\n";

static void checkEvents() {
    CHECK(g_events.size() == 3);
    if (g_events.size() != 3) return;
    const Event& a = g_events[0];
    CHECK(a.start == L(2026, 10, 13, 14, 30));            // 12:30 UTC = 14:30 CEST
    CHECK(a.end == L(2026, 10, 13, 15, 30));
    CHECK(!a.allDay);
    CHECK_STR(a.summary, "Zahnarzt , Dr. M\xFCller & Kollegen");
    CHECK_STR(a.location, "Hauptstra\xDF" "e 5");
    const Event& b = g_events[1];
    CHECK(b.start == L(2026, 10, 14, 9, 0));
    CHECK(b.end == L(2026, 10, 14, 10, 30));
    CHECK(strncmp(b.summary, "Ein sehr langer Titel der ueber mehrere Zeilen gefaltet wurde und trotzdem", 20) == 0);
    CHECK(strstr(b.summary, "trotzdem") != nullptr || strlen(b.summary) == sizeof(b.summary) - 1);
    const Event& c = g_events[2];
    CHECK(c.allDay);
    CHECK(c.start == L(2026, 10, 15));
    CHECK(c.end == L(2026, 10, 17));
    CHECK_STR(c.summary, "Urlaub");
}

static void testCalDav() {
    // whole body at once
    g_events.clear();
    {
        CalDavStream s(collect, nullptr);
        s.feed(kMultistatus, strlen(kMultistatus));
        s.finish();
    }
    checkEvents();

    // byte by byte (HTTP chunks can split anywhere, also inside tags and entities)
    g_events.clear();
    {
        CalDavStream s(collect, nullptr);
        for (size_t i = 0; kMultistatus[i]; i++) s.feed(kMultistatus + i, 1);
        s.finish();
    }
    checkEvents();

    // plain iCalendar without XML
    g_events.clear();
    {
        const char* ics = "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART:20260101T000000\nSUMMARY:Neujahr\nEND:VEVENT\nEND:VCALENDAR";
        IcsParser p(collect, nullptr);
        p.feed(ics, strlen(ics));
        p.flush();
        CHECK(g_events.size() == 1);
        if (!g_events.empty()) CHECK(g_events[0].start == L(2026, 1, 1));    // floating = local
    }
}

// ── to-do ────────────────────────────────────────────────────────────────

static std::vector<Task> g_tasks;
static void collectTask(const Task& t, void*) { g_tasks.push_back(t); }

static const char* kTodoMd =
    "# Aufgaben\n"
    "- [ ] Ohne Datum #privat\n"
    "- [x] Erledigt\n"
    "- [ ] Steuer machen \xF0\x9F\x93\x85 2026-10-15 \xE2\x8F\xAB\n"
    "## Heute\n"
    "- [ ] Bericht an **Herrn M\xC3\xBCller** senden [[Projekt X|Projekt]] \xF0\x9F\x93\x85 2026-10-12\n"
    "  - [/] Unteraufgabe [due:: 2026-10-11]\n"
    "* [ ] Link [Doku](https://example.org/x) \xE2\x8F\xB3 2026-10-20 ^abc123\n"
    "- [-] Abgebrochen\n"
    "## Spaeter\n"
    "- [ ] Nur in anderer Sektion\n";

static void testTodo() {
    g_tasks.clear();
    {
        TodoParser p(nullptr, collectTask, nullptr);
        for (const char* c = kTodoMd; *c; c++) p.feed(c, 1);
        p.finish();
    }
    CHECK(g_tasks.size() == 6);
    if (g_tasks.size() == 6) {
        CHECK_STR(g_tasks[0].text, "Ohne Datum");
        CHECK(!g_tasks[0].hasDate);
        CHECK_STR(g_tasks[1].text, "Steuer machen");
        CHECK(g_tasks[1].hasDate && g_tasks[1].date == L(2026, 10, 15));
        CHECK(g_tasks[1].priority == 2);
        CHECK_STR(g_tasks[2].text, "Bericht an Herrn M\xFCller senden Projekt");
        CHECK(g_tasks[2].date == L(2026, 10, 12));
        CHECK_STR(g_tasks[3].text, "Unteraufgabe");
        CHECK(g_tasks[3].date == L(2026, 10, 11));
        CHECK_STR(g_tasks[4].text, "Link Doku");
        CHECK(g_tasks[4].date == L(2026, 10, 20));            // scheduled counts when there is no due date
    }

    // only the section below "## Heute"
    g_tasks.clear();
    {
        TodoParser p("Heute", collectTask, nullptr);
        p.feed(kTodoMd, strlen(kTodoMd));
        p.finish();
    }
    CHECK(g_tasks.size() == 3);

    // order: overdue, today, soon, later, undated
    g_tasks.clear();
    {
        TodoParser p(nullptr, collectTask, nullptr);
        p.feed(kTodoMd, strlen(kTodoMd));
        p.finish();
    }
    const int64_t today = L(2026, 10, 12);
    sortTasks(g_tasks.data(), (int)g_tasks.size(), today);
    CHECK_STR(g_tasks[0].text, "Unteraufgabe");              // overdue (11.10.)
    CHECK(g_tasks[0].overdueFlag);
    CHECK_STR(g_tasks[1].text, "Bericht an Herrn M\xFCller senden Projekt");   // today
    CHECK_STR(g_tasks[2].text, "Steuer machen");             // in 3 days
    CHECK_STR(g_tasks[3].text, "Link Doku");                 // in 8 days = later
    CHECK_STR(g_tasks[4].text, "Ohne Datum");                // undated
}

// ── agenda ───────────────────────────────────────────────────────────────

static Event mk(int64_t start, int64_t end, bool allDay, const char* s) {
    Event e;
    e.start = start; e.end = end; e.allDay = allDay;
    strncpy(e.summary, s, sizeof(e.summary) - 1);
    return e;
}

static void testAgenda() {
    Agenda a;
    const int64_t today = L(2026, 10, 12);
    const int64_t horizon = today + 7 * 86400;
    CHECK(a.add(mk(L(2026, 10, 12, 16), L(2026, 10, 12, 17), false, "Spaeter"), today, horizon));
    CHECK(a.add(mk(L(2026, 10, 12, 9), L(2026, 10, 12, 10), false, "Frueh"), today, horizon));
    CHECK(a.add(mk(L(2026, 10, 12), L(2026, 10, 13), true, "Feiertag"), today, horizon));
    CHECK(a.add(mk(L(2026, 10, 12, 9), L(2026, 10, 12, 10), false, "Frueh"), today, horizon));          // duplicate (second calendar)
    CHECK(!a.add(mk(L(2026, 10, 11, 9), L(2026, 10, 11, 10), false, "Gestern"), today, horizon));       // already over
    CHECK(!a.add(mk(L(2026, 10, 20, 9), L(2026, 10, 20, 10), false, "Zu weit"), today, horizon));
    a.finalize();
    CHECK(a.n == 3);
    CHECK_STR(a.ev[0].summary, "Feiertag");                    // all-day first
    CHECK_STR(a.ev[1].summary, "Frueh");
    CHECK_STR(a.ev[2].summary, "Spaeter");
    bool ongoing = true;
    CHECK(a.nextTimed(L(2026, 10, 12, 8), ongoing) == 1 && !ongoing);
    CHECK(a.nextTimed(L(2026, 10, 12, 9, 30), ongoing) == 1 && ongoing);
    CHECK(a.nextTimed(L(2026, 10, 12, 10, 30), ongoing) == 2 && !ongoing);
    CHECK(a.nextTimed(L(2026, 10, 12, 18), ongoing) == -1);
    CHECK(a.countOnDay(today) == 3);

    char buf[24];
    dayLabel(today, today, buf, sizeof(buf)); CHECK_STR(buf, "Today");
    dayLabel(today + 86400, today, buf, sizeof(buf)); CHECK_STR(buf, "Tomorrow");
    dayLabel(today + 3 * 86400, today, buf, sizeof(buf)); CHECK_STR(buf, "Thu 15.10.");
    timeLabel(a.ev[1], buf, sizeof(buf)); CHECK_STR(buf, "09:00-10:00");
    timeLabel(a.ev[0], buf, sizeof(buf)); CHECK_STR(buf, "all day");
    const uint32_t h1 = a.hash();
    a.ev[1].summary[0] = 'X';
    CHECK(a.hash() != h1);
}

// ── weather ──────────────────────────────────────────────────────────────

static void testWeather() {
    CHECK(kindFromWmo(0) == WxKind::Clear);
    CHECK(kindFromWmo(2) == WxKind::PartlyCloudy);
    CHECK(kindFromWmo(3) == WxKind::Cloudy);
    CHECK(kindFromWmo(45) == WxKind::Fog);
    CHECK(kindFromWmo(53) == WxKind::Drizzle);
    CHECK(kindFromWmo(63) == WxKind::Rain);
    CHECK(kindFromWmo(73) == WxKind::Snow);
    CHECK(kindFromWmo(81) == WxKind::Showers);
    CHECK(kindFromWmo(95) == WxKind::Thunder);
    CHECK(kindFromWmo(42) == WxKind::Unknown);

    // Shape of an Open-Meteo response (current + hourly + daily, 2 days), shortened.
    std::string json =
        "{\"latitude\":52.52,\"longitude\":13.41,\"timezone\":\"Europe/Berlin\","
        "\"current\":{\"time\":\"2026-10-12T14:30\",\"temperature_2m\":14.6,\"relative_humidity_2m\":71,"
        "\"apparent_temperature\":12.9,\"is_day\":1,\"weather_code\":61,\"wind_speed_10m\":18.4},"
        "\"hourly\":{\"time\":[";
    for (int i = 0; i < 48; i++) {
        char t[40];
        snprintf(t, sizeof(t), "%s\"2026-10-%02dT%02d:00\"", i ? "," : "", 12 + i / 24, i % 24);
        json += t;
    }
    json += "],\"temperature_2m\":[";
    for (int i = 0; i < 48; i++) { char t[16]; snprintf(t, sizeof(t), "%s%.1f", i ? "," : "", 8.0 + i * 0.25); json += t; }
    json += "],\"precipitation_probability\":[";
    for (int i = 0; i < 48; i++) { char t[8]; snprintf(t, sizeof(t), "%s%d", i ? "," : "", (i * 3) % 100); json += t; }
    json += "],\"weather_code\":[";
    for (int i = 0; i < 48; i++) { char t[8]; snprintf(t, sizeof(t), "%s%d", i ? "," : "", i < 24 ? 3 : 61); json += t; }
    json += "]},\"daily\":{\"time\":[\"2026-10-12\",\"2026-10-13\"],\"weather_code\":[61,3],"
            "\"temperature_2m_max\":[15.2,13.0],\"temperature_2m_min\":[8.1,7.4],"
            "\"sunrise\":[\"2026-10-12T07:12\",\"2026-10-13T07:14\"],\"sunset\":[\"2026-10-12T18:40\",\"2026-10-13T18:38\"],"
            "\"precipitation_probability_max\":[80,30]}}";

    Weather w;
    CHECK(parseOpenMeteo(json.c_str(), json.size(), L(2026, 10, 12, 14, 30), w));
    CHECK(w.valid);
    CHECK(w.temp10 == 146);
    CHECK(w.feels10 == 129);
    CHECK(w.wmo == 61);
    CHECK(w.isDay);
    CHECK(w.humidity == 71);
    CHECK(w.windKmh == 18);
    CHECK(w.day0 == L(2026, 10, 12));
    CHECK(w.hTemp10[0] == 80 && w.hTemp10[24] == 140);
    CHECK(w.hWmo[3] == 3 && w.hWmo[30] == 61);
    CHECK(w.hPop[10] == 30);
    CHECK(w.day[0].sunrise == L(2026, 10, 12, 7, 12));
    CHECK(w.day[0].sunset == L(2026, 10, 12, 18, 40));
    CHECK(w.day[1].sunrise == L(2026, 10, 13, 7, 14));
    CHECK(w.day[0].tMax10 == 152 && w.day[0].tMin10 == 81);
    CHECK(w.day[0].popMax == 80);

    Weather bad;
    CHECK(!parseOpenMeteo("{\"error\":true}", 14, 0, bad));
    CHECK(!parseOpenMeteo("not json", 8, 0, bad));
    CHECK(!bad.valid);
}

static int lit(const Canvas& c) {
    int n = 0;
    for (int y = 0; y < kFieldH; y++)
        for (int x = 0; x < kFieldW; x++) n += c.get(x, y);
    return n;
}

static void testAnim() {
    const bool preview = getenv("ARC_PREVIEW") != nullptr;
    char buf[400];
    const WxKind kinds[] = {WxKind::Clear, WxKind::PartlyCloudy, WxKind::Cloudy, WxKind::Fog, WxKind::Drizzle,
                            WxKind::Rain, WxKind::Showers, WxKind::Snow, WxKind::Thunder, WxKind::Unknown};
    for (WxKind k : kinds) {
        for (int day = 0; day < 2; day++) {
            int changes = 0;
            Canvas prev;
            for (uint32_t f = 0; f < 40; f++) {
                Canvas c;
                drawWeather(c, k, day == 1, f);
                CHECK(lit(c) > 0);
                if (f > 0 && memcmp(prev.col, c.col, sizeof(c.col)) != 0) changes++;
                prev = c;
                if (preview && f < 4 && day == 1) {
                    dump(c, buf, sizeof(buf));
                    printf("%s frame %u:\n%s\n", kindName(k), (unsigned)f, buf);
                }
            }
            if (k != WxKind::Unknown) CHECK(changes > 5);   // it really animates (the "?" stays still)
        }
    }
    // moon: full is brighter than new; waxing is lit on the right, waning on the left
    Canvas nm, fm, wax, wan;
    drawMoon(nm, 8, 0.0);
    drawMoon(fm, 8, 0.5);
    drawMoon(wax, 8, 0.25);
    drawMoon(wan, 8, 0.75);
    CHECK(lit(fm) > lit(nm) + 20);
    int waxLeft = 0, waxRight = 0, wanLeft = 0, wanRight = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 4; x++) { waxLeft += wax.get(8 + x, y); wanLeft += wan.get(8 + x, y); }
        for (int x = 4; x < 8; x++) { waxRight += wax.get(8 + x, y); wanRight += wan.get(8 + x, y); }
    }
    CHECK(waxRight > waxLeft);
    CHECK(wanLeft > wanRight);
    if (preview) {
        dump(wax, buf, sizeof(buf));
        printf("moon first quarter:\n%s\n", buf);
        dump(fm, buf, sizeof(buf));
        printf("moon full:\n%s\n", buf);
    }

    Canvas up, down;
    drawSunEvent(up, true, 8);
    drawSunEvent(down, false, 8);
    CHECK(memcmp(up.col, down.col, sizeof(up.col)) != 0);
    Canvas riseStart, riseEnd;
    drawSunEvent(riseStart, true, 0);
    drawSunEvent(riseEnd, true, 8);
    CHECK(lit(riseEnd) > lit(riseStart));
    if (preview) {
        dump(riseEnd, buf, sizeof(buf));
        printf("sunrise:\n%s\n", buf);
    }
}

int main() {
    testCivil();
    testFold();
    testMoon();
    testCalDav();
    testTodo();
    testAgenda();
    testWeather();
    testAnim();
    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
