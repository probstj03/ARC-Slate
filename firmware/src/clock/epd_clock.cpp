#include "epd_clock.h"
#include <ArcCore.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Preferences.h>
#include <math.h>
#include "clock_config.h"
#include "core/agenda.h"
#include "core/civil.h"
#include "core/moon.h"
#include "core/todo.h"
#include "core/weather.h"
#include "data_store.h"
#include "epd_driver.h"
#include "lid.h"
#include "spi_bus.h"

using epddrv::printAt;
using epddrv::printCentered;
using epddrv::printRight;
using epddrv::textWidth;

namespace {

constexpr int W = epddrv::W, H = epddrv::H;
constexpr int HEADER_H = 76;
constexpr int WX_Y = 80, WX_H = 100;
constexpr int BODY_Y = 186, BODY_BOTTOM = 440;
constexpr int SEP_X = 478;
constexpr int FOOTER_Y = 446;

epddrv::Display& d = epddrv::display();

clockdata::Data* s_snap = nullptr;          // copy of the data taken for one drawing pass
TaskHandle_t s_task = nullptr;
volatile bool s_requested = false, s_forced = false;
int64_t s_lastDay = 0;                      // local day number of the last refresh
int64_t s_lastRefreshSec = 0;
uint32_t s_calRevDrawn = 0;
uint32_t s_refreshCount = 0;
Preferences s_prefs;

int64_t nowSec() { return arc::clock::nowNs() / 1000000000LL; }

// ── text helpers ─────────────────────────────────────────────────────────

String fit(const GFXfont* font, String text, int maxW) {
    if (textWidth(font, text) <= maxW) return text;
    while (text.length() > 1 && textWidth(font, text + "...") > maxW) text.remove(text.length() - 1);
    text.trim();
    return text + "...";
}

void degree(int x, int y, int r = 3) { d.drawCircle(x, y, r, GxEPD_BLACK); }

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
    d.fillCircle(cx, cy, r, GxEPD_BLACK);
    for (int i = 0; i < 8; i++) {
        const float a = i * (float)M_PI / 4.0f;
        const int x1 = cx + (int)lroundf(cosf(a) * (r + 5)), y1 = cy + (int)lroundf(sinf(a) * (r + 5));
        const int x2 = cx + (int)lroundf(cosf(a) * (r + 12)), y2 = cy + (int)lroundf(sinf(a) * (r + 12));
        d.drawLine(x1, y1, x2, y2, GxEPD_BLACK);
        d.drawLine(x1 + 1, y1, x2 + 1, y2, GxEPD_BLACK);
    }
}

void drawMoonIcon(int cx, int cy, int r, double phaseFrac) {
    const double k = cos(2.0 * M_PI * phaseFrac);      // 1 new, -1 full
    const bool waxing = phaseFrac < 0.5;
    d.drawCircle(cx, cy, r, GxEPD_BLACK);
    for (int y = -r + 1; y < r; y++) {
        const double hw = sqrt((double)r * r - (double)y * y);
        int x0, x1;
        if (waxing) { x0 = (int)lround(k * hw); x1 = (int)floor(hw); }
        else { x0 = (int)ceil(-hw); x1 = (int)lround(-k * hw); }
        if (x1 > x0) d.drawFastHLine(cx + x0, cy + y, x1 - x0 + 1, GxEPD_BLACK);
    }
}

void drawCloud(int ox, int oy, int s /*size 64 box*/, bool outlineWhite = false) {
    auto shape = [&](uint16_t color, int grow) {
        d.fillCircle(ox + s * 22 / 64, oy + s * 38 / 64, s * 11 / 64 + grow, color);
        d.fillCircle(ox + s * 36 / 64, oy + s * 30 / 64, s * 15 / 64 + grow, color);
        d.fillCircle(ox + s * 50 / 64, oy + s * 39 / 64, s * 10 / 64 + grow, color);
        d.fillRoundRect(ox + s * 12 / 64 - grow, oy + s * 38 / 64 - grow, s * 46 / 64 + 2 * grow, s * 14 / 64 + 2 * grow, s * 7 / 64, color);
    };
    if (outlineWhite) shape(GxEPD_WHITE, 3);
    shape(GxEPD_BLACK, 0);
}

