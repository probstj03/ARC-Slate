#include "net.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <sys/time.h>
#include "arc_clock.h"
#include "clap.h"
#include "epaper.h"
#include "ltc_engine.h"
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

constexpr const char* HOSTNAME = "arc-slate";
constexpr const char* ARC_NET_SSID = "ARC-Net";
constexpr const char* TZ_EUROPE_BERLIN = "CET-1CEST,M3.5.0,M10.5.0/3";

struct KnownNet { String ssid; String pass; int priority; };

AsyncWebServer s_server(80);
AsyncWebSocket s_ws("/ws");
SemaphoreHandle_t s_netMtx = nullptr;
std::vector<KnownNet> s_known;
String s_arcPass;

enum NetRole { NET_OFFLINE, NET_STATION, NET_ARCNET_CLIENT, NET_ARCNET_AP };
volatile NetRole s_role = NET_OFFLINE;
String s_ssid;
volatile bool s_serverStarted = false;
volatile bool s_statusDirty = false;
volatile bool s_ntpRequested = false;
volatile int64_t s_setTimeUs = 0;       // deferred "set_time" from ARC-Studio
String s_deviceId;

// ── known networks (NVS) ─────────────────────────────────────────────────
void loadKnown() {
    Preferences p;
    p.begin("wifi", true);
    const String json = p.getString("list", "");
    s_arcPass = p.getString("arcPass", kArcNetPassword);
    p.end();
    s_known.clear();
    if (json.length()) {
        JsonDocument doc;
        if (deserializeJson(doc, json) == DeserializationError::Ok) {
            for (JsonObject n : doc.as<JsonArray>()) {
                s_known.push_back({n["s"] | "", n["p"] | "", n["r"] | 0});
            }
        }
        return;
    }
    for (const auto& d : kDefaultWifi) s_known.push_back({d.ssid, d.pass, d.priority});
}

void saveKnown() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& n : s_known) {
        JsonObject o = arr.add<JsonObject>();
        o["s"] = n.ssid; o["p"] = n.pass; o["r"] = n.priority;
    }
    String json;
    serializeJson(doc, json);
    Preferences p;
    p.begin("wifi", false);
    p.putString("list", json);
    p.putString("arcPass", s_arcPass);
    p.end();
}

// Simple FNV-1a hash so ARC-Studio can see whether its list is on the device.
String knownHash() {
    uint32_t h = 2166136261u;
    auto mix = [&h](const String& s) { for (char c : s) { h ^= (uint8_t)c; h *= 16777619u; } h ^= 0xFF; h *= 16777619u; };
    for (const auto& n : s_known) { mix(n.ssid); mix(n.pass); mix(String(n.priority)); }
    mix(s_arcPass);
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
    return String(buf);
}

// ── messages ─────────────────────────────────────────────────────────────
void sendAll(JsonDocument& doc) {
    if (!s_serverStarted || s_ws.count() == 0) return;
    String out;
    serializeJson(doc, out);
    s_ws.textAll(out);
}

String isoTimestamp(int64_t rtcUs) {
    const DateTime d((uint32_t)(rtcUs / 1000000LL));
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d", d.year(), d.month(), d.day(), d.hour(),
             d.minute(), d.second(), (int)((rtcUs / 1000) % 1000));
    return String(buf);
}

void fillStatus(JsonDocument& doc, const char* type) {
    const int64_t rtcUs = arcclock::nowUs();
    doc["type"] = type;
    doc["device"] = "slate";
    doc["id"] = s_deviceId;
    doc["proto"] = 2;
    doc["fw"] = ARC_FW_VERSION;
    doc["gen_mode"] = ltc::modeKey();
    doc["tc_mode"] = ltc::rateKey();
    doc["running"] = ltc::running();
    doc["timecode"] = ltc::tcToString(ltc::current());
    doc["timestamp"] = isoTimestamp(rtcUs);   // legacy (ms)
    doc["rtcUs"] = rtcUs;
    doc["roll"] = state::rollString();
    state::lock();
    const ShotInfo& s = state::shot();
    doc["scene"] = s.scene;
    doc["shot"] = s.shot;
    doc["take"] = s.take;
    doc["brightness"] = state::settings().brightness;
    state::unlock();
    JsonObject jam = doc["jam"].to<JsonObject>();
    jam["okCnt"] = ltc::jamOkCount();
    jam["rxFps"] = ltc::rxFps();
    jam["muteHeld"] = ltc::muteHeld();
    doc["sqw"] = arcclock::sqwActive();
}

