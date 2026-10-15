#include "epd_layout.h"
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <stdio.h>
#include "../core/civil.h"
#include "../core/moon.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace epdlayout {

namespace {

constexpr uint16_t kBlack = 0x0000, kWhite = 0xFFFF;

constexpr int HEADER_H = 76;
constexpr int WX_Y = 80, WX_H = 100;
constexpr int BODY_Y = 186, BODY_BOTTOM = 440;
constexpr int SEP_X = 478;
constexpr int FOOTER_Y = 446;

Adafruit_GFX* G = nullptr;
const clockdata::Data* S = nullptr;
Options opt;

// ── text helpers (GFX fonts) ─────────────────────────────────────────────

void printAt(int x, int y, const GFXfont* font, const String& text) {
    G->setFont(font);
    G->setCursor(x, y);
    G->print(text);
}

int textWidth(const GFXfont* font, const String& text) {
    int16_t bx, by;
    uint16_t bw, bh;
    G->setFont(font);
    G->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    return (int)bw;
}

void printCentered(int cx, int y, const GFXfont* font, const String& text) {
    int16_t bx, by;
    uint16_t bw, bh;
    G->setFont(font);
    G->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    G->setCursor(cx - (int)bw / 2 - bx, y);
    G->print(text);
}

void printRight(int xRight, int y, const GFXfont* font, const String& text) {
    int16_t bx, by;
    uint16_t bw, bh;
    G->setFont(font);
    G->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    G->setCursor(xRight - (int)bw - bx, y);
    G->print(text);
}

String fit(const GFXfont* font, String text, int maxW) {
    if (textWidth(font, text) <= maxW) return text;
    while (text.length() > 1 && textWidth(font, text + "...") > maxW) text.remove(text.length() - 1);
    text.trim();
    return text + "...";
}

void degree(int x, int y, int r = 3) { G->drawCircle(x, y, r, kBlack); }

// "-3" + degree ring; returns the x after the ring
int printTemp(int x, int y, const GFXfont* font, int t10) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(t10 / 10.0f));
    printAt(x, y, font, buf);
    const int w = textWidth(font, buf);
    degree(x + w + 6, y - (font == &FreeSansBold24pt7b ? 26 : font == &FreeSansBold18pt7b ? 20 : 12), font == &FreeSansBold24pt7b ? 5 : 3);
    return x + w + 16;
}

// ── icons ────────────────────────────────────────────────────────────────

void drawSun(int cx, int cy, int r) {
    G->fillCircle(cx, cy, r, kBlack);
    for (int i = 0; i < 8; i++) {
        const float a = i * (float)M_PI / 4.0f;
        const int x1 = cx + (int)lroundf(cosf(a) * (r + 5)), y1 = cy + (int)lroundf(sinf(a) * (r + 5));
        const int x2 = cx + (int)lroundf(cosf(a) * (r + 12)), y2 = cy + (int)lroundf(sinf(a) * (r + 12));
        G->drawLine(x1, y1, x2, y2, kBlack);
        G->drawLine(x1 + 1, y1, x2 + 1, y2, kBlack);
    }
}

void drawMoonIcon(int cx, int cy, int r, double phaseFrac) {
    const double k = cos(2.0 * M_PI * phaseFrac);      // 1 new, -1 full
    const bool waxing = phaseFrac < 0.5;
    G->drawCircle(cx, cy, r, kBlack);
    for (int y = -r + 1; y < r; y++) {
        const double hw = sqrt((double)r * r - (double)y * y);
        int x0, x1;
        if (waxing) { x0 = (int)lround(k * hw); x1 = (int)floor(hw); }
        else { x0 = (int)ceil(-hw); x1 = (int)lround(-k * hw); }
        if (x1 > x0) G->drawFastHLine(cx + x0, cy + y, x1 - x0 + 1, kBlack);
    }
}

