/*
 * ARC-Clock – desk clock / calendar on the clapperboard hardware (ARC_AP board, ESP32-S3).
 * Second app on the chip (ota_1); ARC-Slate (ota_0) is the other one.
 *
 *   lid open    LED matrices: time with seconds, weather animation, info pages, reminders
 *   lid closed  matrices dark; the e-ink shows the daily overview (one refresh per day)
 *
 * Time base: DS3231 + ARC-Core (arc::clock). Network, known WiFis and NTP: ARC-Core (arc::net),
 * shared with ARC-Slate. Data: Nextcloud (CalDAV calendar, Obsidian to-do file) and Open-Meteo.
 * Pins: pins.h · serial console 115200 baud, "help" lists the commands.
 */
#include <Arduino.h>
#include <ArcCore.h>
#include <Wire.h>
#include "bootsel.h"
#include "clock_config.h"
#include "core/civil.h"
#include "data_store.h"
#include "epd_clock.h"
#include "input.h"
#include "lid.h"
#include "matrix.h"
#include "matrix_clock.h"
#include "pins.h"
#include "spi_bus.h"

#if __has_include("secrets.h")
#include "secrets.h"
constexpr int kDefaultWifiCount = sizeof(kDefaultWifi) / sizeof(kDefaultWifi[0]);
#elif __has_include("../slate/secrets.h")
#include "../slate/secrets.h"           // no own secrets.h: the same default networks as the slate
constexpr int kDefaultWifiCount = sizeof(kDefaultWifi) / sizeof(kDefaultWifi[0]);
#else
static const arc::net::DefaultNetwork kDefaultWifi[] = {{"", "", 0}};   // placeholder, no default network
constexpr int kDefaultWifiCount = 0;
static const char* const kArcNetPassword = "arc-net-2026";
#endif

