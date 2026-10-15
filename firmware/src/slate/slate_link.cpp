#include "slate_link.h"
#include <ArcCore.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "clap.h"
#include "epaper.h"
#include "matrix.h"
#include "state.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#ifndef ARC_FW_VERSION
#define ARC_FW_VERSION "dev"
#endif

namespace {

const char* const kCaps[] = {"ltc_gen", "ltc_jam", "clap", "flash", "matrix", "eink", "encoder", "schedule"};

int toInt(JsonVariantConst v, int fallback) {
    if (v.is<int>()) return v.as<int>();
    if (v.is<const char*>()) return String(v.as<const char*>()).toInt();
    return fallback;
}

void handleShot(JsonDocument& doc) {
    state::lock();
    ShotInfo& s = state::shot();
    if (!doc["scene"].isNull()) s.scene = toInt(doc["scene"], s.scene);
    if (!doc["shot"].isNull()) s.shot = doc["shot"].as<String>();
    if (!doc["take"].isNull()) s.take = toInt(doc["take"], s.take);
    if (!doc["roll"].isNull()) state::setRollFromString(s, doc["roll"].as<String>());
    if (!doc["code"].isNull()) s.code = doc["code"].as<String>();
    if (!doc["production"].isNull()) s.production = doc["production"].as<String>();
    if (!doc["director"].isNull()) s.director = doc["director"].as<String>();
    if (!doc["camera"].isNull()) s.camera = doc["camera"].as<String>();
    if (!doc["environment"].isNull()) s.environment = doc["environment"].as<String>();
    if (!doc["daynight"].isNull()) s.daynight = doc["daynight"].as<String>();
    if (!doc["mos"].isNull()) s.mos = doc["mos"].as<bool>();
    if (!doc["starttime"].isNull()) s.starttime = doc["starttime"].as<String>();
    if (!doc["endtime"].isNull()) s.endtime = doc["endtime"].as<String>();
    if (!doc["speed"].isNull()) s.speed = doc["speed"].as<float>();
    double fps = 0;
    if (doc["framerate"].is<const char*>()) fps = String(doc["framerate"].as<const char*>()).toFloat();
    else if (!doc["framerate"].isNull()) fps = doc["framerate"].as<double>();
    s.loaded = true;
    state::touch();
    state::unlock();
    arc::TcRate r;
    if (arc::rateFromFps(fps, r)) arc::engine::setRate(r);
    slate::updateUserBits();
    epd::requestCard();
}

// Device-specific messages; returns true if handled (generic ones go to ARC-Core).
bool onMessage(const String& type, JsonDocument& doc, AsyncWebSocketClient*) {
    if (type == "shot") { handleShot(doc); return true; }
    if (type == "settings") {
        if (!doc["brightness"].isNull()) {
            const uint8_t b = (uint8_t)constrain(doc["brightness"].as<int>(), 0, 15);
            state::lock(); state::settings().brightness = b; state::touch(); state::unlock();
            mtx::setBrightness(b);
        }
        if (!doc["flashLatencyUs"].isNull()) {
            state::lock(); state::settings().flashLatencyUs = doc["flashLatencyUs"].as<int32_t>(); state::touch(); state::unlock();
        }
        arc::net::broadcastStatus();
        return true;
    }
    if (type == "command") {
        const String cmd = doc["cmd"] | "";
        if (cmd == "flash_test") { clap::testFlash(); return true; }
        if (cmd == "seek") {
            const double secs = doc["time"].as<double>();
            arc::engine::setTimecode(arc::framesToTc((uint32_t)(secs * arc::realFps(arc::engine::rate())), arc::engine::rate()));
            return true;
        }
        if (cmd == "set_gen_mode" && String(doc["val"] | "") == "playback") return true;   // not on the slate
        return false;
    }
    // Other devices' messages (song_info, uploads, status) are ignored.
    return type == "song_info" || type == "prepare_upload" || type == "finish_upload" || type == "status";
}

void fillStatus(JsonDocument& doc) {
    doc["roll"] = state::rollString();
    state::lock();
    const ShotInfo& s = state::shot();
    doc["scene"] = s.scene;
    doc["shot"] = s.shot;
    doc["take"] = s.take;
    doc["brightness"] = state::settings().brightness;
    state::unlock();
    doc["clapClosed"] = clap::closed();
}

void onClientChange(bool) { epd::requestStatus(); }

}  // namespace