void sendHello(AsyncWebSocketClient* client) {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["device"] = "slate";
    doc["id"] = s_deviceId;
    doc["fw"] = ARC_FW_VERSION;
    doc["proto"] = 2;
    JsonArray caps = doc["caps"].to<JsonArray>();
    for (const char* c : {"ltc_gen", "ltc_jam", "clap", "flash", "matrix", "eink", "encoder"}) caps.add(c);
    doc["wifiHash"] = knownHash();
    String out;
    serializeJson(doc, out);
    client->text(out);
}

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
    float fps = doc["framerate"].isNull() ? 0.0f : doc["framerate"].as<float>();
    if (doc["framerate"].is<const char*>()) fps = String(doc["framerate"].as<const char*>()).toFloat();
    s.loaded = true;
    state::touch();
    state::unlock();
    TcRate r;
    if (ltc::rateFromFps(fps, r)) ltc::setRate(r);
    ltc::refreshUserBits();
    epd::requestCard();
}

void handleCommand(JsonDocument& doc) {
    const String cmd = doc["cmd"] | "";
    if (cmd == "start" || cmd == "play") ltc::start();
    else if (cmd == "stop" || cmd == "pause") ltc::stop();
    else if (cmd == "reset") ltc::resetTimecode();
    else if (cmd == "seek") {
        const float secs = doc["time"].as<float>();
        ltc::setTimecode(ltc::framesToTc((uint32_t)(secs * ltc::fps()), ltc::rate()));
    } else if (cmd == "set_tc_mode") {
        TcRate r;
        if (ltc::rateFromKey(doc["val"] | "", r)) ltc::setRate(r);
    } else if (cmd == "set_gen_mode") {
        SystemMode m;
        if (ltc::modeFromKey(doc["val"] | "", m)) ltc::requestMode(m);   // "playback" is ignored
    } else if (cmd == "sync_rtc") s_ntpRequested = true;
    else if (cmd == "restart") ltc::requestMode(ltc::mode());
    else if (cmd == "flash_test") clap::testFlash();
    else if (cmd == "jam_confirm_unplug") ltc::releaseMuteHold();
    s_statusDirty = true;
}

void handleWifiConfig(JsonDocument& doc) {
    xSemaphoreTake(s_netMtx, portMAX_DELAY);
    if (doc["networks"].is<JsonArray>()) {
        s_known.clear();
        for (JsonObject n : doc["networks"].as<JsonArray>()) {
            s_known.push_back({n["ssid"] | "", n["password"] | "", n["priority"] | 0});
        }
    }
    if (!doc["arcNet"]["password"].isNull()) s_arcPass = doc["arcNet"]["password"].as<String>();
    saveKnown();
    const String hash = knownHash();
    xSemaphoreGive(s_netMtx);
    JsonDocument ack;
    ack["type"] = "wifi_config_ack";
    ack["device"] = "slate";
    ack["wifiHash"] = hash;
    sendAll(ack);
}

