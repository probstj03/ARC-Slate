#include "epd_layout.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../assets/logo_jp.h"
#include "../core/civil.h"
#include "../core/moon.h"
#include "../fonts/Lato10pt.h"
#include "../fonts/Lato12pt.h"
#include "../fonts/LatoBold10pt.h"
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
constexpr int WX_Y = 100, WX_H = 74;      // weather band: the left block ends flush with the sunrise / sunset line
constexpr int BODY_Y = WX_Y + WX_H + 10, BODY_BOTTOM = 476;
constexpr int SEP_X = 490;                // calendar | dashboard (the calendar is the wider one)
constexpr int BAR_H = 28;                 // black heading bars

const GFXfont* const F_SMALL = &Lato10pt;
const GFXfont* const F_SMALL_B = &LatoBold10pt;
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
    printAt(x + 10, y + 22, F_TEXT_B, title);
    G->setTextColor(kBlack);
}

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
        // lit part [x0, x1]: right side when waxing, left side when waning (northern hemisphere)
        int x0, x1;
        if (waxing) { x0 = (int)lround(k * hw); x1 = (int)floor(hw); }
        else { x0 = (int)ceil(-hw); x1 = (int)lround(-k * hw); }
        // The dark part is filled black (inverted: black reads as shadow), the lit part stays white.
        const int left = (int)ceil(-hw), right = (int)floor(hw);
        if (x0 > left) G->drawFastHLine(cx + left, cy + y, x0 - left, kBlack);
        if (x1 < right) G->drawFastHLine(cx + x1 + 1, cy + y, right - x1, kBlack);
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
    G->fillRect(0, HEADER_H, epdlayout::W, 4, kBlack);
}

// ── weather: forecast of today (snapshot of the morning refresh) and the place ──

void tempText(char* out, size_t cap, int t10) { snprintf(out, cap, "%d\xB0", (int)lroundf(t10 / 10.0f)); }

// Location pin: a drop with a hole, 14 px wide, 22 px tall; (cx, top) = centre x and top edge.
void drawPin(int cx, int top) {
    G->fillCircle(cx, top + 7, 7, kBlack);
    G->fillTriangle(cx - 6, top + 10, cx + 6, top + 10, cx, top + 22, kBlack);
    G->fillCircle(cx, top + 7, 3, kWhite);
}

void drawWeatherBand(int64_t now) {
    const ac::Weather& w = S->weather;
    // place with a pin on the right (baseline WX_Y + 24); the left block ends at the baseline of the sunrise / sunset
    // line (WX_Y + 66)
    if (opt.place && opt.place[0]) {
        printRight(W - 12, WX_Y + 24, F_HEAD, opt.place);
        drawPin(W - 12 - textWidth(F_HEAD, opt.place) - 16, WX_Y + 3);
    }
    if (!w.valid) {
        printAt(14, WX_Y + 34, F_TEXT_B, "No weather data");
        const char* err = S->src[clockdata::SRC_WEATHER].error;
        printFit(14, WX_Y + 58, F_SMALL, err[0] ? err : "not fetched yet", 420);
        return;
    }
    const int di = (int)((ac::dayStart(now) - w.day0) / 86400);
    const ac::DayInfo day = (di >= 0 && di < 3) ? w.day[di] : ac::DayInfo();
    const ac::WxKind kind = ac::kindFromWmo(day.wmo);

    drawWeatherIcon(12, WX_Y, kind);
    char hi[12], lo[12], line[64];
    tempText(hi, sizeof(hi), day.tMax10);
    tempText(lo, sizeof(lo), day.tMin10);
    printAt(104, WX_Y + 40, F_HUGE, hi);
    printAt(104 + textWidth(F_HUGE, hi) + 14, WX_Y + 40, F_BIG, lo);
    snprintf(line, sizeof(line), "%s, rain %u %%", ac::kindName(kind), day.popMax);
    printFit(104, WX_Y + 66, F_TEXT, line, 330);

    if (day.sunrise && day.sunset) {
        const ac::Civil sr = ac::fromSec(day.sunrise), ss = ac::fromSec(day.sunset);
        snprintf(line, sizeof(line), "Sunrise %02d:%02d   Sunset %02d:%02d", sr.hour, sr.minute, ss.hour, ss.minute);
        printRight(W - 12, WX_Y + 66, F_TEXT, line);
    }
}