void drawWeatherIcon(int ox, int oy, ac::WxKind kind, bool day) {       // within a 64x64 box
    switch (kind) {
        case ac::WxKind::Clear:
            if (day) drawSun(ox + 32, oy + 32, 14);
            else { d.fillCircle(ox + 30, oy + 32, 17, GxEPD_BLACK); d.fillCircle(ox + 40, oy + 26, 15, GxEPD_WHITE); }
            break;
        case ac::WxKind::PartlyCloudy:
            if (day) drawSun(ox + 22, oy + 22, 9);
            else { d.fillCircle(ox + 20, oy + 22, 12, GxEPD_BLACK); d.fillCircle(ox + 27, oy + 17, 10, GxEPD_WHITE); }
            drawCloud(ox + 4, oy + 8, 60, true);
            break;
        case ac::WxKind::Cloudy:
            drawCloud(ox, oy, 64);
            break;
        case ac::WxKind::Fog:
            for (int i = 0; i < 4; i++) d.fillRoundRect(ox + 6 + (i % 2) * 8, oy + 14 + i * 11, 44, 4, 2, GxEPD_BLACK);
            break;
        case ac::WxKind::Drizzle:
            drawCloud(ox, oy - 6, 64);
            for (int i = 0; i < 4; i++) d.fillCircle(ox + 16 + i * 11, oy + 46 + (i % 2) * 6, 2, GxEPD_BLACK);
            break;
        case ac::WxKind::Rain:
        case ac::WxKind::Showers:
            drawCloud(ox, oy - 6, 64);
            for (int i = 0; i < 4; i++) {
                const int x = ox + 16 + i * 11, y = oy + 44 + (i % 2) * 5;
                d.drawLine(x, y, x - 4, y + 11, GxEPD_BLACK);
                d.drawLine(x + 1, y, x - 3, y + 11, GxEPD_BLACK);
            }
            break;
        case ac::WxKind::Snow:
            drawCloud(ox, oy - 6, 64);
            for (int i = 0; i < 3; i++) {
                const int x = ox + 18 + i * 14, y = oy + 52 + (i % 2) * 4;
                d.drawLine(x - 4, y, x + 4, y, GxEPD_BLACK);
                d.drawLine(x, y - 4, x, y + 4, GxEPD_BLACK);
                d.drawLine(x - 3, y - 3, x + 3, y + 3, GxEPD_BLACK);
                d.drawLine(x - 3, y + 3, x + 3, y - 3, GxEPD_BLACK);
            }
            break;
        case ac::WxKind::Thunder:
            drawCloud(ox, oy - 8, 64);
            d.fillTriangle(ox + 34, oy + 40, ox + 22, oy + 58, ox + 32, oy + 56, GxEPD_BLACK);
            d.fillTriangle(ox + 30, oy + 54, ox + 40, oy + 42, ox + 28, oy + 63, GxEPD_BLACK);
            break;
        default:
            printCentered(ox + 32, oy + 48, &FreeSansBold24pt7b, "?");
            break;
    }
}

void drawSmallSun(int x, int y, bool rise) {         // 22x14 horizon icon with an arrow
    d.drawFastHLine(x, y + 12, 22, GxEPD_BLACK);
    d.fillCircle(x + 8, y + 12, 6, GxEPD_BLACK);
    d.fillRect(x, y + 13, 22, 3, GxEPD_WHITE);
    if (rise) { d.fillTriangle(x + 19, y, x + 15, y + 6, x + 23, y + 6, GxEPD_BLACK); }
    else { d.fillTriangle(x + 19, y + 7, x + 15, y + 1, x + 23, y + 1, GxEPD_BLACK); }
}

// ── header ───────────────────────────────────────────────────────────────

