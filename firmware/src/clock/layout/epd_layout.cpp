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

// ── calendar: today and the next days with events ───────────────────────

void dayLabel(int64_t ds, int64_t today, char* out, size_t cap) {
    const ac::Civil c = ac::fromSec(ds);
    const int wd = ac::weekday(ac::daysOf(ds));
    if (ds == today) snprintf(out, cap, "Today");
    else if (ds == today + 86400) snprintf(out, cap, "Tomorrow");
    else snprintf(out, cap, "%s %d %s", ac::weekdayName(wd, false), c.day, ac::monthName(c.month, true));
}

void drawCalendar(int64_t now) {
    const ac::Agenda& a = S->agenda;
    const int64_t today = ac::dayStart(now);
    const int x1 = SEP_X - 14;
    headingBar(0, BODY_Y, SEP_X - 8, "EVENTS");
    if (!S->src[clockdata::SRC_CALENDAR].ok) {
        printAt(12, BODY_Y + BAR_H + 34, F_TEXT_B, "Calendar unavailable");
        const char* err = S->src[clockdata::SRC_CALENDAR].error;
        printFit(12, BODY_Y + BAR_H + 62, F_SMALL, err[0] ? err : "not fetched yet", x1 - 12);
        return;
    }
    int y = BODY_Y + BAR_H + 8;
    bool hidden = false;
    for (int day = 0; day < 7; day++) {
        const int64_t ds = today + (int64_t)day * 86400;
        int idx[ac::kMaxEvents];
        int n = 0;
        for (int i = 0; i < a.n; i++) {
            const int64_t end = a.ev[i].end > a.ev[i].start ? a.ev[i].end : a.ev[i].start + 1;
            if (a.ev[i].start < ds + 86400 && end > ds && n < ac::kMaxEvents) idx[n++] = i;
        }
        if (n == 0 && day != 0) continue;
        if (y + ROW_H * 2 > BODY_BOTTOM) { hidden = true; break; }
        char label[40];
        dayLabel(ds, today, label, sizeof(label));
        printAt(12, y + 22, F_TEXT_B, label);
        y += ROW_H;
        if (n == 0) {
            printAt(84, y + 22, F_TEXT, "No events");
            y += ROW_H + 8;
            continue;
        }
        for (int k = 0; k < n; k++) {
            if (y + ROW_H > BODY_BOTTOM) { hidden = true; break; }
            const ac::Event& e = a.ev[idx[k]];
            if (!e.allDay && e.start >= ds) {
                const ac::Civil s = ac::fromSec(e.start);
                char tl[8];
                snprintf(tl, sizeof(tl), "%02d:%02d", s.hour, s.minute);
                printAt(12, y + 22, F_TEXT, tl);
            }
            printFit(84, y + 22, F_TEXT, e.summary, x1 - 84);
            y += ROW_H;
        }
        if (hidden) break;
        y += 8;
    }
    if (hidden) printRight(x1, BODY_BOTTOM + 2, F_SMALL, "more ...");
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
        const bool overdue = t.hasDate && t.date < today;
        const bool dueToday = t.hasDate && t.date == today;
        for (int d = 0; d < 2; d++) G->drawRect(x0 + d, y + 6 + d, 18 - 2 * d, 18 - 2 * d, kBlack);
        if (overdue || dueToday) G->fillRect(x0 + 5, y + 11, 8, 8, kBlack);     // due: filled
        const GFXfont* f = (t.priority >= 2 || overdue || dueToday) ? F_TEXT_B : F_TEXT;
        printFit(x0 + 30, y + 22, f, t.text, x1 - (x0 + 30));
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