namespace {

constexpr const char* kCaps[] = {"clock"};

uint32_t s_lastActivityMs = 0;
int64_t s_sparkleDay = 0;             // local day of the last "first open" effect
uint32_t s_switchAtMs = 0;            // long press: switch to the slate shortly

int64_t nowSec() { return arc::clock::nowNs() / 1000000000LL; }

void fillStatus(JsonDocument& doc) {
    doc["lid"] = lid::open() ? "open" : "closed";
    JsonObject src = doc["sources"].to<JsonObject>();
    clockdata::lock();
    const clockdata::Data& d = clockdata::data();
    for (int i = 0; i < clockdata::SRC_COUNT; i++) {
        JsonObject o = src[clockdata::sourceName((clockdata::Source)i)].to<JsonObject>();
        o["ok"] = d.src[i].ok;
        o["at"] = d.src[i].at;
        if (d.src[i].error[0]) o["error"] = d.src[i].error;
    }
    clockdata::unlock();
}

void onLid(bool open) {
    s_lastActivityMs = millis();
    mtx::setEnabled(open);
    if (open) {
        const int64_t today = ac::daysOf(nowSec());
        if (today != s_sparkleDay) {          // first time the lid opens today: a short sparkle
            s_sparkleDay = today;
            clockmatrix::sparkle(1500);
        }
    } else {
        epdclock::lidClosed();                // an e-ink change that waited for the lid
    }
}

void detectSqwEdge() {
    bool falling = true;
    int32_t offsetMs = 0;
    Serial.println("[CLK] Messe, welche SQW-Flanke den Sekundenwechsel markiert ...");
    if (!arc::clock::detectSqwEdge(falling, offsetMs)) {
        Serial.println("[CLK] Messung fehlgeschlagen (RTC/SQW pruefen)");
        return;
    }
    Serial.printf("[CLK] Sekundenwechsel = %s Flanke (Abstand %ld ms)\n", falling ? "fallende" : "steigende", (long)offsetMs);
    ClockConfig& c = clockcfg::get();
    c.sqwKnown = true;
    c.sqwFalling = falling;
    clockcfg::save();
    arc::clock::useEdge(falling);
}

String age(int64_t at) {
    if (!at) return "nie";
    const int64_t s = nowSec() - at;
    if (s < 90) return String((long)s) + " s";
    if (s < 5400) return String((long)(s / 60)) + " min";
    return String((long)(s / 3600)) + " h";
}

void printHelp() {
    Serial.println(
        "\n=== ARC-Clock " ARC_FW_VERSION " - serielle Befehle ===\n"
        "  ?                   Status\n"
        "  cfg                 Einstellungen anzeigen\n"
        "  nc url URL | nc user ID | nc login NAME|- | nc pass APP-PASSWORT | nc insecure 0|1\n"
        "                      (ID = Benutzer-ID fuer die Pfade, z. B. Admin; login = Anmeldename des App-Passworts, z. B. E-Mail)\n"
        "  cal add PFAD | cal clear      CalDAV-Pfad (z. B. /remote.php/dav/calendars/julian/personal/)\n"
        "  todo path DATEI     Markdown-Datei fuers Dashboard (1:1, ohne Front Matter und erste Ueberschrift)\n"
        "  loc LAT LON [ORT] | loc auto   Standort fuer das Wetter (auto: aus der oeffentlichen IP)\n"
        "  fetch [cal|todo|wx|all]   Daten jetzt abrufen und anzeigen\n"
        "  epd                 E-Ink jetzt aktualisieren (sonst nur bei Aenderungen, Pruefung alle 15 min; kleine teilweise)\n"
        "  epdpart             Test: Dashboard-Bereich teilweise aktualisieren\n"
        "  epdtime HH:MM       taeglicher Wetter-Stand auf dem E-Ink\n"
        "  bright day|night N  Helligkeit 0-15 (folgt dem Sonnenstand) | idle MIN | remind MIN | flash 0|1\n"
        "  every cal|todo|wx MIN   Abrufintervalle\n"
        "  wifi | wifi add SSID PASS [PRIO] | wifi clear\n"
        "  time YYYY-MM-DD HH:MM:SS   RTC stellen | n  NTP | sqw  SQW-Flanke messen\n"
        "  scroll TEXT | sparkle | mtx 0|1|2 (Test)    Taster kurz: naechster Termin\n"
        "  mode slate          zur Klappen-Firmware wechseln\n");
}

void printStatus() {
    const int64_t now = nowSec();
    const ac::Civil c = ac::fromSec(now);
    Serial.printf("Zeit %04d-%02d-%02d %02d:%02d:%02d  SQW %s  Temp %.1f C\n", c.year, c.month, c.day, c.hour, c.minute, c.second,
                  arc::clock::sqwActive() ? "ok" : "FEHLT", arc::clock::temperature());
    Serial.printf("Netz: %s\n", arc::net::statusLine().c_str());
    Serial.printf("Klappe %s (seit %lu s), Matrix %s, Nacht %s\n", lid::open() ? "offen" : "zu", (unsigned long)(lid::sinceMs() / 1000),
                  mtx::enabled() ? "an" : "aus", clockmatrix::isNight() ? "ja" : "nein");
    clockdata::lock();
    const clockdata::Data& d = clockdata::data();
    for (int i = 0; i < clockdata::SRC_COUNT; i++)
        Serial.printf("  %-8s %s  Stand vor %s  %s\n", clockdata::sourceName((clockdata::Source)i), d.src[i].ok ? "ok    " : "FEHLT ",
                      age(d.src[i].at).c_str(), d.src[i].error);
    Serial.printf("  %d Termine, %d Dashboard-Zeilen, Wetter %s\n", d.agenda.n, d.docCount, d.weather.valid ? "da" : "fehlt");
    clockdata::unlock();
    Serial.println(epdclock::statusLine());
    Serial.printf("Heap frei %lu, PSRAM frei %lu\n", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
}

bool parseBool(const String& s) { return s == "1" || s == "on" || s == "ja" || s == "true"; }

void handleConfig(const String& cmd, const String& arg) {
    ClockConfig& c = clockcfg::get();
    bool changed = true;
    if (cmd == "nc") {
        const int sp = arg.indexOf(' ');
        const String key = sp < 0 ? arg : arg.substring(0, sp);
        String val = sp < 0 ? String() : arg.substring(sp + 1);
        val.trim();
        if (key == "url") { while (val.endsWith("/")) val.remove(val.length() - 1); c.ncUrl = val; }
        else if (key == "user") c.ncUser = val;
        else if (key == "login") c.ncLogin = val == "-" ? "" : val;
        else if (key == "pass") c.ncPass = val;
        else if (key == "insecure") c.tlsInsecure = parseBool(val);
        else { Serial.println("nc url|user|pass|insecure ..."); changed = false; }
    } else if (cmd == "cal") {
        if (arg.startsWith("add ")) {
            String p = arg.substring(4);
            p.trim();
            if (!p.endsWith("/")) p += "/";
            if (!c.calendars.isEmpty() && !c.calendars.endsWith("\n")) c.calendars += "\n";
            c.calendars += p;
        } else if (arg == "clear") c.calendars = "";
        else { Serial.println("cal add PFAD | cal clear"); changed = false; }
    } else if (cmd == "todo") {
        const int sp = arg.indexOf(' ');
        const String key = sp < 0 ? arg : arg.substring(0, sp);
        String val = sp < 0 ? String() : arg.substring(sp + 1);
        val.trim();
        if (key == "path") c.todoPath = val;
        else if (key == "head") c.todoHeading = val == "-" ? "" : val;
        else { Serial.println("todo path DATEI | todo head UEBERSCHRIFT"); changed = false; }
    } else if (cmd == "loc") {
        float lat, lon;
        char place[40] = "";
        if (arg == "auto") {
            c.locAuto = true;
            clockdata::relocate();                   // next weather fetch asks ip-api.com first
            clockdata::request(clockdata::SRC_WEATHER);
        } else if (sscanf(arg.c_str(), "%f %f %39[^\n]", &lat, &lon, place) >= 2) {
            c.lat = lat; c.lon = lon;
            c.locAuto = false;
            if (place[0]) c.place = place;
            clockdata::request(clockdata::SRC_WEATHER);
        } else { Serial.println("loc LAT LON [ORT] | loc auto"); changed = false; }
    } else if (cmd == "epdtime") {
        int h, m;
        if (sscanf(arg.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) { c.epdHour = h; c.epdMinute = m; }
        else { Serial.println("epdtime HH:MM"); changed = false; }
    } else if (cmd == "bright") {
        const int sp = arg.indexOf(' ');
        const String which = sp < 0 ? arg : arg.substring(0, sp);
        const int v = constrain(arg.substring(sp + 1).toInt(), 0, 15);
        if (sp > 0 && which == "day") c.brightnessDay = v;
        else if (sp > 0 && which == "night") c.brightnessNight = v;
        else { Serial.println("bright day|night 0-15"); changed = false; }
    } else if (cmd == "idle") c.idleOffMin = (uint16_t)max(0, (int)arg.toInt());
    else if (cmd == "remind") c.reminderMin = (uint8_t)constrain(arg.toInt(), 0, 120);
    else if (cmd == "flash") c.reminderFlash = arg.toInt() != 0;
    else if (cmd == "every") {
        const int sp = arg.indexOf(' ');
        const String which = sp < 0 ? arg : arg.substring(0, sp);
        const uint16_t v = (uint16_t)constrain(arg.substring(sp + 1).toInt(), 1, 1440);
        if (sp > 0 && which == "cal") c.calendarMin = v;
        else if (sp > 0 && which == "todo") c.todoMin = v;
        else if (sp > 0 && which == "wx") c.weatherMin = v;
        else { Serial.println("every cal|todo|wx MINUTEN"); changed = false; }
    } else changed = false;
    if (changed) {
        clockcfg::save();
        clockcfg::print(Serial);
        if (cmd == "nc" || cmd == "cal" || cmd == "todo") clockdata::resume();   // new credentials / paths: try again
    }
}

void handleSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) return;
    const int sp = line.indexOf(' ');
    const String cmd = sp < 0 ? line : line.substring(0, sp);
    const String arg = sp < 0 ? String() : line.substring(sp + 1);

    if (cmd == "?") printStatus();
    else if (cmd == "help") printHelp();
    else if (cmd == "cfg") clockcfg::print(Serial);
    else if (cmd == "nc" || cmd == "cal" || cmd == "todo" || cmd == "loc" || cmd == "epdtime" || cmd == "bright" || cmd == "idle" ||
             cmd == "remind" || cmd == "flash" || cmd == "every") handleConfig(cmd, arg);
    else if (cmd == "fetch") {
        using clockdata::Source;
        if (arg.isEmpty() || arg == "all" || arg == "cal") clockdata::fetchNowVerbose(clockdata::SRC_CALENDAR, Serial);
        if (arg.isEmpty() || arg == "all" || arg == "todo") clockdata::fetchNowVerbose(clockdata::SRC_TODO, Serial);
        if (arg.isEmpty() || arg == "all" || arg == "wx") clockdata::fetchNowVerbose(clockdata::SRC_WEATHER, Serial);
    }
    else if (cmd == "epd") epdclock::request(arg == "force");
    else if (cmd == "wifi") {
        if (arg.startsWith("add ")) {
            const String rest = arg.substring(4);
            const int a = rest.indexOf(' ');
            const String ssid = rest.substring(0, a);
            String pass = rest.substring(a + 1);
            int prio = 10;
            const int b = pass.lastIndexOf(' ');
            if (b > 0 && pass.substring(b + 1).toInt() > 0) { prio = pass.substring(b + 1).toInt(); pass = pass.substring(0, b); }
            arc::net::addNetwork(ssid, pass, prio);
        } else if (arg == "clear") arc::net::clearNetworks();
        arc::net::printNetworks(Serial);
    }
    else if (cmd == "time") {
        int y, mo, da, h, mi, se;
        if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &da, &h, &mi, &se) == 6) arc::clock::setTime(DateTime(y, mo, da, h, mi, se));
    }
    else if (cmd == "n") { arc::net::requestNtpSync(); Serial.println("NTP-Sync angefordert"); }
    else if (cmd == "sqw") detectSqwEdge();
    else if (cmd == "epdpart") epdclock::requestPartialTest();
    else if (cmd == "wxtest") {
        // wxtest KIND [night] [PHASE 0..1]: shows that weather on the matrix for 60 s
        struct Kind { const char* name; uint8_t wmo, wind; };
        static const Kind kinds[] = {{"clear", 0, 0}, {"heiter", 2, 0}, {"cloudy", 3, 0}, {"fog", 45, 0}, {"drizzle", 51, 0},
                                     {"rain", 63, 0}, {"showers", 80, 0}, {"snow", 73, 0}, {"thunder", 95, 0}, {"wind", 0, 50}};
        String rest = arg;
        rest.trim();
        rest.toLowerCase();
        const int sp = rest.indexOf(' ');
        const String name = sp < 0 ? rest : rest.substring(0, sp);
        bool night = false;
        double phase = -1;
        for (int i = sp; i >= 0 && i < (int)rest.length();) {
            const int next = rest.indexOf(' ', i + 1);
            String w = rest.substring(i + 1, next < 0 ? rest.length() : next);
            w.trim();
            if (w == "night") night = true;
            else if (w.length()) phase = constrain(w.toFloat(), 0.0f, 1.0f);
            i = next;
        }
        const Kind* k = nullptr;
        for (const Kind& c : kinds) if (name == c.name) k = &c;
        if (!k) Serial.println("wxtest clear|heiter|cloudy|fog|drizzle|rain|showers|snow|thunder|wind [night] [PHASE 0..1]");
        else {
            clockmatrix::testWeather(k->wmo, k->wind, night, phase, 60000);
            Serial.printf("wxtest: %s%s, Mondphase %s, 60 s\n", k->name, night ? " (Nacht)" : "", phase < 0 ? "echt" : String(phase, 2).c_str());
        }
    }
    else if (cmd == "scroll") clockmatrix::scroll(arg.isEmpty() ? String("ARC CLOCK TEST") : arg);
    else if (cmd == "sparkle") clockmatrix::sparkle(1500);
    else if (cmd == "mtx") mtx::setTestMode((uint8_t)arg.toInt());
    else if (cmd == "mode") {
        if (arg == "slate") bootsel::switchTo(bootsel::App::Slate);
        else Serial.println("mode slate");
    }
    else Serial.printf("Unbekannter Befehl '%s' - 'help' zeigt alle Befehle\n", line.c_str());
}