void drawCloud(int ox, int oy, int s /*size 64 box*/, bool outlineWhite = false) {
    auto shape = [&](uint16_t color, int grow) {
        G->fillCircle(ox + s * 22 / 64, oy + s * 38 / 64, s * 11 / 64 + grow, color);
        G->fillCircle(ox + s * 36 / 64, oy + s * 30 / 64, s * 15 / 64 + grow, color);
        G->fillCircle(ox + s * 50 / 64, oy + s * 39 / 64, s * 10 / 64 + grow, color);
        G->fillRoundRect(ox + s * 12 / 64 - grow, oy + s * 38 / 64 - grow, s * 46 / 64 + 2 * grow, s * 14 / 64 + 2 * grow, s * 7 / 64, color);
    };
    if (outlineWhite) shape(kWhite, 3);
    shape(kBlack, 0);
}

void drawWeatherIcon(int ox, int oy, ac::WxKind kind, bool day) {       // within a 64x64 box
    switch (kind) {
        case ac::WxKind::Clear:
            if (day) drawSun(ox + 32, oy + 32, 14);
            else { G->fillCircle(ox + 30, oy + 32, 17, kBlack); G->fillCircle(ox + 40, oy + 26, 15, kWhite); }
            break;
        case ac::WxKind::PartlyCloudy:
            if (day) drawSun(ox + 22, oy + 22, 9);
            else { G->fillCircle(ox + 20, oy + 22, 12, kBlack); G->fillCircle(ox + 27, oy + 17, 10, kWhite); }
            drawCloud(ox + 4, oy + 8, 60, true);
            break;
        case ac::WxKind::Cloudy:
            drawCloud(ox, oy, 64);
            break;
        case ac::WxKind::Fog:
            for (int i = 0; i < 4; i++) G->fillRoundRect(ox + 6 + (i % 2) * 8, oy + 14 + i * 11, 44, 4, 2, kBlack);
            break;
        case ac::WxKind::Drizzle:
            drawCloud(ox, oy - 6, 64);
            for (int i = 0; i < 4; i++) G->fillCircle(ox + 16 + i * 11, oy + 46 + (i % 2) * 6, 2, kBlack);
            break;
        case ac::WxKind::Rain:
        case ac::WxKind::Showers:
            drawCloud(ox, oy - 6, 64);
            for (int i = 0; i < 4; i++) {
                const int x = ox + 16 + i * 11, y = oy + 44 + (i % 2) * 5;
                G->drawLine(x, y, x - 4, y + 11, kBlack);
                G->drawLine(x + 1, y, x - 3, y + 11, kBlack);
            }
            break;
        case ac::WxKind::Snow:
            drawCloud(ox, oy - 6, 64);
            for (int i = 0; i < 3; i++) {
                const int x = ox + 18 + i * 14, y = oy + 52 + (i % 2) * 4;
                G->drawLine(x - 4, y, x + 4, y, kBlack);
                G->drawLine(x, y - 4, x, y + 4, kBlack);
                G->drawLine(x - 3, y - 3, x + 3, y + 3, kBlack);
                G->drawLine(x - 3, y + 3, x + 3, y - 3, kBlack);
            }
            break;
        case ac::WxKind::Thunder:
            drawCloud(ox, oy - 8, 64);
            G->fillTriangle(ox + 34, oy + 40, ox + 22, oy + 58, ox + 32, oy + 56, kBlack);
            G->fillTriangle(ox + 30, oy + 54, ox + 40, oy + 42, ox + 28, oy + 63, kBlack);
            break;
        default:
            printCentered(ox + 32, oy + 48, &FreeSansBold24pt7b, "?");
            break;
    }
}

void drawSmallSun(int x, int y, bool rise) {         // 22x14 horizon icon with an arrow
    G->drawFastHLine(x, y + 12, 22, kBlack);
    G->fillCircle(x + 8, y + 12, 6, kBlack);
    G->fillRect(x, y + 13, 22, 3, kWhite);
    if (rise) { G->fillTriangle(x + 19, y, x + 15, y + 6, x + 23, y + 6, kBlack); }
    else { G->fillTriangle(x + 19, y + 7, x + 15, y + 1, x + 23, y + 1, kBlack); }
}