namespace slate {

void begin() {
    arc::net::Config cfg;
    cfg.hostname = "arc-slate";
    cfg.deviceType = "slate";
    cfg.device = "slate";
    cfg.firmware = ARC_FW_VERSION;
    cfg.caps = kCaps;
    cfg.capCount = sizeof(kCaps) / sizeof(kCaps[0]);
    cfg.defaults = kDefaultWifi;
    cfg.defaultCount = sizeof(kDefaultWifi) / sizeof(kDefaultWifi[0]);
    cfg.arcNetPassword = kArcNetPassword;
    cfg.onMessage = onMessage;
    cfg.fillStatus = fillStatus;
    cfg.onClientChange = onClientChange;
    arc::net::begin(cfg);
    updateUserBits();
}

void loop() {
    // Date user bits change at midnight; shot bits whenever the shot changes.
    static uint32_t lastMs = 0;
    if (millis() - lastMs > 60000) {
        lastMs = millis();
        updateUserBits();
    }
}

void updateUserBits() {
    state::lock();
    const ShotInfo s = state::shot();
    state::unlock();
    uint32_t bits;
    if (s.loaded) {
        bits = arc::shotUserBits(s.cam, s.rollNum, s.scene, s.shot.toInt(), s.take);
    } else {
        const DateTime d = arc::clock::now();
        bits = arc::dateUserBits(d.day(), d.month(), d.year(), arc::isEuSummerTime(d.year(), d.month(), d.day(), d.hour()));
    }
    arc::engine::setUserBits(bits);
}

void sendSlateState() {
    JsonDocument doc;
    doc["type"] = "slateState";
    doc["device"] = "slate";
    doc["id"] = arc::net::deviceId();
    state::lock();
    const ShotInfo& s = state::shot();
    doc["cam"] = String(s.cam);
    doc["rollNum"] = s.rollNum;
    doc["scene"] = s.scene;
    doc["shot"] = s.shot;
    doc["take"] = s.take;
    state::unlock();
    doc["roll"] = state::rollString();
    arc::net::send(doc);
}

namespace {
struct TakeScan { String scene, shot; int maxTake; };

bool scanTakes(JsonDocument& ev, void* ctx) {
    TakeScan& t = *(TakeScan*)ctx;
    if (String(ev["kind"] | "") != "clap_close") return true;
    if (String(ev["scene"] | 0) == t.scene && String(ev["shot"] | "") == t.shot) {
        const int take = ev["take"] | 0;
        if (take > t.maxTake) t.maxTake = take;
    }
    return true;
}
}  // namespace

bool stepShotOffline(int dir) {
    const int count = arc::project::shotCount();
    if (count == 0) return false;
    state::lock();
    const String scene = String(state::shot().scene);
    const String shot = state::shot().shot;
    state::unlock();
    int idx = arc::project::indexOf(scene, shot);
    idx = idx < 0 ? 0 : ((idx + dir) % count + count) % count;
    JsonDocument msg;
    if (!arc::project::shotMessage(idx, msg)) return false;
    // Next take of that shot from the device's own clap log.
    TakeScan scan{String(msg["scene"].is<const char*>() ? String(msg["scene"].as<const char*>()).toInt() : msg["scene"].as<int>()),
                  msg["shot"].as<String>(), 0};
    arc::events::forEachAfter(0, scanTakes, &scan);
    msg["take"] = scan.maxTake + 1;
    handleShot(msg);
    sendSlateState();
    return true;
}

void requestShotStep(int dir) {
    JsonDocument doc;
    doc["type"] = dir > 0 ? "nextShot" : "previousShot";
    doc["device"] = "slate";
    arc::net::send(doc);
}

String statusLine() {
    using arc::engine::Mode;
    String line = arc::net::statusLine() + "  |  ";
    switch (arc::engine::mode()) {
        case Mode::FreeRun:   line += "Free Run"; break;
        case Mode::RecordRun: line += arc::engine::running() ? "Rec Run REC" : "Rec Run STOP"; break;
        case Mode::Slave:     line += "Slave"; break;
        case Mode::JamFree:
        case Mode::JamRec: {
            const arc::engine::JamStatus j = arc::engine::jam();
            line += j.state == arc::engine::JamStatus::State::Verifying ? "Jam verify " + String(j.verifyRemainingS) + "s"
                                                                       : "Jam...";
            break;
        }
        default: break;
    }
    line += " " + String(arc::rateKey(arc::engine::rate())) + " fps";
    if (arc::engine::jam().muteHeld) line += "  |  UNPLUG CABLE";
    if (!arc::clock::sqwActive()) line += "  |  RTC SQW missing";
    return line;
}

}  // namespace slate
