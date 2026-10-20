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

constexpr int64_t CHECK_EVERY_SEC = 15 * 60;       // look for changes every 15 minutes
constexpr int64_t LID_DEFER_MAX_SEC = 2 * 3600;    // a change waits at most this long for the lid to close
constexpr int QUIET_FROM_MIN = 22 * 60;            // night rest: from 22:00 until the daily refresh

clockdata::Data* s_snap = nullptr;          // copy of the data for one drawing pass
ac::Weather* s_wx = nullptr;                // weather as of the morning refresh (frozen during the day)
TaskHandle_t s_task = nullptr;
volatile bool s_requested = false;
uint32_t s_drawnHash = 0;                   // hash of the picture on the panel (0 = unknown)
int64_t s_lastDay = 0;                      // local day of the last daily refresh (weather snapshot)
int64_t s_lastRefreshSec = 0;
int64_t s_nextCheckSec = 0;
int64_t s_pendingSince = 0;                 // a change waits for the lid since then (0 = none)
uint32_t s_refreshCount = 0, s_checkCount = 0;
bool s_quietLogged = false;
Preferences s_prefs;

int64_t nowSec() { return arc::clock::nowNs() / 1000000000LL; }

// Draws the layout into nothing and hashes the pixels: same hash = same picture = no refresh needed.
class HashGfx : public Adafruit_GFX {
public:
    HashGfx() : Adafruit_GFX(epdlayout::W, epdlayout::H) {}
    uint32_t hash = 2166136261u;
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (color) return;                                       // white is the background: only black pixels count
        hash = (hash ^ ((uint32_t)x | ((uint32_t)y << 10))) * 16777619u;
    }
    void fillScreen(uint16_t color) override { hash = (hash ^ color) * 16777619u; }
};

epdlayout::Options options(String& placeKeep) {
    placeKeep = clockcfg::copy().place;
    epdlayout::Options opt;
    opt.place = placeKeep.c_str();
    opt.version = "ARC-Clock " ARC_FW_VERSION;
    return opt;
}

// Copies the current data, with the weather of the morning refresh.
void takeSnapshot() {
    if (!s_snap) s_snap = new clockdata::Data();
    if (!s_wx) s_wx = new ac::Weather();
    clockdata::lock();
    *s_snap = clockdata::data();
    clockdata::unlock();
    if (s_wx->valid) s_snap->weather = *s_wx;
}

uint32_t pictureHash(int64_t now) {
    String place;
    const epdlayout::Options opt = options(place);
    HashGfx h;
    epdlayout::draw(h, *s_snap, now, opt);
    return h.hash ? h.hash : 1;
}

bool drawAll(int64_t now) {
    String place;
    const epdlayout::Options opt = options(place);
    spibus::lock();
    d.setFullWindow();
    epdlayout::draw(d, *s_snap, now, opt);
    const bool ok = epddrv::refreshFull();
    spibus::unlock();
    return ok;
}

void saveState() {
    s_prefs.begin("clock", false);
    s_prefs.putLong64("epdDay", s_lastDay);
    s_prefs.putULong("epdHash", s_drawnHash);
    s_prefs.end();
}

// Refreshes the panel with the snapshot. Returns false if the panel did not refresh.
bool refreshNow(int64_t now, uint32_t hash, const char* why) {
    Serial.printf("[EPD] Aktualisierung (%s): %u Termine, %d Aufgaben, Wetter %s\n", why, (unsigned)s_snap->agenda.n, s_snap->taskCount,
                  s_snap->weather.valid ? "ok" : "fehlt");
    const uint32_t t0 = millis();
    if (!drawAll(now)) {
        Serial.println("[EPD] Aktualisierung fehlgeschlagen, neuer Versuch bei der naechsten Pruefung");
        s_drawnHash = 0;
        return false;
    }
    s_drawnHash = hash;
    s_lastRefreshSec = now;
    s_refreshCount++;
    s_pendingSince = 0;
    saveState();
    Serial.printf("[EPD] fertig in %lu ms\n", (unsigned long)(millis() - t0));
    return true;
}

bool timePlausible() { return ac::fromSec(nowSec()).year >= 2024; }