// ── header ───────────────────────────────────────────────────────────────

void drawHeader(int64_t now) {
    const ac::Civil c = ac::fromSec(now);
    const int wd = ac::weekday(ac::daysOf(now));
    char date[48];
    snprintf(date, sizeof(date), "%s, %d %s %d", ac::weekdayName(wd, false), c.day, ac::monthName(c.month, false), c.year);
    const GFXfont* fonts[] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b};
    const GFXfont* f = fonts[2];
    for (const GFXfont* cand : fonts) if (textWidth(cand, date) <= 510) { f = cand; break; }
    printAt(10, f == &FreeSansBold24pt7b ? 50 : f == &FreeSansBold18pt7b ? 46 : 40, f, date);
    char week[16];
    snprintf(week, sizeof(week), "Wk %d", ac::isoWeek(c.year, c.month, c.day));
    printAt(12, 69, &FreeSans9pt7b, week);

    // moon block on the right: disc, phase name, illumination and the next full / new moon
    const ac::MoonInfo m = ac::moonAt(ac::localToUtc(now));
    const int mx = 570;
    drawMoonIcon(mx, 38, 24, m.age / ac::kSynodicMonth);
    const int tx = mx + 36, tw = W - 10 - tx;
    printAt(tx, 32, &FreeSansBold12pt7b, fit(&FreeSansBold12pt7b, ac::moonPhaseName(m.phase), tw));
    const int64_t nextFull = ac::nextMoonEvent(ac::localToUtc(now), true);
    const int64_t nextNew = ac::nextMoonEvent(ac::localToUtc(now), false);
    const bool fullFirst = nextFull < nextNew;
    const int days = (int)((((fullFirst ? nextFull : nextNew) - ac::localToUtc(now)) + 43200) / 86400);
    char moon[48];
    snprintf(moon, sizeof(moon), "%d %%  %s in %d d", (int)lroundf(m.illum * 100.0), fullFirst ? "Full moon" : "New moon", days);
    printAt(tx, 56, &FreeSans9pt7b, fit(&FreeSans9pt7b, moon, tw));
    G->fillRect(0, HEADER_H - 2, W, 3, kBlack);
}

// ── weather band ─────────────────────────────────────────────────────────

