#include "epd_clock.h"
#include <ArcCore.h>
#include <Preferences.h>
#include <WiFi.h>
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
TaskHandle_t s_task = nullptr;
volatile bool s_requested = false;
constexpr int ZONES = epdlayout::ZONE_COUNT;
constexpr int PARTIALS_BEFORE_FULL = 12;    // a full refresh against ghosting after this many partial ones
uint32_t s_zone[ZONES] = {0};               // hash per zone of the picture on the panel (0 = unknown)
int s_partials = 0;                         // partial refreshes since the last full one
bool s_panelValid = false;                  // the panel shows the picture of s_zone (a full refresh worked since the start)
volatile bool s_partialTest = false;
int64_t s_lastDay = 0;                      // local day of the last daily refresh (weather snapshot)
int64_t s_lastRefreshSec = 0;
int64_t s_nextCheckSec = 0;
int64_t s_pendingSince = 0;                 // a change waits for the lid since then (0 = none)
uint32_t s_refreshCount = 0, s_checkCount = 0;
bool s_quietLogged = false;
Preferences s_prefs;

int64_t nowSec() { return arc::clock::nowNs() / 1000000000LL; }

// Draws the layout into nothing and hashes the pixels per zone: same hash = same picture = no refresh needed.
class HashGfx : public Adafruit_GFX {
public:
    HashGfx() : Adafruit_GFX(epdlayout::W, epdlayout::H) { for (auto& v : zone) v = 2166136261u; }
    uint32_t zone[ZONES];
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (color) return;                                       // white is the background: only black pixels count
        uint32_t& h = zone[epdlayout::zoneOf(x, y)];
        h = (h ^ ((uint32_t)x | ((uint32_t)y << 10))) * 16777619u;
    }
    void fillScreen(uint16_t color) override { for (auto& v : zone) v = (v ^ color) * 16777619u; }
};

epdlayout::Options options(String& placeKeep) {
    placeKeep = clockcfg::copy().place;
    epdlayout::Options opt;
    opt.place = placeKeep.c_str();
    opt.version = "ARC-Clock " ARC_FW_VERSION;
    return opt;
}

// Copies the current data (the forecast of the day on the panel follows the weather data).
void takeSnapshot() {
    if (!s_snap) s_snap = new clockdata::Data();
    clockdata::lock();
    *s_snap = clockdata::data();
    clockdata::unlock();
}

void pictureZones(int64_t now, uint32_t out[ZONES]) {
    String place;
    const epdlayout::Options opt = options(place);
    HashGfx h;
    epdlayout::draw(h, *s_snap, now, opt);
    for (int z = 0; z < ZONES; z++) out[z] = h.zone[z] ? h.zone[z] : 1;
}

// Draws the whole frame into the buffer and sends it: all of it (nWindows = 0) or window by window (partial refreshes).
bool drawFrame(int64_t now, const epdlayout::Rect* windows, int nWindows) {
    String place;
    const epdlayout::Options opt = options(place);
    spibus::lock();
    d.setFullWindow();
    epdlayout::draw(d, *s_snap, now, opt);
    bool ok = true;
    if (nWindows > 0) {
        for (int i = 0; i < nWindows && ok; i++) ok = epddrv::refreshWindow(windows[i].x, windows[i].y, windows[i].w, windows[i].h);
    } else {
        ok = epddrv::refreshFull();
    }
    spibus::unlock();
    return ok;
}

epdlayout::Rect alignX(epdlayout::Rect r) {         // the panel takes windows on multiples of 8 in x
    const int x1 = (r.x + r.w + 7) & ~7;
    r.x &= ~7;
    r.w = x1 - r.x;
    return r;
}

// Small changes are refreshed zone by zone, partially (one window per changed zone, so two far apart zones do not
// become one big window). The daily refresh, the first one after a start, big changes (more than 65 % of the picture)
// and every PARTIALS_BEFORE_FULL-th window are full refreshes (ghosting).
int partialWindows(uint32_t mask, bool daily, epdlayout::Rect out[ZONES]) {
    if (daily || !s_panelValid) return 0;
    int n = 0;
    long area = 0;
    for (int z = 0; z < ZONES; z++) {
        if (!(mask & (1u << z))) continue;
        out[n] = alignX(epdlayout::zoneRect(z));
        area += (long)out[n].w * out[n].h;
        n++;
    }
    if (s_partials + n > PARTIALS_BEFORE_FULL || area > (long)epdlayout::W * epdlayout::H * 65 / 100) return 0;
    return n;
}

void saveState() {
    s_prefs.begin("clock", false);
    s_prefs.putLong64("epdDay", s_lastDay);
    s_prefs.end();
}