void drawHeader(int64_t now) {
    const ac::Civil c = ac::fromSec(now);
    const int wd = ac::weekday(ac::daysOf(now));
    char date[48];
    snprintf(date, sizeof(date), "%s, %d. %s %d", ac::weekdayName(wd, false), c.day, ac::monthName(c.month, false), c.year);
    const GFXfont* fonts[] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b};
    const GFXfont* f = fonts[2];
    for (const GFXfont* cand : fonts) if (textWidth(cand, date) <= 540) { f = cand; break; }
    printAt(10, f == &FreeSansBold24pt7b ? 50 : f == &FreeSansBold18pt7b ? 46 : 40, f, date);
    char week[16];
    snprintf(week, sizeof(week), "KW %d", ac::isoWeek(c.year, c.month, c.day));
    printAt(10, 70, &FreeSans9pt7b, week);

    // moon block on the right
    const ac::MoonInfo m = ac::moonAt(ac::localToUtc(now));
    drawMoonIcon(W - 196, 38, 24, m.age / ac::kSynodicMonth);
    printAt(W - 160, 28, &FreeSansBold12pt7b, ac::moonPhaseName(m.phase) + 0);
    char moon[40];
    const int64_t nextFull = ac::nextMoonEvent(ac::localToUtc(now), true);
    const int64_t nextNew = ac::nextMoonEvent(ac::localToUtc(now), false);
    const bool fullFirst = nextFull < nextNew;
    const int days = (int)((((fullFirst ? nextFull : nextNew) - ac::localToUtc(now)) + 43200) / 86400);
    snprintf(moon, sizeof(moon), "%d %% - %s in %d T", (int)lroundf(m.illum * 100.0), fullFirst ? "Vollmond" : "Neumond", days);
    printAt(W - 160, 52, &FreeSans9pt7b, moon);
    d.fillRect(0, HEADER_H - 2, W, 3, GxEPD_BLACK);
}

// ── weather band ─────────────────────────────────────────────────────────

void drawWeatherBand(int64_t now) {
    const ClockConfig cfg = clockcfg::copy();
    const ac::Weather& w = s_snap->weather;
    if (!w.valid) {
        printAt(14, WX_Y + 56, &FreeSans12pt7b, "Kein Wetter - Abruf fehlgeschlagen oder noch nicht erfolgt");
        d.fillRect(0, WX_Y + WX_H, W, 2, GxEPD_BLACK);
        return;
    }
    const int di = (int)((ac::dayStart(now) - w.day0) / 86400);
    const ac::DayInfo day = (di >= 0 && di < 3) ? w.day[di] : ac::DayInfo();

    // current conditions (as of the fetch, i.e. the morning refresh)
    drawWeatherIcon(10, WX_Y + 14, ac::kindFromWmo(w.wmo), w.isDay);
    const int x = printTemp(86, WX_Y + 58, &FreeSansBold24pt7b, w.temp10);
    (void)x;
    printAt(86, WX_Y + 80, &FreeSans9pt7b, ac::kindName(ac::kindFromWmo(w.wmo)));
    char line[64];
    snprintf(line, sizeof(line), "%d / %d   Regen %u %%", (int)lroundf(day.tMin10 / 10.0f), (int)lroundf(day.tMax10 / 10.0f), day.popMax);
    printAt(86, WX_Y + 96, &FreeSans9pt7b, line);

    // sunrise / sunset
    const int sx = 262;
    drawSmallSun(sx, WX_Y + 14, true);
    drawSmallSun(sx, WX_Y + 48, false);
    const ac::Civil sr = ac::fromSec(day.sunrise), ss = ac::fromSec(day.sunset);
    char t[12];
    snprintf(t, sizeof(t), "%02d:%02d", sr.hour, sr.minute);
    printAt(sx + 34, WX_Y + 30, &FreeSansBold18pt7b, day.sunrise ? t : "--:--");
    snprintf(t, sizeof(t), "%02d:%02d", ss.hour, ss.minute);
    printAt(sx + 34, WX_Y + 64, &FreeSansBold18pt7b, day.sunset ? t : "--:--");
    if (day.sunrise && day.sunset > day.sunrise) {
        const int len = (int)((day.sunset - day.sunrise) / 60);
        snprintf(line, sizeof(line), "Tag %d h %02d min", len / 60, len % 60);
        printAt(sx, WX_Y + 90, &FreeSans9pt7b, line);
    }
    snprintf(line, sizeof(line), "%s   Wind %u km/h", cfg.place.c_str(), w.windKmh);
    printAt(sx, WX_Y + 106 - 2, &FreeSans9pt7b, fit(&FreeSans9pt7b, line, 210));

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
        d.drawFastHLine(gx, gy + gh + 2, gw, GxEPD_BLACK);
        for (int h = h0; h <= h1; h += 4) {
            d.drawFastVLine(px(h), gy + gh + 2, 5, GxEPD_BLACK);
            char hb[4];
            snprintf(hb, sizeof(hb), "%d", h);
            printCentered(px(h), gy + gh + 20, &FreeSans9pt7b, hb);
        }
        for (int h = h0; h < h1; h++) {                        // rain probability as bars at the bottom
            const int idx = di * 24 + h;
            if (idx < 0 || idx >= ac::kHours) continue;
            const int bar = w.hPop[idx] * 22 / 100;
            if (bar > 0) d.fillRect(px(h) + 1, gy + gh - bar, max(2, gw / (h1 - h0) - 2), bar, GxEPD_BLACK);
        }
        int prevX = 0, prevY = 0;
        for (int h = h0; h <= h1; h++) {
            const int idx = di * 24 + h;
            if (idx < 0 || idx >= ac::kHours) continue;
            const int xx = px(h), yy = py(w.hTemp10[idx]);
            if (h > h0) { d.drawLine(prevX, prevY, xx, yy, GxEPD_BLACK); d.drawLine(prevX, prevY - 1, xx, yy - 1, GxEPD_BLACK); }
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
            d.fillTriangle(xx, gy + gh - 1, xx - 4, gy + gh + 7, xx + 4, gy + gh + 7, GxEPD_BLACK);
        };
        marker(day.sunrise);
        marker(day.sunset);
    }
    d.fillRect(0, WX_Y + WX_H, W, 2, GxEPD_BLACK);
}