// ── calendar: today and tomorrow as columns, day grid 8..18 h, events as inverted (black) boxes ──

bool onDay(const ac::Event& e, int64_t ds) {
    const int64_t end = e.end > e.start ? e.end : e.start + 1800;
    return e.start < ds + 86400 && end > ds;
}

// Events outside the day grid (before DAY_FROM_H / from DAY_TO_H) go to a strip below it. Early ones are shown at
// once; evening ones of today only from EVENING_FROM_H on ("later"), those of tomorrow not at all (tomorrow they are
// today's).
bool nightShown(const ac::Event& e, int64_t ds, int day, int nowMin) {
    if (!ac::isNightEvent(e, ds, DAY_FROM_H, DAY_TO_H)) return false;
    const int m = (int)((max(e.start, ds) - ds) / 60);
    if (m < DAY_FROM_H * 60) return true;
    return day == 0 && nowMin >= EVENING_FROM_H * 60;
}

void drawCalendar(int64_t now) {
    const ac::Agenda& a = S->agenda;
    const int64_t today = ac::dayStart(now);
    const int nowMin = (int)((now - today) / 60);
    headingBar(0, BODY_Y, SEP_X - 8, "EVENTS");
    if (!S->src[clockdata::SRC_CALENDAR].ok) {
        printAt(12, BODY_Y + BAR_H + 30, F_TEXT_B, "Calendar unavailable");
        const char* err = S->src[clockdata::SRC_CALENDAR].error;
        printFit(12, BODY_Y + BAR_H + 56, F_SMALL, err[0] ? err : "not fetched yet", SEP_X - 30);
        return;
    }
    constexpr int AXIS_W = 28;                       // hour labels
    const int x0 = AXIS_W, x1 = SEP_X - 12;
    const int colW = (x1 - x0) / CAL_DAYS;
    const int headY = BODY_Y + BAR_H + 2;            // "Today" / "Tomorrow", small
    const int allDayY = headY + 20;                  // all-day bars
    constexpr int BAR_ROW_H = 18, ALLDAY_ROWS = 2, NIGHT_LINES = 3;

    // how many all-day rows and night lines are needed (the same for both columns)
    int allDayRows = 0, nightLines = 0;
    for (int d = 0; d < CAL_DAYS; d++) {
        const int64_t ds = today + d * 86400LL;
        int nAll = 0, nNight = 0;
        for (int i = 0; i < a.n; i++) {
            const ac::Event& e = a.ev[i];
            if (e.allDay) { if (e.start < ds + 86400 && e.end > ds) nAll++; }
            else if (nightShown(e, ds, d, nowMin)) nNight++;
        }
        allDayRows = max(allDayRows, min(nAll, ALLDAY_ROWS));
        nightLines = max(nightLines, min(nNight, NIGHT_LINES));
    }
    const int gridY = allDayY + (allDayRows ? allDayRows * (BAR_ROW_H + 3) + 3 : 2);
    const int stripH = nightLines ? 6 + nightLines * (BAR_ROW_H + 3) : 0;
    const int gridBottom = BODY_BOTTOM - stripH;
    const int gridH = gridBottom - gridY;
    auto yOf = [&](int minutes) {
        return gridY + (int)((long)(minutes - DAY_FROM_H * 60) * gridH / ((DAY_TO_H - DAY_FROM_H) * 60));
    };

    // dotted hour lines, labels every two hours
    for (int h = DAY_FROM_H; h <= DAY_TO_H; h++) {
        const int y = yOf(h * 60);
        for (int x = x0; x < x1; x += 4) G->drawPixel(x, y, kBlack);
        if (h < DAY_TO_H && h % 2 == 0) {
            char hb[4];
            snprintf(hb, sizeof(hb), "%d", h);
            printRight(AXIS_W - 6, y + 14, F_SMALL, hb);
        }
    }

    for (int d = 0; d < CAL_DAYS; d++) {
        const int64_t ds = today + d * 86400LL;
        const int cx = x0 + d * colW;
        printAt(cx + 6, headY + 15, F_SMALL, d == 0 ? "Today" : "Tomorrow");
        if (d > 0) G->fillRect(cx - 1, headY, 2, BODY_BOTTOM - headY, kBlack);   // column separator

        // all-day events: black bars with white text
        int row = 0;
        for (int i = 0; i < a.n && row < ALLDAY_ROWS; i++) {
            const ac::Event& e = a.ev[i];
            if (!e.allDay || e.start >= ds + 86400 || e.end <= ds) continue;
            const int by = allDayY + row * (BAR_ROW_H + 3);
            G->fillRoundRect(cx + 3, by, colW - 6, BAR_ROW_H, 3, kBlack);
            G->setTextColor(kWhite);
            printFit(cx + 7, by + 14, F_SMALL, e.summary, colW - 14);
            G->setTextColor(kBlack);
            row++;
        }

        // day events as inverted boxes; overlapping ones share the width (lanes)
        int idx[ac::kMaxEvents], lane[ac::kMaxEvents], n = 0;
        for (int i = 0; i < a.n && n < ac::kMaxEvents; i++) {
            const ac::Event& e = a.ev[i];
            if (e.allDay || !onDay(e, ds) || ac::isNightEvent(e, ds, DAY_FROM_H, DAY_TO_H)) continue;
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
            const int64_t end = e.end > e.start ? e.end : e.start + 1800;
            const int sMin = max((int)((max(e.start, ds) - ds) / 60), DAY_FROM_H * 60);
            const int eMin = min((int)((min(end, ds + 86400) - ds) / 60), DAY_TO_H * 60);
            const int lw = (colW - 6) / lanes;
            const int bx = cx + 3 + lane[k] * lw;
            const int top = yOf(sMin) + 1;
            const int bottom = min(max(yOf(eMin) - 1, top + 26), gridBottom);
            const int h = bottom - top;
            G->fillRoundRect(bx, top, lw - 2, h, 3, kBlack);
            G->setTextColor(kWhite);
            const int avail = lw - 9;                                     // text area of the box
            if (h >= 46 && e.start >= ds && e.end > e.start) {        // room: start - end small above the title
                const ac::Civil st = ac::fromSec(e.start), en = ac::fromSec(e.end);
                char range[20];                                       // always spelled out: 15:00-17:00
                snprintf(range, sizeof(range), "%02d:%02d-%02d:%02d", st.hour, st.minute, en.hour, en.minute);
                if (textWidth(F_SMALL, range) > avail)                // narrow lane: without the leading zeros, still complete
                    snprintf(range, sizeof(range), "%d:%02d-%d:%02d", st.hour, st.minute, en.hour, en.minute);
                printFit(bx + 5, top + 19, F_SMALL, range, avail);
                printFit(bx + 5, top + 39, F_SMALL, e.summary, avail);
            } else {
                printFit(bx + 5, top + 20, F_SMALL, e.summary, avail);    // a little room above the title
            }
            G->setTextColor(kBlack);
        }

        // strip below the grid: early events and (from the afternoon) the evening ones, as black bars
        if (stripH) {
            G->fillRect(cx + 3, gridBottom + 1, colW - 6, 2, kBlack);
            int ny = gridBottom + 6;
            int shown = 0;
            for (int i = 0; i < a.n && shown < NIGHT_LINES; i++) {
                const ac::Event& e = a.ev[i];
                if (!nightShown(e, ds, d, nowMin)) continue;
                const ac::Civil st = ac::fromSec(max(e.start, ds));
                char line[80];
                snprintf(line, sizeof(line), "%02d:%02d %s", st.hour, st.minute, e.summary);
                G->fillRoundRect(cx + 3, ny, colW - 6, BAR_ROW_H, 3, kBlack);
                G->setTextColor(kWhite);
                printFit(cx + 7, ny + 14, F_SMALL, line, colW - 14);
                G->setTextColor(kBlack);
                ny += BAR_ROW_H + 3;
                shown++;
            }
        }
    }
}