void epdTask(void*) {
    bool bootDone = false;                  // the slate firmware may have drawn the panel: always draw once after a start
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));
        if (!timePlausible()) continue;
        const ClockConfig cfg = clockcfg::copy();
        const int64_t now = nowSec();
        const int64_t today = ac::daysOf(now);
        const int minutes = (int)((now - ac::dayStart(now)) / 60);

        // Daily refresh: fresh data and a new weather snapshot (the only time the weather on the panel changes).
        const bool daily = s_lastDay != today && minutes >= cfg.epdHour * 60 + cfg.epdMinute;
        const bool requested = s_requested;
        const bool check = now >= s_nextCheckSec;
        if (!daily && !requested && !check && bootDone) continue;
        s_requested = false;
        s_nextCheckSec = (now / CHECK_EVERY_SEC + 1) * CHECK_EVERY_SEC;   // on the quarter hour

        if (daily || requested || !bootDone) {
            clockdata::requestAll();
            clockdata::waitForFresh(60000);     // without network: the cached data is drawn
        }
        if (!s_wx) s_wx = new ac::Weather();
        if (daily || !s_wx->valid) {
            clockdata::lock();
            *s_wx = clockdata::data().weather;
            clockdata::unlock();
            if (daily) {
                s_lastDay = today;
                saveState();
            }
        }
        takeSnapshot();
        const uint32_t hash = pictureHash(now);
        s_checkCount++;

        if (!bootDone || requested) {
            refreshNow(now, hash, !bootDone ? "Start" : "Befehl");
            bootDone = true;
            continue;
        }
        if (hash == s_drawnHash) {
            s_pendingSince = 0;
            continue;                           // nothing changed: the panel stays untouched
        }
        // Night rest: from 22:00 until the time of the daily refresh a change is not drawn - unless there are events at
        // night. It is picked up by the daily refresh (the date at the top then changes at that time, not at midnight).
        const int dailyMin = cfg.epdHour * 60 + cfg.epdMinute;
        if (dailyMin < QUIET_FROM_MIN && (minutes >= QUIET_FROM_MIN || minutes < dailyMin) &&
            !epdlayout::hasNightEvents(*s_snap, now)) {
            if (!s_quietLogged) {
                Serial.printf("[EPD] Nachtruhe: Aenderungen warten bis %02d:%02d\n", cfg.epdHour, cfg.epdMinute);
                s_quietLogged = true;
            }
            continue;
        }
        s_quietLogged = false;
        // A change. With the lid open the refresh (black/white flashing) waits, but not forever.
        if (lid::open()) {
            if (!s_pendingSince) s_pendingSince = now;
            if (now - s_pendingSince < LID_DEFER_MAX_SEC) {
                Serial.println("[EPD] Aenderung erkannt - wartet bis die Klappe zu ist");
                continue;
            }
        }
        refreshNow(now, hash, daily ? "taeglich" : "Aenderung");
    }
}

}  // namespace

namespace epdclock {

void begin() {
    s_prefs.begin("clock", true);
    s_lastDay = s_prefs.getLong64("epdDay", 0);
    s_drawnHash = s_prefs.getULong("epdHash", 0);
    s_prefs.end();
    epddrv::init();                          // no drawing here: the task draws once the time and the data are there
}

void startTask() {
    xTaskCreatePinnedToCore(epdTask, "EPDCLK", 8192, nullptr, 1, &s_task, 0);
}

void request(bool) {
    s_requested = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void lidClosed() {
    if (s_pendingSince && s_task) {
        s_nextCheckSec = 0;                  // a waiting change is drawn right away
        xTaskNotifyGive(s_task);
    }
}

int64_t lastRefreshDay() { return s_lastDay; }

String statusLine() {
    char buf[128];
    if (!s_lastRefreshSec) return "E-Ink: seit dem Start noch nicht gezeichnet";
    const ac::Civil c = ac::fromSec(s_lastRefreshSec);
    snprintf(buf, sizeof(buf), "E-Ink: zuletzt %d.%d. %02d:%02d, %u Refreshes / %u Pruefungen seit Start%s", c.day, c.month, c.hour,
             c.minute, (unsigned)s_refreshCount, (unsigned)s_checkCount, s_pendingSince ? ", Aenderung wartet auf die Klappe" : "");
    return String(buf);
}

}  // namespace epdclock