// ── calendar ─────────────────────────────────────────────────────────────

void drawCalendar(int64_t now) {
    const ac::Agenda& a = s_snap->agenda;
    const int64_t today = ac::dayStart(now);
    d.fillRect(0, BODY_Y - 2, SEP_X - 8, 24, GxEPD_BLACK);
    d.setTextColor(GxEPD_WHITE);
    printAt(10, BODY_Y + 16, &FreeSansBold12pt7b, "TERMINE");
    d.setTextColor(GxEPD_BLACK);
    if (!s_snap->src[clockdata::SRC_CALENDAR].ok) {
        printAt(10, BODY_Y + 54, &FreeSans12pt7b, "Kalender nicht verfuegbar");
        printAt(10, BODY_Y + 78, &FreeSans9pt7b, fit(&FreeSans9pt7b, s_snap->src[clockdata::SRC_CALENDAR].error[0] ? s_snap->src[clockdata::SRC_CALENDAR].error : "noch nicht abgerufen", SEP_X - 30));
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
        d.drawFastHLine(8, y, SEP_X - 24, GxEPD_BLACK);
        printAt(10, y + 18, day == 0 ? &FreeSansBold12pt7b : &FreeSansBold9pt7b, full);
        y += 24;
        if (n == 0) {
            printAt(24, y + 14, &FreeSans9pt7b, "keine Termine");
            y += 24;
            continue;
        }
        for (int k = 0; k < n; k++) {
            const ac::Event& e = a.ev[idx[k]];
            char tl[24];
            if (e.allDay) snprintf(tl, sizeof(tl), "ganztags");
            else {
                const ac::Civil s = ac::fromSec(max(e.start, ds));
                snprintf(tl, sizeof(tl), "%02d:%02d", s.hour, s.minute);
            }
            printAt(14, y + 16, &FreeSansBold9pt7b, tl);
            String title = e.summary;
            if (e.location[0]) title += String("  @ ") + e.location;
            printAt(92, y + 17, &FreeSans12pt7b, fit(&FreeSans12pt7b, title, SEP_X - 8 - 100));
            y += 24;
            shown++;
        }
    }
    (void)anyToday;
    if (hidden > 0) {
        printRight(SEP_X - 14, BODY_BOTTOM + 2, &FreeSans9pt7b, String("+ weitere Tage"));
    }
    (void)shown;
}