void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] ARC-Studio verbunden: %s\n", client->remoteIP().toString().c_str());
        sendHello(client);
        JsonDocument init;
        fillStatus(init, "init_sync");
        init.remove("timecode");   // legacy: init_sync has no timecode
        String out;
        serializeJson(init, out);
        client->text(out);
        s_statusDirty = true;
        epd::requestStatus();
        return;
    }
    if (type == WS_EVT_DISCONNECT) {
        Serial.println("[WS] ARC-Studio getrennt");
        epd::requestStatus();
        return;
    }
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->opcode != WS_TEXT || !info->final || info->index != 0 || info->len != len) return;

    JsonDocument doc;
    if (deserializeJson(doc, (const char*)data, len) != DeserializationError::Ok) return;
    const String msgType = doc["type"] | "";
    if (msgType == "shot") handleShot(doc);
    else if (msgType == "command") handleCommand(doc);
    else if (msgType == "settings") {
        if (!doc["brightness"].isNull()) {
            const uint8_t b = (uint8_t)constrain(doc["brightness"].as<int>(), 0, 15);
            state::lock(); state::settings().brightness = b; state::touch(); state::unlock();
            mtx::setBrightness(b);
        }
        s_statusDirty = true;
    } else if (msgType == "wifi_config") handleWifiConfig(doc);
    else if (msgType == "ping") {
        JsonDocument pong;
        pong["type"] = "pong";
        pong["device"] = "slate";
        pong["t0"] = doc["t0"];
        pong["rtcUs"] = arcclock::nowUs();
        String out;
        serializeJson(pong, out);
        client->text(out);
    } else if (msgType == "set_time") {
        s_setTimeUs = (int64_t)doc["epochMs"].as<double>() * 1000LL;   // local time, coarse
    }
    // Other device types' messages ("status", "song_info", uploads) are ignored.
}

// ── WiFi connection ──────────────────────────────────────────────────────
bool connectTo(const String& ssid, const String& pass, uint32_t timeoutMs) {
    Serial.printf("[WiFi] Verbinde mit '%s' ...", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf(" OK (%s)\n", WiFi.localIP().toString().c_str());
        s_ssid = ssid;
        return true;
    }
    Serial.println(" fehlgeschlagen");
    WiFi.disconnect(true);
    delay(100);
    return false;
}

bool scanContains(int n, const char* ssid) {
    for (int i = 0; i < n; i++) if (WiFi.SSID(i) == ssid) return true;
    return false;
}

void startServices() {
    if (!MDNS.begin(HOSTNAME)) Serial.println("[mDNS] Start fehlgeschlagen");
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "device_type", "slate");
    MDNS.addServiceTxt("http", "tcp", "id", s_deviceId.c_str());
    MDNS.addServiceTxt("http", "tcp", "proto", "2");
    if (!s_serverStarted) {
        s_ws.onEvent(onWsEvent);
        s_server.addHandler(&s_ws);
        s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->send(200, "text/plain", "ARC-Slate " ARC_FW_VERSION " – WebSocket: /ws");
        });
        s_server.begin();
        s_serverStarted = true;
    }
}

