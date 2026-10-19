/*
 * ARC-Slate – electronic clapperboard on the ARC_AP board (ESP32-S3).
 *
 * ARC-Core (shared with ARC-Play): RTC wall clock, phase-locked LTC generator,
 * decoder, Jam Sync, WiFi/ARC-Net, protocol v2, event log.
 *
 * Slate modules
 *   matrix      2 MAX7219 chains: timecode (7) and roll/shot/scene+take (9)
 *   epaper      7.5" e-ink: slate card with QR code, menu, status bar
 *   ui          rotary encoder + menu
 *   clap        hall sensor, sync flash, clap events
 *   slate_link  slate protocol (shot data, slate state, user bits)
 *   state       shot data + settings (NVS)
 *
 * Pins: pins.h · Wiring: hardware/wiring/ARC-Slate_Verdrahtung.svg
 * Serial console: 115200 baud, "help" lists the commands.
 */
#include <Arduino.h>
#include <ArcCore.h>
#include <Wire.h>
#include "bootsel.h"
#include "clap.h"
#include "epaper.h"
#include "epd_driver.h"
#include "matrix.h"
#include "matrix_slate.h"
#include "pins.h"
#include "slate_link.h"
#include "spi_bus.h"
#include "state.h"
#include "ui.h"

using arc::engine::Mode;
using arc::TcRate;