// ── to-do ────────────────────────────────────────────────────────────────

void drawTodo(int64_t now) {
    const int x0 = SEP_X + 10;
    const int x1 = W - 8;
    const int64_t today = ac::dayStart(now);
    d.fillRect(SEP_X, BODY_Y - 2, W - SEP_X, 24, GxEPD_BLACK);
    d.setTextColor(GxEPD_WHITE);
    printAt(x0, BODY_Y + 16, &FreeSansBold12pt7b, "TO-DO");
    d.setTextColor(GxEPD_BLACK);
    d.drawFastVLine(SEP_X - 4, BODY_Y - 2, BODY_BOTTOM - BODY_Y + 4, GxEPD_BLACK);
    if (!s_snap->src[clockdata::SRC_TODO].ok) {
        printAt(x0, BODY_Y + 54, &FreeSans9pt7b, "To-Do nicht verfuegbar");
        const char* err = s_snap->src[clockdata::SRC_TODO].error;
        printAt(x0, BODY_Y + 76, &FreeSans9pt7b, fit(&FreeSans9pt7b, err[0] ? err : "noch nicht abgerufen", x1 - x0));
        return;
    }
    int y = BODY_Y + 28;
    int shown = 0;
    for (int i = 0; i < s_snap->taskCount; i++) {
        if (y + 26 > BODY_BOTTOM) break;
        const ac::Task& t = s_snap->tasks[i];
        char tag[16] = "";
        if (t.hasDate) {
            const ac::Civil c = ac::fromSec(t.date);
            if (t.date < today) snprintf(tag, sizeof(tag), "!%d.%d.", c.day, c.month);
            else if (t.date == today) snprintf(tag, sizeof(tag), "heute");
            else if (t.date == today + 86400) snprintf(tag, sizeof(tag), "morgen");
            else snprintf(tag, sizeof(tag), "%d.%d.", c.day, c.month);
        }
        const bool overdue = t.hasDate && t.date < today;
        d.drawRect(x0, y + 3, 15, 15, GxEPD_BLACK);
        if (overdue) d.fillRect(x0 + 4, y + 7, 7, 7, GxEPD_BLACK);
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
    if (shown < s_snap->taskCount) {
        char more[24];
        snprintf(more, sizeof(more), "+ %d weitere", s_snap->taskCount - shown);
        printAt(x0 + 24, BODY_BOTTOM + 2, &FreeSans9pt7b, more);
    }
    if (s_snap->taskCount == 0) printAt(x0 + 24, y + 17, &FreeSans9pt7b, "alles erledigt");
}

// ── footer ───────────────────────────────────────────────────────────────

void hm(int64_t sec, char* out, size_t cap) {
    if (!sec) { snprintf(out, cap, "--:--"); return; }
    const ac::Civil c = ac::fromSec(sec);
    snprintf(out, cap, "%02d:%02d", c.hour, c.minute);
}

void drawFooter(int64_t now) {
    d.fillRect(0, FOOTER_Y - 2, W, 2, GxEPD_BLACK);
    const ac::Civil c = ac::fromSec(now);
    char a[8], b[8], wx[8];
    hm(s_snap->src[clockdata::SRC_CALENDAR].at, a, sizeof(a));
    hm(s_snap->src[clockdata::SRC_TODO].at, b, sizeof(b));
    hm(s_snap->src[clockdata::SRC_WEATHER].at, wx, sizeof(wx));
    char line[120];
    snprintf(line, sizeof(line), "Stand %d.%d. %02d:%02d   Termine %s%s   To-Do %s%s   Wetter %s%s", c.day, c.month, c.hour, c.minute, a,
             s_snap->src[clockdata::SRC_CALENDAR].ok ? "" : " !", b, s_snap->src[clockdata::SRC_TODO].ok ? "" : " !", wx,
             s_snap->src[clockdata::SRC_WEATHER].ok ? "" : " !");
    printAt(10, FOOTER_Y + 22, &FreeSans9pt7b, line);
    printRight(W - 10, FOOTER_Y + 22, &FreeSans9pt7b, "ARC-Clock " ARC_FW_VERSION);
}

// ── refresh ──────────────────────────────────────────────────────────────

void drawAll(int64_t now) {
    spibus::lock();
    d.setFullWindow();
    d.fillScreen(GxEPD_WHITE);
    d.setTextColor(GxEPD_BLACK);
    drawHeader(now);
    drawWeatherBand(now);
    drawCalendar(now);
    drawTodo(now);
    drawFooter(now);
    d.display(false);                       // the bus is released while the panel is busy (busy callback)
    spibus::unlock();
}

void doRefresh() {
    clockdata::requestAll();
    clockdata::waitForFresh(60000);         // without network: the cached data is drawn
    if (!s_snap) s_snap = new clockdata::Data();
    clockdata::lock();
    *s_snap = clockdata::data();
    clockdata::unlock();
    const int64_t now = nowSec();
    Serial.printf("[EPD] Aktualisierung (%u Termine, %d Aufgaben, Wetter %s)\n", (unsigned)s_snap->agenda.n, s_snap->taskCount,
                  s_snap->weather.valid ? "ok" : "fehlt");
    const uint32_t t0 = millis();
    drawAll(now);
    s_lastDay = ac::daysOf(now);
    s_lastRefreshSec = now;
    s_calRevDrawn = s_snap->src[clockdata::SRC_CALENDAR].revision;
    s_refreshCount++;
    s_prefs.begin("clock", false);
    s_prefs.putLong64("epdDay", s_lastDay);
    s_prefs.end();
    Serial.printf("[EPD] fertig in %lu ms\n", (unsigned long)(millis() - t0));
}

bool timePlausible() { return ac::fromSec(nowSec()).year >= 2024; }

void epdTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));
        if (!timePlausible()) continue;
        const ClockConfig cfg = clockcfg::copy();
        const int64_t now = nowSec();
        const int64_t today = ac::daysOf(now);
        const int minutes = (int)((now - ac::dayStart(now)) / 60);

        const bool scheduled = s_lastDay != today && minutes >= cfg.epdHour * 60 + cfg.epdMinute;
        const bool neverDrawn = s_lastDay == 0;
        bool newEvents = false;
        if (cfg.epdOnNewEvents && s_lastDay == today && now - s_lastRefreshSec > 1800) {
            clockdata::lock();
            newEvents = clockdata::data().src[clockdata::SRC_CALENDAR].revision != s_calRevDrawn && clockdata::data().src[clockdata::SRC_CALENDAR].ok;
            clockdata::unlock();
        }
        const bool wanted = s_requested || scheduled || neverDrawn || newEvents;
        if (!wanted) continue;
        if (lid::open() && !s_forced && !neverDrawn) continue;      // do not flash the panel while the lid is open
        s_requested = s_forced = false;
        doRefresh();
    }
}

}  // namespace