// ── dashboard: the markdown document 1:1 (blank lines, headings, tasks with boxes / ticks, bullets, text) ──

int docLineHeight(ac::DocKind k) {
    switch (k) {
        case ac::DocKind::Blank: return 10;
        case ac::DocKind::H1: return 30;
        case ac::DocKind::H2: return 26;
        case ac::DocKind::H3: return 24;
        case ac::DocKind::Rule: return 12;
        default: return 22;
    }
}

void drawDashboard(int64_t) {
    const int x0 = SEP_X + 12, x1 = W - 10;
    headingBar(SEP_X, BODY_Y, W - SEP_X, "DASHBOARD");
    if (!S->src[clockdata::SRC_TODO].ok) {
        printAt(x0, BODY_Y + BAR_H + 30, F_TEXT_B, "Dashboard unavailable");
        const char* err = S->src[clockdata::SRC_TODO].error;
        printFit(x0, BODY_Y + BAR_H + 56, F_SMALL, err[0] ? err : "not fetched yet", x1 - x0);
        return;
    }
    int y = BODY_Y + BAR_H + 6;
    for (int i = 0; i < S->docCount; i++) {
        const ac::DocLine& l = S->doc[i];
        const int h = docLineHeight(l.kind);
        if (y + h > epdlayout::H - 2) break;                         // the document simply ends at the bottom edge
        const int ix = x0 + l.indent * 16;
        switch (l.kind) {
            case ac::DocKind::Blank:
                break;
            case ac::DocKind::H1:
                printFit(x0, y + 22, F_TEXT_B, l.text, x1 - x0);
                break;
            case ac::DocKind::H2:
            case ac::DocKind::H3:
                printFit(x0, y + 19, F_SMALL_B, l.text, x1 - x0);
                break;
            case ac::DocKind::Rule:
                G->fillRect(x0, y + 5, x1 - x0, 2, kBlack);
                break;
            case ac::DocKind::TaskOpen:
            case ac::DocKind::TaskDone: {
                for (int d = 0; d < 2; d++) G->drawRect(ix + d, y + 3 + d, 16 - 2 * d, 16 - 2 * d, kBlack);
                const bool done = l.kind == ac::DocKind::TaskDone;
                if (done) {                                                        // ticked: a check mark
                    for (int d = 0; d < 3; d++) {
                        G->drawLine(ix + 3, y + 11 + d, ix + 6, y + 15 + d, kBlack);
                        G->drawLine(ix + 6, y + 15 + d, ix + 15, y + 4 + d, kBlack);
                    }
                }
                printFit(ix + 24, y + 17, F_SMALL, l.text, x1 - (ix + 24));
                if (done) {                                                        // and struck through
                    const int w = textWidth(F_SMALL, l.text);
                    G->fillRect(ix + 24, y + 10, min(w, x1 - (ix + 24)), 2, kBlack);
                }
                break;
            }
            case ac::DocKind::Bullet:
                G->fillCircle(ix + 7, y + 11, 2, kBlack);
                printFit(ix + 18, y + 17, F_SMALL, l.text, x1 - (ix + 18));
                break;
            default:
                printFit(ix, y + 17, F_SMALL, l.text, x1 - ix);
                break;
        }
        y += h;
    }
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
    drawDashboard(now);
}

bool hasNightEvents(const clockdata::Data& snap, int64_t now) {
    return snap.agenda.hasNightEvents(ac::dayStart(now), CAL_DAYS, DAY_FROM_H, DAY_TO_H);
}

}  // namespace epdlayout