namespace {

volatile bool s_statusChanged = false;
void onEngineStatus() { s_statusChanged = true; }

void onJam(const arc::engine::JamStatus& j) {
    JsonDocument ev;
    ev["type"] = "event";
    ev["kind"] = "jam";
    ev["device"] = "slate";
    ev["deviceId"] = arc::net::deviceId();
    ev["id"] = arc::events::nextId();
    ev["rtcUs"] = arc::clock::nowUs();
    ev["ok"] = j.state == arc::engine::JamStatus::State::Done;
    ev["residualUs"] = j.residualUs;
    ev["ppm"] = j.ppm;
    ev["aging"] = j.aging;
    arc::net::send(ev);
    arc::events::append(ev);
    mtx::showOverlay("UNPLUG", 3000);
}

void printHelp() {
    Serial.println(
        "\n=== ARC-Slate " ARC_FW_VERSION " – serielle Befehle ===\n"
        "  ?                Status\n"
        "  1 2 4 5 6        Modus: Free Run, Record Run, Slave, Jam->Free, Jam->Rec\n"
        "  q w e r          Bildrate: 30, 25, 29.97 DF, 24\n"
        "  s / x            Start-Stop (Record Run) / Stop\n"
        "  t | tc HH:MM:SS:FF   TC auf 0 / setzen (Record Run)\n"
        "  u                Jam: Kabel abgezogen bestaetigen (TC-Ausgang wieder an)\n"
        "  n                NTP-Sync erzwingen\n"
        "  time YYYY-MM-DD HH:MM:SS   RTC stellen\n"
        "  sqw              SQW-Flanke neu messen\n"
        "  c / f            Klappe simulieren / Blitz testen\n"
        "  mtx 0|1|2        Matrix normal / alle LEDs / Modulnummern\n"
        "  epd              E-Ink neu zeichnen\n"
        "  epdtest          E-Ink Testbild | epdspi HZ  E-Ink-Takt setzen (Standard 2000000)\n"
        "  epdstats         E-Ink: Refreshes, Booster-Fehler, Dauer\n"
        "  spidrive 0-3     Treiberstaerke SCK/MOSI (0 = schwach, weniger Ueberschwinger; Standard 0)\n"
        "  enc / encrev     Encoder-Debug / Drehrichtung umkehren\n"
        "  i2c              I2C-Scan\n"
        "  take N | scene N | shot X | roll A001 | bright [day|night] 0-15 (folgt dem Sonnenstand)\n"
        "  lat flash US     Blitz-Latenz setzen (Mikrosekunden)\n"
        "  wifi | wifi add SSID PASS [PRIO] | wifi clear\n"
        "  log | log clear  Ereignis-Log anzeigen / loeschen\n");
}

void printStatus() {
    const arc::Timecode tc = arc::engine::current();
    char tcs[12];
    arc::formatTc(tc, tcs);
    const int64_t now = arc::clock::nowNs();
    const DateTime d((uint32_t)(now / 1000000000LL));
    state::lock();
    const ShotInfo s = state::shot();
    const Settings st = state::settings();
    state::unlock();
    const arc::engine::JamStatus j = arc::engine::jam();
    Serial.printf("TC %s  Modus %s  %s fps  %s\n", tcs, arc::engine::modeKey(arc::engine::mode()),
                  arc::rateKey(arc::engine::rate()), arc::engine::running() ? "läuft" : "steht");
    Serial.printf("RTC %04d-%02d-%02d %02d:%02d:%02d.%06lu  SQW %s (%s)  Trim %lld us  ESP-µs/s %.1f  %.2f °C  Aging %d\n",
                  d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(),
                  (unsigned long)((now / 1000) % 1000000), arc::clock::sqwActive() ? "ok" : "FEHLT",
                  st.sqwFalling ? "fallend" : "steigend", (long long)(arc::clock::trimNs() / 1000),
                  arc::clock::espUsPerRtcSecond(), arc::clock::temperature(), (int)arc::clock::agingOffset());
    Serial.printf("Ausgang: Latenz %.0f us %s, Phasenfehler %.1f us, Slips %lu, Neuausrichtungen %lu\n",
                  arc::engine::outputLatencyUs(), arc::engine::latencyCalibrated() ? "(kalibriert)" : "(Standard)",
                  arc::engine::phaseErrorUs(), (unsigned long)arc::engine::generator().slips(),
                  (unsigned long)arc::engine::generator().realigns());
    static const char* const js[] = {"idle", "hoert zu", "prueft", "fertig", "fehlgeschlagen"};
    Serial.printf("Jam: %s, ok %lu Frames, Rest %ld us, Drift %.2f ppm%s\n", js[(int)j.state], (unsigned long)j.okFrames,
                  (long)j.residualUs, j.ppm, j.muteHeld ? "  [Ausgang stumm bis Kabel ab]" : "");
    Serial.printf("Userbits TX %08lX  RX %08lX  empfangene Frames %lu\n", (unsigned long)arc::engine::txUserBits(),
                  (unsigned long)arc::engine::rxUserBits(), (unsigned long)arc::ltcin::framesReceived());
    Serial.printf("Shot: Roll %s  Szene %d  Shot %s  Take %d  Code %s\n", state::rollString().c_str(), s.scene,
                  s.shot.c_str(), s.take, s.code.c_str());
    Serial.printf("Netz: %s\n", slate::statusLine().c_str());
    Serial.printf("Klappe %s, Ereignisse %lu, Log %u Bytes, Heap %lu\n", clap::closed() ? "zu" : "offen",
                  (unsigned long)clap::eventCount(), (unsigned)arc::events::size(), (unsigned long)ESP.getFreeHeap());
}

void detectSqwEdge() {
    bool falling = true;
    int32_t offsetMs = 0;
    Serial.println("[CLK] Messe, welche SQW-Flanke den Sekundenwechsel markiert ...");
    if (!arc::clock::detectSqwEdge(falling, offsetMs)) {
        Serial.println("[CLK] Messung fehlgeschlagen (RTC/SQW prüfen)");
        return;
    }
    Serial.printf("[CLK] Sekundenwechsel = %s Flanke (Abstand %ld ms)\n", falling ? "fallende" : "steigende", (long)offsetMs);
    state::lock();
    state::settings().sqwFalling = falling;
    state::settings().sqwEdgeKnown = true;
    state::touch();
    state::unlock();
    arc::clock::useEdge(falling);
}

void i2cScan() {
    arc::clock::lockI2C();
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0)
            Serial.printf("  I2C-Gerät 0x%02X%s\n", a, a == 0x68 ? " (DS3231)" : a == 0x57 ? " (EEPROM)" : "");
    }
    arc::clock::unlockI2C();
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
    slate::updateUserBits();
    slate::sendSlateState();
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
    else if (cmd == "1") arc::engine::requestMode(Mode::FreeRun);
    else if (cmd == "2") arc::engine::requestMode(Mode::RecordRun);
    else if (cmd == "4") arc::engine::requestMode(Mode::Slave);
    else if (cmd == "5") arc::engine::requestMode(Mode::JamFree);
    else if (cmd == "6") arc::engine::requestMode(Mode::JamRec);
    else if (cmd == "q") arc::engine::setRate(TcRate::R30);
    else if (cmd == "w") arc::engine::setRate(TcRate::R25);
    else if (cmd == "e") arc::engine::setRate(TcRate::R2997DF);
    else if (cmd == "r") arc::engine::setRate(TcRate::R24);
    else if (cmd == "s") { if (arc::engine::running()) arc::engine::stop(); else arc::engine::start(); }
    else if (cmd == "x") arc::engine::stop();
    else if (cmd == "t") arc::engine::setTimecode(arc::Timecode());
    else if (cmd == "tc") { arc::Timecode tc; if (arc::parseTc(arg.c_str(), tc)) arc::engine::setTimecode(tc); }
    else if (cmd == "u") arc::engine::confirmUnplug();
    else if (cmd == "n") { arc::net::requestNtpSync(); Serial.println("NTP-Sync angefordert (nur im WLAN mit Internet)"); }
    else if (cmd == "time") {
        int y, mo, da, h, mi, se;
        if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &da, &h, &mi, &se) == 6) {
            arc::clock::setTime(DateTime(y, mo, da, h, mi, se));
        }
    }
    else if (cmd == "sqw") detectSqwEdge();
    else if (cmd == "c") clap::simulate();
    else if (cmd == "f") clap::testFlash();
    else if (cmd == "mtx") mtx::setTestMode((uint8_t)arg.toInt());
    else if (cmd == "epd") epd::requestCard();
    else if (cmd == "epdtest") epd::requestTest();
    else if (cmd == "epdpins") epd::probePins();
    else if (cmd == "epdlab") epd::requestLab();
    else if (cmd == "spidrive") {
        if (arg.length()) { spibus::lock(); spibus::setDrive((uint8_t)constrain(arg.toInt(), 0, 3)); spibus::unlock(); }
        Serial.printf("SPI drive strength (SCK/MOSI): %u  (0 weakest .. 3 strongest)\n", spibus::drive());
    }
    else if (cmd == "epdstats") {
        const epddrv::Stats st = epddrv::stats();
        Serial.println(String("E-Ink: ") + st.refreshes + " refreshes, " + st.failures + " booster failures, last " + st.lastMs + " ms");
    }
    else if (cmd == "epdpartial") { epd::setPartial(arg.toInt() != 0); }
    else if (cmd == "epdspi") {
        if (arg.toInt() > 0) epddrv::setSpiHz((uint32_t)arg.toInt());
        Serial.printf("E-Ink SPI-Takt: %lu Hz\n", (unsigned long)epddrv::spiHz());
    }
    else if (cmd == "enc") { static bool dbg = false; dbg = !dbg; ui::setEncoderDebug(dbg); Serial.printf("Encoder-Debug %s\n", dbg ? "an" : "aus"); }
    else if (cmd == "encrev") {
        state::lock(); state::settings().encReverse = !state::settings().encReverse; state::touch(); state::unlock();
        Serial.println("Drehrichtung umgekehrt (wirkt nach Neustart)");
    }
    else if (cmd == "i2c") i2cScan();
    else if (cmd == "take" || cmd == "scene" || cmd == "shot" || cmd == "roll") setShotValue(cmd, arg);
    else if (cmd == "bright") {
        // bright N: value of the current phase | bright day N | bright night N (the renderer applies it)
        const int sp = arg.indexOf(' ');
        const String which = sp < 0 ? String() : arg.substring(0, sp);
        const uint8_t b = (uint8_t)constrain(arg.substring(sp + 1).toInt(), 0, 15);
        const bool night = which == "night" || (which.isEmpty() && slatematrix::isNight());
        state::lock();
        (night ? state::settings().brightnessNight : state::settings().brightness) = b;
        state::touch();
        const uint8_t d = state::settings().brightness, n = state::settings().brightnessNight;
        state::unlock();
        Serial.printf("Helligkeit: Tag %u, Nacht %u (jetzt %s)\n", d, n, slatematrix::isNight() ? "Nacht" : "Tag");
    }
    else if (cmd == "lat" && arg.startsWith("flash ")) {
        state::lock(); state::settings().flashLatencyUs = arg.substring(6).toInt(); state::touch(); state::unlock();
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
            arc::net::addNetwork(ssid, pass, prio);
        } else if (arg == "clear") {
            arc::net::clearNetworks();
        }
        arc::net::printNetworks(Serial);
    }
    else if (cmd == "mode") {
        if (arg == "clock") bootsel::switchTo(bootsel::App::Clock);
        else Serial.println("mode clock  – ARC-Clock starten");
    }
    else if (cmd == "log") {
        if (arg == "clear") { arc::events::clear(); Serial.println("Log gelöscht"); }
        else arc::events::print(Serial);
    }
    else Serial.printf("Unbekannter Befehl '%s' – 'help' zeigt alle Befehle\n", line.c_str());
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nARC-Slate " ARC_FW_VERSION " startet ...");

    state::begin();
    arc::events::begin();
    arc::project::begin();

    spibus::begin();
    state::lock();
    const uint8_t brightness = state::settings().brightness;
    state::unlock();
    mtx::begin(brightness);
    slatematrix::begin();
    mtx::showOverlay("ARC SLATE", 0);
    mtx::startTask();

    state::lock();
    const bool sqwFalling = state::settings().sqwFalling;
    const bool sqwKnown = state::settings().sqwEdgeKnown;
    TcRate rate = TcRate::R25;
    arc::rateFromFps(state::shot().fps, rate);
    state::unlock();
    arc::clock::begin({PIN_RTC_SDA, PIN_RTC_SCL, PIN_RTC_SQW}, sqwFalling);
    if (arc::clock::rtcPresent() && !sqwKnown) detectSqwEdge();

    arc::engine::Config ec;
    ec.dacBck = PIN_DAC_BCK;
    ec.dacLrck = PIN_DAC_LRCK;
    ec.dacDin = PIN_DAC_DIN;
    ec.dacMute = PIN_PCM_XSMT;
    ec.ltcIn = PIN_LTC_IN;
    arc::engine::onStatusChange(onEngineStatus);
    arc::engine::onJamEvent(onJam);
    arc::engine::begin(ec, rate);

    clap::begin();
    ui::begin();
    slate::begin();

    epd::setStatusProvider(slate::statusLine);
    epd::begin();          // boot screen (blocks ~4 s, LEDs keep running)
    epd::startTask();
    epd::requestCard();
    mtx::clearOverlay();

    printHelp();
}

void loop() {
    arc::engine::loop();
    arc::net::loop();
    slate::loop();
    state::saveIfDirty();
    if (s_statusChanged) {
        s_statusChanged = false;
        // Keep the stored frame rate in sync with the engine.
        state::lock();
        const float fps = (float)arc::realFps(arc::engine::rate());
        if (fabsf(state::shot().fps - fps) > 0.001f) { state::shot().fps = fps; state::touch(); }
        state::unlock();
        arc::net::broadcastStatus();
        epd::requestStatus();
    }
    handleSerial();
    vTaskDelay(pdMS_TO_TICKS(5));
}