// Sets the RTC from NTP only if it is clearly wrong (> 2 s): after a jam the
// RTC is more precise than NTP and must not be overwritten.
void syncNtp(bool force) {
    configTzTime(TZ_EUROPE_BERLIN, "pool.ntp.org", "time.nist.gov");
    struct tm ti;
    if (!getLocalTime(&ti, 5000)) { Serial.println("[NTP] keine Antwort"); return; }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm lt;
    localtime_r(&tv.tv_sec, &lt);
    const DateTime local(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    const int64_t localUs = (int64_t)local.unixtime() * 1000000LL + tv.tv_usec;
    const int64_t diffMs = (localUs - arcclock::nowUs()) / 1000;
    if (force || llabs(diffMs) > 2000 || arcclock::rtc().lostPower()) {
        arcclock::setTimeUs(localUs);
        Serial.printf("[NTP] RTC gestellt (Abweichung war %lld ms)\n", (long long)diffMs);
    } else {
        Serial.printf("[NTP] RTC ok (Abweichung %lld ms, nicht verändert)\n", (long long)diffMs);
    }
}

void connectTask(void*) {
    WiFi.persistent(false);
    WiFi.setHostname(HOSTNAME);
    for (;;) {
        if (s_role == NET_STATION || s_role == NET_ARCNET_CLIENT) {
            if (WiFi.status() == WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            Serial.println("[WiFi] Verbindung verloren");
            s_role = NET_OFFLINE;
            epd::requestStatus();
        }
        if (s_role == NET_ARCNET_AP) {
            // Own network: look for known networks once a minute while nobody uses it.
            vTaskDelay(pdMS_TO_TICKS(60000));
            if (WiFi.softAPgetStationNum() > 0 || s_ws.count() > 0) continue;
            WiFi.mode(WIFI_AP_STA);
            const int n = WiFi.scanNetworks();
            bool found = false;
            xSemaphoreTake(s_netMtx, portMAX_DELAY);
            for (const auto& k : s_known) if (scanContains(n, k.ssid.c_str())) found = true;
            xSemaphoreGive(s_netMtx);
            if (!found) continue;
            WiFi.softAPdisconnect(true);
            s_role = NET_OFFLINE;
        }

        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);   // no power-save latency spikes
        const int n = WiFi.scanNetworks();

        // 1) known networks, highest priority first, then signal strength
        xSemaphoreTake(s_netMtx, portMAX_DELAY);
        std::vector<KnownNet> known = s_known;
        xSemaphoreGive(s_netMtx);
        std::vector<std::pair<int, int>> candidates;   // (known index, rssi)
        for (size_t k = 0; k < known.size(); k++)
            for (int i = 0; i < n; i++)
                if (WiFi.SSID(i) == known[k].ssid) candidates.push_back({(int)k, WiFi.RSSI(i)});
        std::sort(candidates.begin(), candidates.end(), [&](const std::pair<int,int>& a, const std::pair<int,int>& b) {
            if (known[a.first].priority != known[b.first].priority) return known[a.first].priority > known[b.first].priority;
            return a.second > b.second;
        });
        bool ok = false;
        for (const auto& c : candidates) {
            if (connectTo(known[c.first].ssid, known[c.first].pass, 8000)) { s_role = NET_STATION; ok = true; break; }
        }

        // 2) existing ARC-Net (created by ARC-Play or another device)
        if (!ok && scanContains(n, ARC_NET_SSID)) {
            if (connectTo(ARC_NET_SSID, s_arcPass, 8000)) { s_role = NET_ARCNET_CLIENT; ok = true; }
        }

        // 3) nobody there: wait a random time (so two devices don't both
        //    start an AP), look again, then open ARC-Net ourselves.
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(esp_random() % 3000));
            const int n2 = WiFi.scanNetworks();
            if (scanContains(n2, ARC_NET_SSID) && connectTo(ARC_NET_SSID, s_arcPass, 8000)) {
                s_role = NET_ARCNET_CLIENT;
                ok = true;
            }
        }
        if (!ok) {
            WiFi.mode(WIFI_AP);
            WiFi.softAP(ARC_NET_SSID, s_arcPass.c_str());
            s_ssid = ARC_NET_SSID;
            s_role = NET_ARCNET_AP;
            Serial.printf("[WiFi] Eigenes Netz '%s' gestartet (%s)\n", ARC_NET_SSID, WiFi.softAPIP().toString().c_str());
        }

        startServices();
        if (s_role == NET_STATION) syncNtp(false);
        epd::requestStatus();
        s_statusDirty = true;
    }
}

}  // namespace