void drawWeatherBand(int64_t now) {
    const ac::Weather& w = S->weather;
    if (!w.valid) {
        printAt(14, WX_Y + 56, &FreeSans12pt7b, "No weather data - fetch failed or not done yet");
        G->fillRect(0, WX_Y + WX_H, W, 2, kBlack);
        return;
    }
    const int di = (int)((ac::dayStart(now) - w.day0) / 86400);
    const ac::DayInfo day = (di >= 0 && di < 3) ? w.day[di] : ac::DayInfo();

    // current conditions (as of the fetch, i.e. the morning refresh)
    drawWeatherIcon(10, WX_Y + 8, ac::kindFromWmo(w.wmo), w.isDay);
    printTemp(86, WX_Y + 50, &FreeSansBold24pt7b, w.temp10);
    printAt(86, WX_Y + 70, &FreeSans9pt7b, ac::kindName(ac::kindFromWmo(w.wmo)));
    char line[64];
    snprintf(line, sizeof(line), "%d / %d   Rain %u %%", (int)lroundf(day.tMin10 / 10.0f), (int)lroundf(day.tMax10 / 10.0f), day.popMax);
    printAt(86, WX_Y + 87, &FreeSans9pt7b, fit(&FreeSans9pt7b, line, 170));

    // sunrise / sunset
    const int sx = 262;
    drawSmallSun(sx, WX_Y + 8, true);
    drawSmallSun(sx, WX_Y + 44, false);
    const ac::Civil sr = ac::fromSec(day.sunrise), ss = ac::fromSec(day.sunset);
    char t[12];
    snprintf(t, sizeof(t), "%02d:%02d", sr.hour, sr.minute);
    printAt(sx + 34, WX_Y + 27, &FreeSansBold18pt7b, day.sunrise ? t : "--:--");
    snprintf(t, sizeof(t), "%02d:%02d", ss.hour, ss.minute);
    printAt(sx + 34, WX_Y + 63, &FreeSansBold18pt7b, day.sunset ? t : "--:--");
    if (day.sunrise && day.sunset > day.sunrise) {
        const int len = (int)((day.sunset - day.sunrise) / 60);
        snprintf(line, sizeof(line), "Daylight %d h %02d min", len / 60, len % 60);
        printAt(sx, WX_Y + 87, &FreeSans9pt7b, line);
    }

    // hourly graph 06..22 of today: temperature line, rain bars
    const int gx = 505, gy = WX_Y + 8, gw = 280, gh = 66;
    const int h0 = 6, h1 = 22;
    int tMin = 1000, tMax = -1000;
    for (int h = h0; h <= h1; h++) {
        const int idx = di * 24 + h;
        if (idx < 0 || idx >= ac::kHours) continue;
        tMin = min(tMin, (int)w.hTemp10[idx]);
        tMax = max(tMax, (int)w.hTemp10[idx]);
    }
    if (tMax >= tMin) {
        if (tMax - tMin < 40) { tMax += 20; tMin -= 20; }
        auto px = [&](int h) { return gx + (h - h0) * gw / (h1 - h0); };
        auto py = [&](int t10) { return gy + gh - (t10 - tMin) * gh / (tMax - tMin); };
        G->drawFastHLine(gx, gy + gh + 2, gw, kBlack);
        for (int h = h0; h <= h1; h += 4) {
            G->drawFastVLine(px(h), gy + gh + 2, 5, kBlack);
            char hb[4];
            snprintf(hb, sizeof(hb), "%d", h);
            printCentered(px(h), gy + gh + 20, &FreeSans9pt7b, hb);
        }
        for (int h = h0; h < h1; h++) {                        // rain probability as bars at the bottom
            const int idx = di * 24 + h;
            if (idx < 0 || idx >= ac::kHours) continue;
            const int bar = w.hPop[idx] * 22 / 100;
            if (bar > 0) G->fillRect(px(h) + 1, gy + gh - bar, max(2, gw / (h1 - h0) - 2), bar, kBlack);
        }
        int prevX = 0, prevY = 0;
        for (int h = h0; h <= h1; h++) {
            const int idx = di * 24 + h;
            if (idx < 0 || idx >= ac::kHours) continue;
            const int xx = px(h), yy = py(w.hTemp10[idx]);
            if (h > h0) { G->drawLine(prevX, prevY, xx, yy, kBlack); G->drawLine(prevX, prevY - 1, xx, yy - 1, kBlack); }
            prevX = xx; prevY = yy;
        }
        // min / max labels
        char lb[8];
        snprintf(lb, sizeof(lb), "%d", (int)lroundf(tMax / 10.0f));
        printAt(gx - 34, gy + 10, &FreeSans9pt7b, lb);
        snprintf(lb, sizeof(lb), "%d", (int)lroundf(tMin / 10.0f));
        printAt(gx - 34, gy + gh, &FreeSans9pt7b, lb);
        // sun markers on the time axis
        auto marker = [&](int64_t sec) {
            if (!sec) return;
            const ac::Civil c = ac::fromSec(sec);
            const float hf = c.hour + c.minute / 60.0f;
            if (hf < h0 || hf > h1) return;
            const int xx = gx + (int)((hf - h0) * gw / (h1 - h0));
            G->fillTriangle(xx, gy + gh - 1, xx - 4, gy + gh + 7, xx + 4, gy + gh + 7, kBlack);
        };
        marker(day.sunrise);
        marker(day.sunset);
    }
    G->fillRect(0, WX_Y + WX_H, W, 2, kBlack);
}

// ── calendar ─────────────────────────────────────────────────────────────

