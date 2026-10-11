/*
 * ARC-Slate – electronic clapperboard on the ARC_AP board (ESP32-S3).
 *
 * Modules
 *   arc_clock   RTC-disciplined µs clock (DS3231 + SQW edge)
 *   ltc_engine  LTC generator / decoder / Jam Sync (PCM5102 + J2)
 *   matrix      2 MAX7219 chains: timecode (7) and roll/shot/scene+take (9)
 *   epaper      7.5" e-ink: slate card with QR code, menu, status bar
 *   ui          rotary encoder + menu
 *   clap        hall sensor, sync flash, clap events
 *   net         WiFi / ARC-Net, mDNS, WebSocket to ARC-Studio
 *   state       shot data + settings (NVS)
 *
 * Pins: pins.h · Wiring: hardware/wiring/ARC-Slate_Verdrahtung.svg
 * Serial console: 115200 baud, "help" lists the commands.
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include "arc_clock.h"
#include "clap.h"
#include "epaper.h"
#include "ltc_engine.h"
#include "matrix.h"
#include "net.h"
#include "spi_bus.h"
#include "state.h"
#include "ui.h"

namespace {

volatile bool s_statusChanged = false;

void onLtcStatus() { s_statusChanged = true; }

void printHelp() {
    Serial.println(
        "\n=== ARC-Slate " ARC_FW_VERSION " – serielle Befehle ===\n"
        "  ?                Status\n"
        "  1 2 4 5 6        Modus: Free Run, Record Run, Slave, Jam->Free, Jam->Rec\n"
        "  q w e r          Bildrate: 30, 25, 29.97 DF, 24\n"
        "  s / x            Start-Stop (Record Run) / Stop\n"
        "  t | tc HH:MM:SS:FF   TC auf 0 / setzen\n"
        "  u                Jam: Kabel abgezogen bestaetigen (TC-Ausgang wieder an)\n"
        "  n                NTP-Sync erzwingen\n"
        "  time YYYY-MM-DD HH:MM:SS   RTC stellen\n"
        "  sqw              SQW-Flanke neu messen\n"
        "  c / f            Klappe simulieren / Blitz testen\n"
        "  mtx 0|1|2        Matrix normal / alle LEDs / Modulnummern\n"
        "  epd              E-Ink neu zeichnen\n"
        "  enc / encrev     Encoder-Debug / Drehrichtung umkehren\n"
        "  i2c              I2C-Scan\n"
        "  take N | scene N | shot X | roll A001 | bright 0-15\n"
        "  lat flash US | lat ltc US   Latenzen setzen (Mikrosekunden)\n"
        "  wifi | wifi add SSID PASS [PRIO] | wifi clear\n"
        "  log | log clear  Ereignis-Log anzeigen / loeschen\n");
}

void printStatus() {
    const Timecode tc = ltc::current();
    const DateTime d = arcclock::now();
    state::lock();
    const ShotInfo s = state::shot();
    const Settings st = state::settings();
    state::unlock();
    Serial.printf("TC %s  Modus %s  %s fps  %s\n", ltc::tcToString(tc).c_str(), ltc::modeKey(), ltc::rateKey(),
                  ltc::running() ? "läuft" : "steht");
    Serial.printf("RTC %04d-%02d-%02d %02d:%02d:%02d.%06lu  SQW %s (%s)  RTC %s\n", d.year(), d.month(), d.day(),
                  d.hour(), d.minute(), d.second(), (unsigned long)arcclock::usInSecond(),
                  arcclock::sqwActive() ? "ok" : "FEHLT", st.sqwFalling ? "fallend" : "steigend",
                  arcclock::rtcPresent() ? "ok" : "FEHLT");
    Serial.printf("Shot: Roll %s  Szene %d  Shot %s  Take %d  Code %s\n", state::rollString().c_str(), s.scene,
                  s.shot.c_str(), s.take, s.code.c_str());
    Serial.printf("Userbits TX %08lX  RX %08lX  Jam ok %lu  RX %.2f fps%s\n", (unsigned long)ltc::txUserBits(),
                  (unsigned long)ltc::rxUserBits(), (unsigned long)ltc::jamOkCount(), ltc::rxFps(),
                  ltc::muteHeld() ? "  [Ausgang stumm bis Kabel ab]" : "");
    Serial.printf("Netz: %s\n", net::statusLine().c_str());
    Serial.printf("Klappe %s, Ereignisse %lu, Heap %lu\n", clap::closed() ? "zu" : "offen",
                  (unsigned long)clap::eventCount(), (unsigned long)ESP.getFreeHeap());
}

void detectSqwEdge() {
    bool falling = true;
    int32_t offsetMs = 0;
    Serial.println("[CLK] Messe, welche SQW-Flanke den Sekundenwechsel markiert ...");
    if (!arcclock::detectSqwEdge(falling, offsetMs)) {
        Serial.println("[CLK] Messung fehlgeschlagen (RTC/SQW prüfen)");
        return;
    }
    Serial.printf("[CLK] Sekundenwechsel = %s Flanke (Abstand %ld ms)\n", falling ? "fallende" : "steigende",
                  (long)offsetMs);
    state::lock();
    state::settings().sqwFalling = falling;
    state::settings().sqwEdgeKnown = true;
    state::touch();
    state::unlock();
    arcclock::useEdge(falling);
}

void i2cScan() {
    arcclock::lockI2C();
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf("  I2C-Gerät 0x%02X%s\n", a, a == 0x68 ? " (DS3231)" : a == 0x57 ? " (EEPROM)" : "");
    }
    arcclock::unlockI2C();
}

void setShotValue(const String& key, const String& val) {
    state::lock();
    ShotInfo& s = state::shot();
    if (key == "take") s.take = constrain(val.toInt(), 0, 99);
    else if (key == "scene") s.scene = constrain(val.toInt(), 0, 99);
    else if (key == "shot") s.shot = val;
    else if (key == "roll") state::setRollFromString(s, val);
    s.loaded = true;
    state::touch();
    state::unlock();
    ltc::refreshUserBits();
    net::sendSlateState();
    epd::requestCard();
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
    else if (cmd == "1") ltc::requestMode(MODE_GEN_FREE_RUN);
    else if (cmd == "2") ltc::requestMode(MODE_GEN_RECORD_RUN);
    else if (cmd == "4") ltc::requestMode(MODE_SLAVE);
    else if (cmd == "5") ltc::requestMode(MODE_JAM_SYNC_FREE_RUN);
    else if (cmd == "6") ltc::requestMode(MODE_JAM_SYNC_RECORD_RUN);
    else if (cmd == "q") ltc::setRate(RATE_30);
    else if (cmd == "w") ltc::setRate(RATE_25);
    else if (cmd == "e") ltc::setRate(RATE_2997DF);
    else if (cmd == "r") ltc::setRate(RATE_24);
    else if (cmd == "s") { if (ltc::running()) ltc::stop(); else ltc::start(); }
    else if (cmd == "x") ltc::stop();
    else if (cmd == "t") ltc::resetTimecode();
    else if (cmd == "tc") { Timecode tc; if (ltc::parseTc(arg, tc)) ltc::setTimecode(tc); }
    else if (cmd == "u") ltc::releaseMuteHold();
    else if (cmd == "n") { net::requestNtpSync(); Serial.println("NTP-Sync angefordert (nur im WLAN mit Internet)"); }
    else if (cmd == "time") {
        int y, mo, da, h, mi, se;
        if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &da, &h, &mi, &se) == 6) {
            arcclock::setTime(DateTime(y, mo, da, h, mi, se));
            Serial.println("RTC gestellt");
        }
    }
    else if (cmd == "sqw") detectSqwEdge();
    else if (cmd == "c") clap::simulate();
    else if (cmd == "f") clap::testFlash();
    else if (cmd == "mtx") mtx::setTestMode((uint8_t)arg.toInt());
    else if (cmd == "epd") epd::requestCard();
    else if (cmd == "enc") { static bool dbg = false; dbg = !dbg; ui::setEncoderDebug(dbg); Serial.printf("Encoder-Debug %s\n", dbg ? "an" : "aus"); }
    else if (cmd == "encrev") {
        state::lock(); state::settings().encReverse = !state::settings().encReverse; state::touch(); state::unlock();
        Serial.println("Drehrichtung umgekehrt (wirkt nach Neustart)");
    }
    else if (cmd == "i2c") i2cScan();
    else if (cmd == "take" || cmd == "scene" || cmd == "shot" || cmd == "roll") setShotValue(cmd, arg);
    else if (cmd == "bright") {
        const uint8_t b = (uint8_t)constrain(arg.toInt(), 0, 15);
        state::lock(); state::settings().brightness = b; state::touch(); state::unlock();
        mtx::setBrightness(b);
    }
    else if (cmd == "lat") {
        const int sp2 = arg.indexOf(' ');
        const String which = arg.substring(0, sp2);
        const int32_t us = arg.substring(sp2 + 1).toInt();
        state::lock();
        if (which == "flash") state::settings().flashLatencyUs = us;
        else if (which == "ltc") state::settings().ltcOutLatencyUs = us;
        state::touch();
        state::unlock();
    }
    else if (cmd == "wifi") {
        if (arg.startsWith("add ")) {
            const String rest = arg.substring(4);
            const int a = rest.indexOf(' ');
            const String ssid = rest.substring(0, a);
            String pass = rest.substring(a + 1);
            int prio = 10;
            const int b = pass.lastIndexOf(' ');
            if (b > 0 && pass.substring(b + 1).toInt() > 0) { prio = pass.substring(b + 1).toInt(); pass = pass.substring(0, b); }
            net::addNetwork(ssid, pass, prio);
        } else if (arg == "clear") {
            net::clearNetworks();
        }
        net::printNetworks();
    }
    else if (cmd == "log") {
        if (arg == "clear") { LittleFS.remove("/events.jsonl"); LittleFS.remove("/events.old.jsonl"); Serial.println("Log gelöscht"); }
        else {
            File f = LittleFS.open("/events.jsonl", "r");
            if (!f) Serial.println("(leer)");
            else { while (f.available()) Serial.write(f.read()); f.close(); }
        }
    }
    else Serial.printf("Unbekannter Befehl '%s' – 'help' zeigt alle Befehle\n", line.c_str());
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nARC-Slate " ARC_FW_VERSION " startet ...");

    state::begin();
    if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS nicht verfügbar");

    spibus::begin();
    mtx::begin();
    mtx::showOverlay("ARC SLATE", 0);
    mtx::startTask();

    state::lock();
    const bool sqwFalling = state::settings().sqwFalling;
    const bool sqwKnown = state::settings().sqwEdgeKnown;
    state::unlock();
    arcclock::begin(sqwFalling);
    if (arcclock::rtcPresent() && !sqwKnown) detectSqwEdge();

    ltc::onStatusChange(onLtcStatus);
    ltc::begin();
    clap::begin();
    ui::begin();
    net::begin();

    epd::setStatusProvider(net::statusLine);
    epd::begin();          // boot screen (blocks ~4 s, LEDs keep running)
    epd::startTask();
    epd::requestCard();
    mtx::clearOverlay();

    printHelp();
}

void loop() {
    ltc::loop();
    net::loop();
    state::saveIfDirty();
    if (s_statusChanged) {
        s_statusChanged = false;
        net::broadcastStatus();
        epd::requestStatus();
    }
    handleSerial();
    vTaskDelay(pdMS_TO_TICKS(5));
}