namespace net {

void begin() {
    s_netMtx = xSemaphoreCreateMutex();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // valid before WiFi is started
    char id[13];
    snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_deviceId = id;
    loadKnown();
    xTaskCreatePinnedToCore(connectTask, "NET", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
    static uint32_t lastHb = 0;
    if (s_serverStarted) s_ws.cleanupClients();
    if (s_statusDirty || (millis() - lastHb > 1000 && s_ws.count() > 0)) {
        s_statusDirty = false;
        lastHb = millis();
        broadcastStatus();
    }
    if (s_ntpRequested) {
        s_ntpRequested = false;
        if (s_role == NET_STATION) syncNtp(true);
    }
    if (s_setTimeUs) {
        const int64_t t = s_setTimeUs;
        s_setTimeUs = 0;
        if (llabs(t - arcclock::nowUs()) > 2000000LL) arcclock::setTimeUs(t);
    }
}

bool studioConnected() { return s_serverStarted && s_ws.count() > 0; }

String ipString() {
    if (s_role == NET_ARCNET_AP) return WiFi.softAPIP().toString();
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    return "-";
}

String statusLine() {
    String net;
    switch (s_role) {
        case NET_STATION:       net = "WLAN " + s_ssid; break;
        case NET_ARCNET_CLIENT: net = "ARC-Net"; break;
        case NET_ARCNET_AP:     net = "ARC-Net (eigenes Netz)"; break;
        default:                net = "kein Netz"; break;
    }
    String line = net + " " + ipString();
    line += studioConnected() ? "  |  Studio verbunden" : "  |  Studio getrennt";
    line += "  |  ";
    switch (ltc::mode()) {
        case MODE_GEN_FREE_RUN:        line += "Free Run"; break;
        case MODE_GEN_RECORD_RUN:      line += ltc::running() ? "Rec Run REC" : "Rec Run STOP"; break;
        case MODE_SLAVE:               line += "Slave"; break;
        case MODE_JAM_SYNC_FREE_RUN:
        case MODE_JAM_SYNC_RECORD_RUN: line += "Jam..."; break;
        default: break;
    }
    line += " " + String(ltc::rateKey()) + " fps";
    if (ltc::muteHeld()) line += "  |  KABEL ABZIEHEN";
    if (!arcclock::sqwActive()) line += "  |  RTC-SQW fehlt";
    return line;
}

void broadcastStatus() {
    JsonDocument doc;
    fillStatus(doc, "status");
    sendAll(doc);
}

void sendSlateState() {
    JsonDocument doc;
    doc["type"] = "slateState";
    doc["device"] = "slate";
    doc["id"] = s_deviceId;
    state::lock();
    const ShotInfo& s = state::shot();
    doc["cam"] = String(s.cam);
    doc["rollNum"] = s.rollNum;
    doc["scene"] = s.scene;
    doc["shot"] = s.shot;
    doc["take"] = s.take;
    state::unlock();
    doc["roll"] = state::rollString();
    sendAll(doc);
}

void send(JsonDocument& doc) { sendAll(doc); }

void requestNtpSync() { s_ntpRequested = true; }

void requestShotStep(int dir) {
    JsonDocument doc;
    doc["type"] = dir > 0 ? "nextShot" : "previousShot";
    sendAll(doc);
}

void addNetwork(const String& ssid, const String& pass, int priority) {
    xSemaphoreTake(s_netMtx, portMAX_DELAY);
    for (auto it = s_known.begin(); it != s_known.end(); ++it) {
        if (it->ssid == ssid) { s_known.erase(it); break; }
    }
    s_known.push_back({ssid, pass, priority});
    saveKnown();
    xSemaphoreGive(s_netMtx);
}

void clearNetworks() {
    xSemaphoreTake(s_netMtx, portMAX_DELAY);
    s_known.clear();
    saveKnown();
    xSemaphoreGive(s_netMtx);
}

void printNetworks() {
    xSemaphoreTake(s_netMtx, portMAX_DELAY);
    Serial.printf("Bekannte Netze (%u), Hash %s:\n", (unsigned)s_known.size(), knownHash().c_str());
    for (const auto& n : s_known) Serial.printf("  %-24s Prio %d\n", n.ssid.c_str(), n.priority);
    Serial.printf("ARC-Net-Passwort gesetzt: %s\n", s_arcPass.length() >= 8 ? "ja" : "NEIN (min. 8 Zeichen)");
    xSemaphoreGive(s_netMtx);
}

}  // namespace net