void drawCalendar(int64_t now) {
    const ac::Agenda& a = S->agenda;
    const int64_t today = ac::dayStart(now);
    G->fillRect(0, BODY_Y - 2, SEP_X - 8, 24, kBlack);
    G->setTextColor(kWhite);
    printAt(10, BODY_Y + 16, &FreeSansBold12pt7b, "EVENTS");
    G->setTextColor(kBlack);
    if (!S->src[clockdata::SRC_CALENDAR].ok) {
        printAt(10, BODY_Y + 54, &FreeSans12pt7b, "Calendar unavailable");
        printAt(10, BODY_Y + 78, &FreeSans9pt7b, fit(&FreeSans9pt7b, S->src[clockdata::SRC_CALENDAR].error[0] ? S->src[clockdata::SRC_CALENDAR].error : "not fetched yet", SEP_X - 30));
        return;
    }
    int y = BODY_Y + 26;
    int shown = 0, hidden = 0;
    bool anyToday = false;
    for (int day = 0; day < 7; day++) {
        const int64_t ds = today + (int64_t)day * 86400;
        int idx[ac::kMaxEvents];
        int n = 0;
        for (int i = 0; i < a.n; i++) {
            const int64_t end = a.ev[i].end > a.ev[i].start ? a.ev[i].end : a.ev[i].start + 1;
            if (a.ev[i].start < ds + 86400 && end > ds && n < ac::kMaxEvents) idx[n++] = i;
        }
        if (day == 0) anyToday = n > 0;
        if (n == 0 && day != 0) continue;
        const int need = 22 + (n ? n : 1) * 24;
        if (y + need > BODY_BOTTOM) {
            for (int j = day; j < 7; j++) hidden += a.countOnDay(today + (int64_t)j * 86400) > 0 ? 1 : 0;
            break;
        }
        // day header
        char label[32];
        ac::dayLabel(ds, today, label, sizeof(label));
        const ac::Civil c = ac::fromSec(ds);
        char full[48];
        if (day <= 1) snprintf(full, sizeof(full), "%s  %s %d.%d.", label, ac::weekdayName(ac::weekday(ac::daysOf(ds)), true), c.day, c.month);
        else snprintf(full, sizeof(full), "%s", label);
        G->drawFastHLine(8, y, SEP_X - 24, kBlack);
        printAt(10, y + 18, day == 0 ? &FreeSansBold12pt7b : &FreeSansBold9pt7b, full);
        y += 24;
        if (n == 0) {
            printAt(24, y + 14, &FreeSans9pt7b, "no events");
            y += 24;
            continue;
        }
        for (int k = 0; k < n; k++) {
            const ac::Event& e = a.ev[idx[k]];
            char tl[24];
            if (e.allDay) snprintf(tl, sizeof(tl), "all day");
            else {
                const ac::Civil s = ac::fromSec(max(e.start, ds));
                snprintf(tl, sizeof(tl), "%02d:%02d", s.hour, s.minute);
            }
            printAt(14, y + 16, &FreeSansBold9pt7b, tl);
            String title = e.summary;
            if (e.location[0]) title += String("  @ ") + e.location;
            printAt(104, y + 17, &FreeSans12pt7b, fit(&FreeSans12pt7b, title, SEP_X - 8 - 112));
            y += 24;
            shown++;
        }
    }
    (void)anyToday;
    if (hidden > 0) {
        printRight(SEP_X - 14, BODY_BOTTOM + 2, &FreeSans9pt7b, String("+ more days"));
    }
    (void)shown;
}

// ── to-do ────────────────────────────────────────────────────────────────

