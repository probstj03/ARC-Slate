#include "epd_layout.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../assets/logo_jp.h"
#include "../core/civil.h"
#include "../core/moon.h"
#include "../fonts/Lato10pt.h"
#include "../fonts/Lato12pt.h"
#include "../fonts/LatoBold12pt.h"
#include "../fonts/LatoBold16pt.h"
#include "../fonts/LatoBold20pt.h"
#include "../fonts/LatoBold28pt.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Only content that changes at most once a day (or when the calendar / to-do list really changes) is drawn here:
// epd_clock.cpp redraws the panel only when the picture differs. Everything current (time, current weather,
// reminders) is on the LED matrices. Texts from the calendar / to-do are Latin-1 (umlauts as one byte, core/fold.h).

namespace epdlayout {

namespace {

constexpr uint16_t kBlack = 0x0000, kWhite = 0xFFFF;

constexpr int HEADER_H = 96;
constexpr int WX_Y = 100, WX_H = 96;
constexpr int BODY_Y = 206, BODY_BOTTOM = 476;
constexpr int SEP_X = 440;
constexpr int BAR_H = 32;                 // black heading bars
constexpr int ROW_H = 30;

const GFXfont* const F_SMALL = &Lato10pt;
const GFXfont* const F_TEXT = &Lato12pt;
const GFXfont* const F_TEXT_B = &LatoBold12pt;
const GFXfont* const F_HEAD = &LatoBold16pt;
const GFXfont* const F_BIG = &LatoBold20pt;
const GFXfont* const F_HUGE = &LatoBold28pt;

Adafruit_GFX* G = nullptr;
const clockdata::Data* S = nullptr;
Options opt;

// ── text helpers ─────────────────────────────────────────────────────────

void printAt(int x, int y, const GFXfont* font, const char* text) {
    G->setFont(font);
    G->setCursor(x, y);
    G->print(text);
}

int textWidth(const GFXfont* font, const char* text) {
    int16_t bx, by;
    uint16_t bw, bh;
    G->setFont(font);
    G->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    return (int)bw + (bx > 0 ? bx : 0);
}

void printRight(int xRight, int y, const GFXfont* font, const char* text) {
    int16_t bx, by;
    uint16_t bw, bh;
    G->setFont(font);
    G->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    G->setCursor(xRight - (int)bw - bx, y);
    G->print(text);
}

// Shortens `text` with "..." until it fits into maxW pixels.
void printFit(int x, int y, const GFXfont* font, const char* text, int maxW) {
    char buf[96];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    if (textWidth(font, buf) > maxW) {
        size_t n = strlen(buf);
        while (n > 1) {
            buf[--n] = 0;
            while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
            char t[100];
            snprintf(t, sizeof(t), "%s...", buf);
            if (textWidth(font, t) <= maxW) { strcpy(buf, t); break; }
        }
    }
    printAt(x, y, font, buf);
}

void headingBar(int x, int y, int w, const char* title) {
    G->fillRect(x, y, w, BAR_H, kBlack);
    G->setTextColor(kWhite);
    printAt(x + 10, y + 25, F_HEAD, title);
    G->setTextColor(kBlack);
}

void thickLine(int y) { G->fillRect(0, y, epdlayout::W, 4, kBlack); }

// ── icons ────────────────────────────────────────────────────────────────

// A line with round caps, `t` pixels thick.
void thickStroke(int x0, int y0, int x1, int y1, int t) {
    const int steps = max(abs(x1 - x0), abs(y1 - y0));
    for (int i = 0; i <= steps; i++) {
        const int x = x0 + (x1 - x0) * i / (steps ? steps : 1), y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        G->fillCircle(x, y, t / 2, kBlack);
    }
}

// Sun: solid disc, a white gap, 8 short rays with round ends.
void drawSun(int cx, int cy, int r) {
    G->fillCircle(cx, cy, r, kBlack);
    const int r0 = r + r / 2 + 2, r1 = r * 2 + 4;
    for (int i = 0; i < 8; i++) {
        const float a = i * (float)M_PI / 4.0f;
        thickStroke(cx + (int)lroundf(cosf(a) * r0), cy + (int)lroundf(sinf(a) * r0), cx + (int)lroundf(cosf(a) * r1),
                    cy + (int)lroundf(sinf(a) * r1), r >= 12 ? 5 : 4);
    }
}

void drawMoonIcon(int cx, int cy, int r, double phaseFrac) {
    const double k = cos(2.0 * M_PI * phaseFrac);      // 1 new, -1 full
    const bool waxing = phaseFrac < 0.5;
    for (int d = 0; d < 3; d++) G->drawCircle(cx, cy, r - d, kBlack);
    for (int y = -r + 1; y < r; y++) {
        const double hw = sqrt((double)r * r - (double)y * y);
        int x0, x1;
        if (waxing) { x0 = (int)lround(k * hw); x1 = (int)floor(hw); }
        else { x0 = (int)ceil(-hw); x1 = (int)lround(-k * hw); }
        if (x1 > x0) G->drawFastHLine(cx + x0, cy + y, x1 - x0 + 1, kBlack);
    }
}

void drawCloud(int ox, int oy, int s, bool outlineWhite = false) {
    auto shape = [&](uint16_t color, int grow) {
        G->fillCircle(ox + s * 22 / 64, oy + s * 38 / 64, s * 11 / 64 + grow, color);
        G->fillCircle(ox + s * 36 / 64, oy + s * 30 / 64, s * 15 / 64 + grow, color);
        G->fillCircle(ox + s * 50 / 64, oy + s * 39 / 64, s * 10 / 64 + grow, color);
        G->fillRoundRect(ox + s * 12 / 64 - grow, oy + s * 38 / 64 - grow, s * 46 / 64 + 2 * grow, s * 14 / 64 + 2 * grow, s * 7 / 64, color);
    };
    if (outlineWhite) shape(kWhite, 4);
    shape(kBlack, 0);
}

void drawWeatherIcon(int ox, int oy, ac::WxKind kind) {       // within a 72x72 box, day symbols
    switch (kind) {
        case ac::WxKind::Clear:
            drawSun(ox + 36, oy + 36, 14);
            break;
        case ac::WxKind::PartlyCloudy:
            drawSun(ox + 26, oy + 26, 10);
            drawCloud(ox + 8, oy + 16, 64, true);
            break;
        case ac::WxKind::Cloudy:
            drawCloud(ox, oy + 2, 72);
            break;
        case ac::WxKind::Fog:
            for (int i = 0; i < 4; i++) G->fillRoundRect(ox + 6 + (i % 2) * 10, oy + 16 + i * 13, 52, 6, 3, kBlack);
            break;
        case ac::WxKind::Drizzle:
            drawCloud(ox, oy - 6, 72);
            for (int i = 0; i < 4; i++) G->fillCircle(ox + 18 + i * 12, oy + 54 + (i % 2) * 7, 3, kBlack);
            break;
        case ac::WxKind::Rain:
        case ac::WxKind::Showers:
            drawCloud(ox, oy - 6, 72);
            for (int i = 0; i < 4; i++) {
                const int x = ox + 18 + i * 12, y = oy + 50 + (i % 2) * 6;
                thickStroke(x, y, x - 4, y + 12, 3);
            }
            break;
        case ac::WxKind::Snow:
            drawCloud(ox, oy - 6, 72);
            for (int i = 0; i < 3; i++) {
                const int x = ox + 20 + i * 16, y = oy + 60 + (i % 2) * 5;
                thickStroke(x - 5, y, x + 5, y, 3);
                thickStroke(x, y - 5, x, y + 5, 3);
            }
            break;
        case ac::WxKind::Thunder:
            drawCloud(ox, oy - 8, 72);
            G->fillTriangle(ox + 38, oy + 44, ox + 24, oy + 64, ox + 36, oy + 62, kBlack);
            G->fillTriangle(ox + 34, oy + 60, ox + 46, oy + 46, ox + 30, oy + 71, kBlack);
            break;
        default:
            break;
    }
}

// ── header: logo, date, week, moon ───────────────────────────────────────

void drawHeader(int64_t now) {
    G->drawBitmap(10, (HEADER_H - LOGO_JP_H) / 2, logo_jp, LOGO_JP_W, LOGO_JP_H, kBlack);

    const ac::Civil c = ac::fromSec(now);
    const int wd = ac::weekday(ac::daysOf(now));
    char date[48];
    snprintf(date, sizeof(date), "%s, %s %d", ac::weekdayName(wd, false), ac::monthName(c.month, false), c.day);
    const int tx = 10 + LOGO_JP_W + 22;
    const GFXfont* f = textWidth(F_HUGE, date) <= 560 - tx ? F_HUGE : F_BIG;
    printAt(tx, 52, f, date);
    char week[16];
    snprintf(week, sizeof(week), "Week %d", ac::isoWeek(c.year, c.month, c.day));
    printAt(tx, 82, F_TEXT, week);

    const ac::MoonInfo m = ac::moonAt(ac::localToUtc(now));
    const int mx = 572;
    drawMoonIcon(mx, 48, 24, m.age / ac::kSynodicMonth);
    const int px = mx + 36, pw = W - 8 - px;
    printFit(px, 44, F_TEXT_B, ac::moonPhaseName(m.phase), pw);
    const int64_t nextFull = ac::nextMoonEvent(ac::localToUtc(now), true);
    const int64_t nextNew = ac::nextMoonEvent(ac::localToUtc(now), false);
    const bool fullFirst = nextFull < nextNew;
    const int days = (int)((((fullFirst ? nextFull : nextNew) - ac::localToUtc(now)) + 43200) / 86400);
    char moon[48];
    if (days <= 0) snprintf(moon, sizeof(moon), "%s today", fullFirst ? "Full moon" : "New moon");
    else snprintf(moon, sizeof(moon), "%s in %d day%s", fullFirst ? "Full moon" : "New moon", days, days == 1 ? "" : "s");
    printFit(px, 72, F_SMALL, moon, pw);
    thickLine(HEADER_H);
}

// ── weather: forecast of today (snapshot of the morning refresh) and the place ──

void tempText(char* out, size_t cap, int t10) { snprintf(out, cap, "%d\xB0", (int)lroundf(t10 / 10.0f)); }

void drawWeatherBand(int64_t now) {
    const ac::Weather& w = S->weather;
    // place on the right, always
    if (opt.place && opt.place[0]) printRight(W - 12, WX_Y + 34, F_HEAD, opt.place);
    if (!w.valid) {
        printAt(14, WX_Y + 42, F_TEXT_B, "No weather data");
        const char* err = S->src[clockdata::SRC_WEATHER].error;
        printFit(14, WX_Y + 72, F_SMALL, err[0] ? err : "not fetched yet", 420);
        thickLine(WX_Y + WX_H);
        return;
    }
    const int di = (int)((ac::dayStart(now) - w.day0) / 86400);
    const ac::DayInfo day = (di >= 0 && di < 3) ? w.day[di] : ac::DayInfo();
    const ac::WxKind kind = ac::kindFromWmo(day.wmo);

    drawWeatherIcon(12, WX_Y + 12, kind);
    char hi[12], lo[12], line[64];
    tempText(hi, sizeof(hi), day.tMax10);
    tempText(lo, sizeof(lo), day.tMin10);
    printAt(104, WX_Y + 50, F_HUGE, hi);
    printAt(104 + textWidth(F_HUGE, hi) + 14, WX_Y + 50, F_BIG, lo);
    snprintf(line, sizeof(line), "%s, rain %u %%", ac::kindName(kind), day.popMax);
    printFit(104, WX_Y + 82, F_TEXT, line, 330);

    if (day.sunrise && day.sunset) {
        const ac::Civil sr = ac::fromSec(day.sunrise), ss = ac::fromSec(day.sunset);
        snprintf(line, sizeof(line), "Sunrise %02d:%02d   Sunset %02d:%02d", sr.hour, sr.minute, ss.hour, ss.minute);
        printRight(W - 12, WX_Y + 72, F_TEXT, line);
    }
    thickLine(WX_Y + WX_H);
}

// ── calendar: today and tomorrow as columns, each split into morning (until 12:00) and afternoon ──

void dayHeader(int64_t ds, int64_t today, char* out, size_t cap) {
    const ac::Civil c = ac::fromSec(ds);
    const int wd = ac::weekday(ac::daysOf(ds));
    if (ds == today) snprintf(out, cap, "Today, %s %d", ac::weekdayName(wd, true), c.day);
    else if (ds == today + 86400) snprintf(out, cap, "Tomorrow, %s %d", ac::weekdayName(wd, true), c.day);
    else snprintf(out, cap, "%s %d", ac::weekdayName(wd, true), c.day);
}

void drawCalendar(int64_t now) {
    const ac::Agenda& a = S->agenda;
    const int64_t today = ac::dayStart(now);
    headingBar(0, BODY_Y, SEP_X - 8, "EVENTS");
    if (!S->src[clockdata::SRC_CALENDAR].ok) {
        printAt(12, BODY_Y + BAR_H + 34, F_TEXT_B, "Calendar unavailable");
        const char* err = S->src[clockdata::SRC_CALENDAR].error;
        printFit(12, BODY_Y + BAR_H + 62, F_SMALL, err[0] ? err : "not fetched yet", SEP_X - 30);
        return;
    }
    constexpr int DAYS = 2;
    constexpr int AXIS_W = 30;                       // hour labels
    const int x0 = AXIS_W, x1 = SEP_X - 12;
    const int colW = (x1 - x0) / DAYS;
    const int headY = BODY_Y + BAR_H + 4;            // day names
    const int allDayY = headY + 28;                  // all-day bars
    constexpr int ALLDAY_H = 20, ALLDAY_ROWS = 2;
    constexpr int NOON = 12 * 60;

    // visible hours: morning 8..12 and afternoon 12..19, widened by the events of the two days
    int amStart = 8 * 60, pmEnd = 19 * 60;
    for (int i = 0; i < a.n; i++) {
        const ac::Event& e = a.ev[i];
        if (e.allDay || e.start >= today + DAYS * 86400LL || e.end <= today) continue;
        if (ac::dayStart(e.start) >= today) amStart = min(amStart, (int)((e.start - ac::dayStart(e.start)) / 3600) * 60);
        const int64_t end = e.end > e.start ? e.end : e.start + 1800;
        if (ac::dayStart(end - 1) == ac::dayStart(e.start)) {
            const int m = (int)((end - ac::dayStart(e.start)) / 60);
            pmEnd = max(pmEnd, ((m + 59) / 60) * 60);
        }
    }

    // how many all-day rows are needed (at most two)
    int allDayRows = 0;
    for (int d = 0; d < DAYS; d++) {
        const int64_t ds = today + d * 86400LL;
        int n = 0;
        for (int i = 0; i < a.n; i++)
            if (a.ev[i].allDay && a.ev[i].start < ds + 86400 && a.ev[i].end > ds) n++;
        allDayRows = max(allDayRows, min(n, ALLDAY_ROWS));
    }
    const int gridY = allDayY + allDayRows * (ALLDAY_H + 3) + 4;
    const int halfH = (BODY_BOTTOM - gridY - 6) / 2;  // morning and afternoon get the same height
    struct Half { int from, to, y; };
    const Half halves[2] = {{amStart, NOON, gridY}, {NOON, pmEnd, gridY + halfH + 6}};
    auto yOf = [&](const Half& h, int minutes) { return h.y + (int)((long)(minutes - h.from) * halfH / (h.to - h.from)); };

    // per half: dotted hour lines, labels every two hours, a solid line between morning and afternoon
    for (const Half& h : halves) {
        for (int m = h.from; m <= h.to; m += 60) {
            const int y = yOf(h, m);
            for (int x = x0; x < x1; x += 4) G->drawPixel(x, y, kBlack);
            if (m < h.to && ((m / 60) % 2 == 0 || m == h.from)) {
                char hb[4];
                snprintf(hb, sizeof(hb), "%d", m / 60);
                printRight(AXIS_W - 6, y + 14, F_SMALL, hb);
            }
        }
    }
    G->fillRect(x0, halves[1].y - 4, x1 - x0, 2, kBlack);

    for (int d = 0; d < DAYS; d++) {
        const int64_t ds = today + d * 86400LL;
        const int cx = x0 + d * colW;
        char head[32];
        dayHeader(ds, today, head, sizeof(head));
        printFit(cx + 6, headY + 20, F_TEXT_B, head, colW - 8);
        if (d > 0) G->fillRect(cx - 1, headY, 2, BODY_BOTTOM - headY, kBlack);   // column separator

        // all-day events as black bars with white text
        int row = 0;
        for (int i = 0; i < a.n && row < ALLDAY_ROWS; i++) {
            const ac::Event& e = a.ev[i];
            if (!e.allDay || e.start >= ds + 86400 || e.end <= ds) continue;
            const int by = allDayY + row * (ALLDAY_H + 3);
            G->fillRoundRect(cx + 3, by, colW - 6, ALLDAY_H, 3, kBlack);
            G->setTextColor(kWhite);
            printFit(cx + 7, by + 15, F_SMALL, e.summary, colW - 14);
            G->setTextColor(kBlack);
            row++;
        }

        // timed events as boxes; overlapping ones share the width (lanes)
        int idx[ac::kMaxEvents], lane[ac::kMaxEvents], n = 0;
        for (int i = 0; i < a.n && n < ac::kMaxEvents; i++) {
            const ac::Event& e = a.ev[i];
            if (e.allDay || e.start >= ds + 86400 || e.end <= ds) continue;
            idx[n++] = i;
        }
        int lanes = 1;
        for (int k = 0; k < n; k++) {
            lane[k] = 0;
            for (int j = 0; j < k; j++) {
                const ac::Event& p = a.ev[idx[j]];
                const ac::Event& q = a.ev[idx[k]];
                if (p.start < q.end && q.start < p.end && lane[j] == lane[k]) { lane[k]++; j = -1; }
            }
            lanes = max(lanes, lane[k] + 1);
        }
        lanes = min(lanes, 3);
        for (int k = 0; k < n; k++) {
            if (lane[k] >= lanes) continue;
            const ac::Event& e = a.ev[idx[k]];
            const int sMin = (int)((max(e.start, ds) - ds) / 60);
            const int eMin = (int)((min(e.end > e.start ? e.end : e.start + 1800, ds + 86400) - ds) / 60);
            const int lw = (colW - 6) / lanes;
            const int bx = cx + 3 + lane[k] * lw;
            bool titled = false;
            for (const Half& h : halves) {                       // an event over noon gets a box in both halves
                const int from = max(sMin, h.from), to = min(eMin, h.to);
                if (to <= from && !(sMin >= h.from && sMin < h.to)) continue;
                const int top = yOf(h, max(from, h.from));
                const int bottom = min(max(yOf(h, to), top + 22), h.y + halfH);
                G->fillRect(bx, top, lw - 2, bottom - top, kWhite);
                G->drawRect(bx, top, lw - 2, bottom - top, kBlack);
                G->drawRect(bx + 1, top + 1, lw - 4, bottom - top - 2, kBlack);
                G->fillRect(bx, top, 4, bottom - top, kBlack);                  // accent on the left
                printFit(bx + 7, top + 16, F_SMALL, e.summary, lw - 12);
                if (!titled && bottom - top >= 40 && e.start >= ds) {           // tall enough: the time below
                    const ac::Civil st = ac::fromSec(e.start);
                    char tl[8];
                    snprintf(tl, sizeof(tl), "%02d:%02d", st.hour, st.minute);
                    printAt(bx + 7, top + 34, F_SMALL, tl);
                }
                titled = true;
            }
        }
    }
}

// ── to-do ────────────────────────────────────────────────────────────────

void drawTodo(int64_t now) {
    const int x0 = SEP_X + 14, x1 = W - 12;
    const int64_t today = ac::dayStart(now);
    headingBar(SEP_X, BODY_Y, W - SEP_X, "DASHBOARD");
    if (!S->src[clockdata::SRC_TODO].ok) {
        printAt(x0, BODY_Y + BAR_H + 34, F_TEXT_B, "Dashboard unavailable");
        const char* err = S->src[clockdata::SRC_TODO].error;
        printFit(x0, BODY_Y + BAR_H + 62, F_SMALL, err[0] ? err : "not fetched yet", x1 - x0);
        return;
    }
    int y = BODY_Y + BAR_H + 8;
    int shown = 0;
    for (int i = 0; i < S->taskCount; i++) {
        if (y + ROW_H > BODY_BOTTOM - (i + 1 < S->taskCount ? 20 : 0)) break;
        const ac::Task& t = S->tasks[i];
        const bool overdue = !t.done && t.hasDate && t.date < today;
        const bool dueToday = !t.done && t.hasDate && t.date == today;
        for (int d = 0; d < 2; d++) G->drawRect(x0 + d, y + 6 + d, 18 - 2 * d, 18 - 2 * d, kBlack);
        if (overdue || dueToday) G->fillRect(x0 + 5, y + 11, 8, 8, kBlack);     // due: filled
        if (t.done) {                                                           // ticked: a check mark
            for (int d = 0; d < 3; d++) {
                G->drawLine(x0 + 3, y + 14 + d, x0 + 7, y + 18 + d, kBlack);
                G->drawLine(x0 + 7, y + 18 + d, x0 + 16, y + 6 + d, kBlack);
            }
        }
        const GFXfont* f = (!t.done && (t.priority >= 2 || overdue || dueToday)) ? F_TEXT_B : F_TEXT;
        printFit(x0 + 30, y + 22, f, t.text, x1 - (x0 + 30));
        if (t.done) {                                                           // and struck through
            const int w = textWidth(f, t.text);
            G->fillRect(x0 + 30, y + 14, min(w, x1 - (x0 + 30)), 2, kBlack);
        }
        y += ROW_H;
        shown++;
    }
    if (shown < S->taskCount) {
        char more[24];
        snprintf(more, sizeof(more), "%d more ...", S->taskCount - shown);
        printAt(x0 + 30, BODY_BOTTOM + 2, F_SMALL, more);
    }
    if (S->taskCount == 0) printAt(x0 + 30, y + 22, F_TEXT, "All done");
}

}  // namespace

void draw(Adafruit_GFX& g, const clockdata::Data& snap, int64_t now, const Options& options) {
    G = &g;
    S = &snap;
    opt = options;
    G->fillScreen(kWhite);
    G->setTextColor(kBlack);
    G->setTextWrap(false);
    drawHeader(now);
    drawWeatherBand(now);
    drawCalendar(now);
    G->fillRect(SEP_X - 6, BODY_Y, 4, BODY_BOTTOM - BODY_Y + 4, kBlack);
    drawTodo(now);
}

}  // namespace epdlayout