// Refreshes the panel with the snapshot. Returns false if the panel did not refresh.
bool refreshNow(int64_t now, const uint32_t zones[ZONES], const epdlayout::Rect* windows, int nWindows, const char* why) {
    Serial.printf("[EPD] Aktualisierung (%s%s): %u Termine, %d Dashboard-Zeilen, Wetter %s\n", why, nWindows ? ", teilweise" : "", (unsigned)s_snap->agenda.n, s_snap->docCount,
                  s_snap->weather.valid ? "ok" : "fehlt");
    const uint32_t t0 = millis();
    // The radio's modem sleep wakes up in bursts that disturbed the long SPI wires (rows of the picture shifted by
    // a few pixels): the radio stays awake while the frame is sent and the panel refreshes.
    WiFi.setSleep(false);
    const bool drawn = drawFrame(now, windows, nWindows);
    WiFi.setSleep(true);
    if (!drawn) {
        Serial.println("[EPD] Aktualisierung fehlgeschlagen, neuer Versuch bei der naechsten Pruefung");
        for (auto& v : s_zone) v = 0;                  // unknown: the next check draws everything again
        s_panelValid = false;
        return false;
    }
    for (int z = 0; z < ZONES; z++) s_zone[z] = zones[z];
    if (nWindows) s_partials += nWindows;
    else { s_partials = 0; s_panelValid = true; }
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
        if (s_partialTest) {                    // serial `epdpart`: a partial refresh of the dashboard area, to check that path
            s_partialTest = false;
            if (!s_snap || !s_panelValid) {
                Serial.println("[EPD] epdpart: erst nach einem vollen Refresh moeglich");
            } else {
                takeSnapshot();
                const epdlayout::Rect r = alignX(epdlayout::zoneRect(epdlayout::ZONE_DASHBOARD));
                const uint32_t t0 = millis();
                WiFi.setSleep(false);
                const bool ok = drawFrame(nowSec(), &r, 1);
                WiFi.setSleep(true);
                Serial.printf("[EPD] epdpart: Bereich %dx%d bei %d,%d in %lu ms %s\n", r.w, r.h, r.x, r.y,
                              (unsigned long)(millis() - t0), ok ? "ok" : "FEHLGESCHLAGEN");
            }
            continue;
        }
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

        if (!bootDone) clockdata::waitForNetwork(45000);   // the first picture should have fresh data, not the cache
        if (daily || requested || !bootDone) {
            clockdata::requestAll();
            clockdata::waitForFresh(60000);     // without network: the cached data is drawn
        }
        if (daily) {                            // remembered; the daily refresh is always a full one (ghosting)
            s_lastDay = today;
            saveState();
        }
        takeSnapshot();
        uint32_t zh[ZONES];
        pictureZones(now, zh);
        s_checkCount++;

        if (!bootDone || requested) {
            refreshNow(now, zh, nullptr, 0, !bootDone ? "Start" : "Befehl");
            bootDone = true;
            continue;
        }
        uint32_t mask = 0;
        for (int z = 0; z < ZONES; z++)
            if (zh[z] != s_zone[z]) mask |= 1u << z;
        if (!mask) {
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
        // A change. A full refresh (black/white flashing) waits with the lid open, but not forever; partial ones are
        // hardly noticeable and are drawn at once, zone by zone.
        epdlayout::Rect wins[ZONES];
        const int nWins = partialWindows(mask, daily, wins);
        if (nWins == 0 && lid::open()) {
            if (!s_pendingSince) s_pendingSince = now;
            if (now - s_pendingSince < LID_DEFER_MAX_SEC) {
                Serial.println("[EPD] Aenderung erkannt - der volle Refresh wartet bis die Klappe zu ist");
                continue;
            }
        }
        refreshNow(now, zh, wins, nWins, daily ? "taeglich" : "Aenderung");
    }
}

}  // namespace

namespace epdclock {

void begin() {
    s_prefs.begin("clock", true);
    s_lastDay = s_prefs.getLong64("epdDay", 0);
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

void requestPartialTest() {
    s_partialTest = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void checkNow() {
    s_nextCheckSec = 0;                      // the next pass looks for changes at once
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
    snprintf(buf, sizeof(buf), "E-Ink: zuletzt %d.%d. %02d:%02d, %u Refreshes (%d teilweise seit dem letzten vollen) / %u Pruefungen%s", c.day,
             c.month, c.hour, c.minute, (unsigned)s_refreshCount, s_partials, (unsigned)s_checkCount, s_pendingSince ? ", Aenderung wartet auf die Klappe" : "");
    return String(buf);
}

}  // namespace epdclock