void drawTodo(int64_t now) {
    const int x0 = SEP_X + 10;
    const int x1 = W - 8;
    const int64_t today = ac::dayStart(now);
    G->fillRect(SEP_X, BODY_Y - 2, W - SEP_X, 24, kBlack);
    G->setTextColor(kWhite);
    printAt(x0, BODY_Y + 16, &FreeSansBold12pt7b, "TO-DO");
    G->setTextColor(kBlack);
    G->drawFastVLine(SEP_X - 4, BODY_Y - 2, BODY_BOTTOM - BODY_Y + 4, kBlack);
    if (!S->src[clockdata::SRC_TODO].ok) {
        printAt(x0, BODY_Y + 54, &FreeSans9pt7b, "To-do unavailable");
        const char* err = S->src[clockdata::SRC_TODO].error;
        printAt(x0, BODY_Y + 76, &FreeSans9pt7b, fit(&FreeSans9pt7b, err[0] ? err : "not fetched yet", x1 - x0));
        return;
    }
    int y = BODY_Y + 28;
    int shown = 0;
    for (int i = 0; i < S->taskCount; i++) {
        if (y + 26 > BODY_BOTTOM) break;
        const ac::Task& t = S->tasks[i];
        char tag[16] = "";
        if (t.hasDate) {
            const ac::Civil c = ac::fromSec(t.date);
            if (t.date < today) snprintf(tag, sizeof(tag), "!%d.%d.", c.day, c.month);
            else if (t.date == today) snprintf(tag, sizeof(tag), "today");
            else if (t.date == today + 86400) snprintf(tag, sizeof(tag), "tomorrow");
            else snprintf(tag, sizeof(tag), "%d.%d.", c.day, c.month);
        }
        const bool overdue = t.hasDate && t.date < today;
        G->drawRect(x0, y + 3, 15, 15, kBlack);
        if (overdue) G->fillRect(x0 + 4, y + 7, 7, 7, kBlack);
        int right = x1;
        if (tag[0]) {
            printRight(x1, y + 17, &FreeSansBold9pt7b, tag);
            right = x1 - textWidth(&FreeSansBold9pt7b, tag) - 8;
        }
        const GFXfont* f = t.priority >= 2 ? &FreeSansBold9pt7b : &FreeSans9pt7b;
        printAt(x0 + 24, y + 17, f, fit(f, t.text, right - (x0 + 24)));
        y += 26;
        shown++;
    }
    if (shown < S->taskCount) {
        char more[24];
        snprintf(more, sizeof(more), "+ %d more", S->taskCount - shown);
        printAt(x0 + 24, BODY_BOTTOM + 2, &FreeSans9pt7b, more);
    }
    if (S->taskCount == 0) printAt(x0 + 24, y + 17, &FreeSans9pt7b, "all done");
}

// ── footer ───────────────────────────────────────────────────────────────

void hm(int64_t sec, char* out, size_t cap) {
    if (!sec) { snprintf(out, cap, "--:--"); return; }
    const ac::Civil c = ac::fromSec(sec);
    snprintf(out, cap, "%02d:%02d", c.hour, c.minute);
}

void drawFooter(int64_t now) {
    G->fillRect(0, FOOTER_Y - 2, W, 2, kBlack);
    const ac::Civil c = ac::fromSec(now);
    char a[8], b[8], wx[8];
    hm(S->src[clockdata::SRC_CALENDAR].at, a, sizeof(a));
    hm(S->src[clockdata::SRC_TODO].at, b, sizeof(b));
    hm(S->src[clockdata::SRC_WEATHER].at, wx, sizeof(wx));
    char line[120];
    snprintf(line, sizeof(line), "Updated %d.%d. %02d:%02d  |  Events %s%s  To-do %s%s  Weather %s%s", c.day, c.month, c.hour, c.minute, a,
             S->src[clockdata::SRC_CALENDAR].ok ? "" : " !", b, S->src[clockdata::SRC_TODO].ok ? "" : " !", wx,
             S->src[clockdata::SRC_WEATHER].ok ? "" : " !");
    printAt(10, FOOTER_Y + 22, &FreeSans9pt7b, line);
    printRight(W - 10, FOOTER_Y + 22, &FreeSans9pt7b, String(opt.place));
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
    drawTodo(now);
    drawFooter(now);
}

}  // namespace epdlayout