void handleInput() {
    input::Event e;
    while (input::poll(e)) {
        s_lastActivityMs = millis();
        if (!mtx::enabled() && lid::open()) {             // auto-off: any input wakes the matrices
            mtx::setEnabled(true);
            continue;
        }
        switch (e) {
            case input::TURN_CW: if (lid::open()) clockmatrix::adjustBrightness(+1); break;
            case input::TURN_CCW: if (lid::open()) clockmatrix::adjustBrightness(-1); break;
            case input::PRESS_SHORT: if (lid::open()) clockmatrix::showNext(); break;
            case input::PRESS_LONG:
                mtx::showOverlay("SLATE", 0);
                s_switchAtMs = millis() + 900;
                break;
            default: break;
        }
    }
    if (s_switchAtMs && (int32_t)(millis() - s_switchAtMs) >= 0) {
        s_switchAtMs = 0;
        if (!bootsel::switchToOther()) mtx::showOverlay("NO SLATE", 2500);
        else mtx::clearOverlay();
    }
}

void handleIdle() {
    const ClockConfig c = clockcfg::copy();
    if (c.idleOffMin && lid::open() && mtx::enabled() && millis() - s_lastActivityMs > (uint32_t)c.idleOffMin * 60000UL &&
        lid::sinceMs() > (uint32_t)c.idleOffMin * 60000UL) {
        mtx::setEnabled(false);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nARC-Clock " ARC_FW_VERSION " startet ...");

    // Encoder button held at power-on: back to the other firmware (emergency exit).
    if (bootsel::heldAtBoot(PIN_ENC_SW, 1500) && bootsel::switchToOther()) return;

    clockcfg::begin();
    clockdata::begin();
    const ClockConfig cfg = clockcfg::copy();

    spibus::begin();
    mtx::begin(cfg.brightnessDay);
    clockmatrix::begin();
    mtx::showOverlay("ARC CLOCK", 1500);
    mtx::startTask();

    arc::clock::begin({PIN_RTC_SDA, PIN_RTC_SCL, PIN_RTC_SQW}, cfg.sqwFalling);
    if (arc::clock::rtcPresent() && !cfg.sqwKnown) detectSqwEdge();

    arc::net::Config nc;
    nc.hostname = "arc-clock";
    nc.deviceType = "clock";
    nc.device = "clock";
    nc.firmware = ARC_FW_VERSION;
    nc.caps = kCaps;
    nc.capCount = 1;
    nc.defaults = kDefaultWifi;
    nc.defaultCount = kDefaultWifiCount;
    nc.arcNetPassword = kArcNetPassword;
    nc.fillStatus = fillStatus;
    nc.ltcEngine = false;
    nc.lowLatency = false;
    arc::net::begin(nc);

    lid::onChange(onLid);
    lid::begin();
    input::begin();
    mtx::setEnabled(lid::open());
    s_lastActivityMs = millis();

    clockdata::startTask();
    epdclock::begin();
    epdclock::startTask();

    printHelp();
}

void loop() {
    arc::net::loop();
    handleInput();
    handleIdle();
    handleSerial();
    vTaskDelay(pdMS_TO_TICKS(5));
}
