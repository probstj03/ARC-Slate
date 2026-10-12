#include "epd_clock.h"
#include <ArcCore.h>
#include <Preferences.h>
#include "clock_config.h"
#include "core/civil.h"
#include "data_store.h"
#include "epd_driver.h"
#include "layout/epd_layout.h"
#include "lid.h"
#include "spi_bus.h"

namespace {

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

// ── refresh ──────────────────────────────────────────────────────────────

void drawAll(int64_t now) {
    const String place = clockcfg::copy().place;
    epdlayout::Options opt;
    opt.place = place.c_str();
    opt.version = "ARC-Clock " ARC_FW_VERSION;
    spibus::lock();
    d.setFullWindow();
    epdlayout::draw(d, *s_snap, now, opt);
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