namespace epdclock {

void begin() {
    s_prefs.begin("clock", true);
    s_lastDay = s_prefs.getLong64("epdDay", 0);
    s_prefs.end();
    epddrv::init();                          // no drawing: the last picture stays on the panel
    Serial.printf("[EPD] letzter Refresh: %s\n", s_lastDay ? String((long)s_lastDay).c_str() : "noch nie");
}

void startTask() {
    xTaskCreatePinnedToCore(epdTask, "EPDCLK", 8192, nullptr, 1, &s_task, 0);
}

void request(bool force) {
    s_requested = true;
    if (force) s_forced = true;
    if (s_task) xTaskNotifyGive(s_task);
}

int64_t lastRefreshDay() { return s_lastDay; }

String statusLine() {
    char buf[96];
    if (!s_lastDay) return "E-Ink: noch nie aktualisiert";
    const ac::Civil c = ac::fromSec(s_lastRefreshSec ? s_lastRefreshSec : s_lastDay * 86400);
    snprintf(buf, sizeof(buf), "E-Ink: zuletzt %d.%d.%s (%u Refreshes seit Start)", c.day, c.month, s_lastRefreshSec ? "" : " (vor Neustart)", (unsigned)s_refreshCount);
    return String(buf);
}

}  // namespace epdclock
