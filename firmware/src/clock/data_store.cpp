#include "data_store.h"
#include <ArcCore.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "clock_config.h"
#include "core/civil.h"
#include "core/ics.h"
#include "http_client.h"

namespace clockdata {

namespace {

constexpr uint32_t kCacheMagic = 0x41434348;   // "ACCH"
constexpr uint16_t kCacheVersion = 2;
constexpr const char* kCachePath = "/clock_cache.bin";

Data s_data;
SemaphoreHandle_t s_mtx = nullptr;        // protects s_data
SemaphoreHandle_t s_fetchMtx = nullptr;   // one fetch at a time (task or serial command)
String s_todoEtag;

// staging areas (static: too big for the task stack)
ac::Agenda s_calStage;
ac::DocLine s_docStage[ac::kMaxDocLines];
int s_docStageN = 0;

volatile uint32_t s_requestMask = 0;
uint32_t s_attempts[SRC_COUNT] = {0, 0, 0};
uint32_t s_attemptsAtRequest[SRC_COUNT] = {0, 0, 0};
uint32_t s_dueMs[SRC_COUNT] = {0, 0, 0};
uint32_t s_backoffMs[SRC_COUNT] = {0, 0, 0};
// Login refused (401/403) or rate limited by the server (429): no automatic retries - every retry would extend the
// brute-force block of Nextcloud. Resumed by `fetch` or new credentials (clockdata::resume()).
volatile bool s_paused[SRC_COUNT] = {false, false, false};
int s_lastStatus = 0;           // HTTP status of the last failure (set by statusText)
uint32_t s_lastCacheSave = 0;

int64_t nowLocal() { return arc::clock::nowNs() / 1000000000LL; }

bool timePlausible() { return ac::fromSec(nowLocal()).year >= 2024; }

bool networkOk() {
    return arc::net::role() == arc::net::Role::Station && WiFi.status() == WL_CONNECTED && timePlausible();
}

bool enabled(Source s, const ClockConfig& c) {
    switch (s) {
        case SRC_CALENDAR: {
            String cals[6];
            return c.ncUrl.length() && c.ncUser.length() && clockcfg::calendarList(c, cals, 6) > 0;
        }
        case SRC_TODO: return c.ncUrl.length() && c.ncUser.length() && c.todoPath.length();
        case SRC_WEATHER: return true;
        default: return false;
    }
}

uint32_t intervalMs(Source s, const ClockConfig& c) {
    const uint32_t min = s == SRC_CALENDAR ? c.calendarMin : s == SRC_TODO ? c.todoMin : c.weatherMin;
    return (min < 1 ? 1 : min) * 60000UL;
}

void markFailure(Source s, const char* what) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    SourceState& st = s_data.src[s];
    st.tried = nowLocal();
    strlcpy(st.error, what, sizeof(st.error));
    xSemaphoreGive(s_mtx);
}

const char* statusText(int status, const String& error) {
    static char buf[48];
    s_lastStatus = status;
    if (status == 401 || status == 403) return "Login abgelehnt (App-Passwort?)";
    if (status == 404) return "Pfad nicht gefunden";
    if (status <= 0) { snprintf(buf, sizeof(buf), "%s", error.c_str()); return buf; }
    snprintf(buf, sizeof(buf), "HTTP %d", status);
    return buf;
}

// ── cache ────────────────────────────────────────────────────────────────

struct CacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t sizeData;
    uint16_t etagLen;
};

void saveCache() {
    File f = LittleFS.open(kCachePath, "w");
    if (!f) return;
    CacheHeader h{kCacheMagic, kCacheVersion, 0, (uint16_t)s_todoEtag.length()};
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    f.write((const uint8_t*)&h, sizeof(h));
    f.write((const uint8_t*)s_todoEtag.c_str(), s_todoEtag.length());
    f.write((const uint8_t*)&s_data, sizeof(Data));
    xSemaphoreGive(s_mtx);
    f.close();
    s_lastCacheSave = millis();
}

void loadCache() {
    File f = LittleFS.open(kCachePath, "r");
    if (!f) return;
    CacheHeader h;
    bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == kCacheMagic && h.version == kCacheVersion &&
              f.size() == (int)(sizeof(h) + h.etagLen + sizeof(Data));
    if (ok) {
        char etag[96] = {0};
        if (h.etagLen < sizeof(etag)) f.read((uint8_t*)etag, h.etagLen); else ok = false;
        if (ok) {
            Data* tmp = new Data();
            if (f.read((uint8_t*)tmp, sizeof(Data)) == sizeof(Data)) {
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_data = *tmp;
                xSemaphoreGive(s_mtx);
                s_todoEtag = etag;
                Serial.println("[CLK] Cache geladen (Kalender/To-Do/Wetter vom letzten Abruf)");
            }
            delete tmp;
        }
    }
    f.close();
}

void saveCacheThrottled() {
    if (s_lastCacheSave == 0 || millis() - s_lastCacheSave > 5 * 60000UL) saveCache();
}

// ── calendar ─────────────────────────────────────────────────────────────

struct CalCtx { int64_t from, to; };

void onEvent(const ac::Event& e, void* c) {
    const CalCtx* ctx = (const CalCtx*)c;
    s_calStage.add(e, ctx->from, ctx->to);
}

void feedDav(const uint8_t* d, size_t n, void* c) { ((ac::CalDavStream*)c)->feed((const char*)d, n); }

void stamp(int64_t utc, char* out, size_t cap) {
    const ac::Civil c = ac::fromSec(utc);
    snprintf(out, cap, "%04d%02d%02dT%02d%02d%02dZ", c.year, c.month, c.day, c.hour, c.minute, c.second);
}

void feedString(const uint8_t* d, size_t n, void* c);

// A configured path may be a single calendar (/remote.php/dav/calendars/<user>/<name>/), the calendar home
// (/remote.php/dav/calendars/<user>/) or the principal (/remote.php/dav/principals/users/<user>/). The last two are
// expanded with a PROPFIND (Depth 1) on the calendar home: every collection that is a calendar with events.
int expandCalendars(const ClockConfig& cfg, const String in[], int nIn, String out[], int max, Print* log) {
    int n = 0;
    for (int i = 0; i < nIn && n < max; i++) {
        if (s_lastStatus == 401 || s_lastStatus == 403 || s_lastStatus == 429) break;
        String p = in[i];
        p.trim();
        if (!p.startsWith("/")) p = "/" + p;
        if (!p.endsWith("/")) p += "/";
        String home;
        const int pr = p.indexOf("/principals/users/");
        if (pr >= 0) {
            home = p.substring(0, pr) + "/calendars/" + p.substring(pr + 18);
        } else {
            const int ca = p.indexOf("/calendars/");
            if (ca >= 0) {
                const String rest = p.substring(ca + 11);                     // "<user>/" or "<user>/<name>/"
                const int slash = rest.indexOf('/');
                if (slash == (int)rest.length() - 1) home = p;                // only the user: the home
            }
        }
        if (home.isEmpty()) { out[n++] = p; continue; }

        clockhttp::Request req;
        req.method = "PROPFIND";
        req.url = cfg.ncUrl + home;
        req.user = cfg.loginName();
        req.pass = cfg.ncPass;
        req.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                   "<d:prop><d:resourcetype/><c:supported-calendar-component-set/></d:prop></d:propfind>";
        req.contentType = "application/xml; charset=utf-8";
        req.depth = "1";
        req.insecure = cfg.tlsInsecure;
        String body;
        body.reserve(4096);
        const clockhttp::Result r = clockhttp::perform(req, feedString, &body);
        if (r.status != 207) {
            if (log) log->printf("  %s: Kalender-Suche FEHLER %s\n", home.c_str(), statusText(r.status, r.error));
            out[n++] = p;                                                     // try it as it is
            continue;
        }
        // one <d:response> per collection; namespace prefixes differ between servers, so search loosely
        int pos = 0;
        while (n < max) {
            const int a = body.indexOf("response>", pos);
            if (a < 0) break;
            const int b = body.indexOf("response>", a + 9);
            if (b < 0) break;
            const String block = body.substring(a, b);
            pos = b + 9;
            const bool isCal = block.indexOf(":calendar/>") >= 0 || block.indexOf(":calendar />") >= 0;
            const bool events = block.indexOf("VEVENT") >= 0;
            const int h0 = block.indexOf("href>");
            const int h1 = h0 >= 0 ? block.indexOf("<", h0 + 5) : -1;
            if (!isCal || !events || h1 < 0) continue;
            const String href = block.substring(h0 + 5, h1);
            if (href.indexOf("/inbox/") >= 0 || href.indexOf("/outbox/") >= 0 || href.indexOf("/trashbin/") >= 0) continue;
            out[n++] = href;
            if (log) log->printf("  gefunden: %s\n", href.c_str());
        }
    }
    return n;
}

bool fetchCalendar(Print* log) {
    const ClockConfig cfg = clockcfg::copy();
    String configured[6], cals[12];
    const int nCfg = clockcfg::calendarList(cfg, configured, 6);
    const int n = expandCalendars(cfg, configured, nCfg, cals, 12, log);
    const int64_t today = ac::dayStart(nowLocal());
    CalCtx ctx{today, today + 7 * 86400};
    char from[20], to[20];
    stamp(ac::localToUtc(ctx.from), from, sizeof(from));
    stamp(ac::localToUtc(ctx.to + 86400), to, sizeof(to));

    String body;
    body.reserve(600);
    body += "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
            "<d:prop><c:calendar-data><c:expand start=\"";
    body += from; body += "\" end=\""; body += to;
    body += "\"/></c:calendar-data></d:prop>"
            "<c:filter><c:comp-filter name=\"VCALENDAR\"><c:comp-filter name=\"VEVENT\"><c:time-range start=\"";
    body += from; body += "\" end=\""; body += to;
    body += "\"/></c:comp-filter></c:comp-filter></c:filter></c:calendar-query>";

    s_calStage.n = 0;
    int okCount = 0;
    String lastError;
    for (int i = 0; i < n; i++) {
        if (s_lastStatus == 401 || s_lastStatus == 403 || s_lastStatus == 429) break;   // refused: no further requests
        clockhttp::Request req;
        req.method = "REPORT";
        req.url = cfg.ncUrl + cals[i];
        req.user = cfg.loginName();
        req.pass = cfg.ncPass;
        req.body = body;
        req.contentType = "application/xml; charset=utf-8";
        req.depth = "1";
        req.insecure = cfg.tlsInsecure;
        ac::CalDavStream stream(onEvent, &ctx);
        const clockhttp::Result r = clockhttp::perform(req, feedDav, &stream);
        stream.finish();
        if (r.status == 207 || r.status == 200) {
            okCount++;
            if (log) log->printf("  %s: %u Bytes\n", cals[i].c_str(), (unsigned)r.bytes);
        } else {
            lastError = statusText(r.status, r.error);
            if (log) log->printf("  %s: FEHLER %s\n", cals[i].c_str(), lastError.c_str());
        }
    }
    if (okCount == 0) {
        markFailure(SRC_CALENDAR, lastError.c_str());
        return false;
    }
    s_calStage.finalize();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const uint32_t oldHash = s_data.agenda.hash();
    s_data.agenda = s_calStage;
    SourceState& st = s_data.src[SRC_CALENDAR];
    st.ok = true;
    st.at = st.tried = nowLocal();
    if (s_data.agenda.hash() != oldHash) st.revision++;
    strlcpy(st.error, okCount < n ? lastError.c_str() : "", sizeof(st.error));
    xSemaphoreGive(s_mtx);
    saveCacheThrottled();
    if (log) {
        log->printf("Kalender: %d Termine in den naechsten 7 Tagen\n", s_calStage.n);
        for (int i = 0; i < s_calStage.n && i < 12; i++) {
            char t[24];
            ac::timeLabel(s_calStage.ev[i], t, sizeof(t));
            const ac::Civil c = ac::fromSec(s_calStage.ev[i].start);
            log->printf("  %02d.%02d. %-12s %s%s%s\n", c.day, c.month, t, s_calStage.ev[i].summary,
                        s_calStage.ev[i].location[0] ? "  @ " : "", s_calStage.ev[i].location);
        }
    }
    return true;
}

// ── dashboard document (markdown on the Nextcloud, shown 1:1) ────────────

void onDocLine(const ac::DocLine& l, void*) {
    if (s_docStageN < ac::kMaxDocLines) s_docStage[s_docStageN++] = l;
}

void feedDoc(const uint8_t* d, size_t n, void* c) { ((ac::DocParser*)c)->feed((const char*)d, n); }

bool fetchTodo(Print* log) {
    const ClockConfig cfg = clockcfg::copy();
    clockhttp::Request req;
    req.url = cfg.ncUrl + "/remote.php/dav/files/" + clockhttp::encodePath(cfg.ncUser) + "/" + clockhttp::encodePath(cfg.todoPath);
    req.user = cfg.loginName();
    req.pass = cfg.ncPass;
    req.insecure = cfg.tlsInsecure;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool have = s_data.src[SRC_TODO].ok;
    xSemaphoreGive(s_mtx);
    if (have && s_todoEtag.length()) req.ifNoneMatch = s_todoEtag;

    s_docStageN = 0;
    ac::DocParser parser(onDocLine, nullptr);
    const clockhttp::Result r = clockhttp::perform(req, feedDoc, &parser);
    parser.finish();

    if (r.status == 304) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_data.src[SRC_TODO].at = s_data.src[SRC_TODO].tried = nowLocal();
        s_data.src[SRC_TODO].error[0] = 0;
        xSemaphoreGive(s_mtx);
        if (log) log->println("Dashboard: unveraendert (304)");
        return true;
    }
    if (r.status != 200) {
        markFailure(SRC_TODO, statusText(r.status, r.error));
        if (log) log->printf("Dashboard: FEHLER %s (%s)\n", s_data.src[SRC_TODO].error, req.url.c_str());
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool changed = s_data.docCount != s_docStageN;
    for (int i = 0; i < s_docStageN && !changed; i++)
        changed = memcmp(&s_data.doc[i], &s_docStage[i], sizeof(ac::DocLine)) != 0;
    for (int i = 0; i < s_docStageN; i++) s_data.doc[i] = s_docStage[i];
    s_data.docCount = s_docStageN;
    SourceState& st = s_data.src[SRC_TODO];
    st.ok = true;
    st.at = st.tried = nowLocal();
    st.error[0] = 0;
    if (changed) st.revision++;
    xSemaphoreGive(s_mtx);
    s_todoEtag = r.etag;
    saveCacheThrottled();
    if (log) {
        log->printf("Dashboard: %d Zeilen\n", s_docStageN);
        static const char* const kMark[] = {"  ", "# ", "##", "##", "[ ]", "[x]", " - ", "   ", "--"};
        for (int i = 0; i < s_docStageN && i < 14; i++)
            log->printf("  %s %s\n", kMark[(int)s_docStage[i].kind], s_docStage[i].text);
    }
    return true;
}

// ── weather ──────────────────────────────────────────────────────────────

void feedString(const uint8_t* d, size_t n, void* c) { ((String*)c)->concat((const char*)d, n); }

// ── automatic place from the public IP ───────────────────────────────────
// ip-api.com: free, no key, city-level accuracy (good enough for the weather). Only HTTP in the free tier;
// nothing secret is sent. Once per start (the provider may move the IP), and again after `loc auto`.
volatile bool s_located = false;

void locateByIp(Print* log) {
    if (s_located || !clockcfg::copy().locAuto) return;
    clockhttp::Request req;
    req.url = "http://ip-api.com/json/?fields=status,message,lat,lon,city";
    req.timeoutMs = 8000;
    String body;
    const clockhttp::Result r = clockhttp::perform(req, feedString, &body);
    JsonDocument doc;
    if (r.status != 200 || deserializeJson(doc, body) || strcmp(doc["status"] | "", "success") != 0) {
        Serial.printf("[LOC] Standort aus der IP nicht ermittelt (%s) - bleibt bei %s\n",
                      r.status != 200 ? statusText(r.status, r.error) : (const char*)(doc["message"] | "Antwort nicht lesbar"),
                      clockcfg::copy().place.c_str());
        return;                                  // next weather fetch tries again
    }
    s_located = true;
    ClockConfig& c = clockcfg::get();
    if (!c.locAuto) return;                      // set by hand meanwhile
    c.lat = doc["lat"] | c.lat;
    c.lon = doc["lon"] | c.lon;
    c.place = (const char*)(doc["city"] | c.place.c_str());
    clockcfg::save();
    Serial.printf("[LOC] Standort aus der IP: %s (%.3f, %.3f)\n", c.place.c_str(), c.lat, c.lon);
    if (log && log != &Serial) log->printf("Standort: %s\n", c.place.c_str());
}

bool fetchWeather(Print* log) {
    locateByIp(log);
    const ClockConfig cfg = clockcfg::copy();
    clockhttp::Request req;
    char url[480];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
             "&hourly=temperature_2m,precipitation_probability,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,precipitation_probability_max"
             "&timezone=Europe%%2FBerlin&forecast_days=3",
             cfg.lat, cfg.lon);
    req.url = url;
    String body;
    body.reserve(12000);
    const clockhttp::Result r = clockhttp::perform(req, feedString, &body);
    if (r.status != 200) {
        markFailure(SRC_WEATHER, statusText(r.status, r.error));
        if (log) log->printf("Wetter: FEHLER %s\n", s_data.src[SRC_WEATHER].error);
        return false;
    }
    static ac::Weather w;
    if (!ac::parseOpenMeteo(body.c_str(), body.length(), nowLocal(), w)) {
        markFailure(SRC_WEATHER, "Antwort nicht lesbar");
        if (log) log->println("Wetter: Antwort nicht lesbar");
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool changed = memcmp(&s_data.weather.hTemp10, &w.hTemp10, sizeof(w.hTemp10)) != 0 || s_data.weather.day0 != w.day0 ||
                         s_data.weather.day[0].sunrise != w.day[0].sunrise || s_data.weather.wmo != w.wmo;
    s_data.weather = w;
    SourceState& st = s_data.src[SRC_WEATHER];
    st.ok = true;
    st.at = st.tried = nowLocal();
    st.error[0] = 0;
    if (changed) st.revision++;
    xSemaphoreGive(s_mtx);
    saveCacheThrottled();
    if (log) {
        log->printf("Wetter %s: %.1f Grad (gefuehlt %.1f), %s, Regen heute bis %u %%\n", cfg.place.c_str(), w.temp10 / 10.0, w.feels10 / 10.0,
                    ac::kindName(ac::kindFromWmo(w.wmo)), w.day[0].popMax);
        const ac::Civil sr = ac::fromSec(w.day[0].sunrise), ss = ac::fromSec(w.day[0].sunset);
        log->printf("  Sonne %02d:%02d - %02d:%02d, heute %.1f..%.1f Grad\n", sr.hour, sr.minute, ss.hour, ss.minute, w.day[0].tMin10 / 10.0,
                    w.day[0].tMax10 / 10.0);
    }
    return true;
}

bool fetchOne(Source s, Print* log) {
    xSemaphoreTake(s_fetchMtx, portMAX_DELAY);
    bool ok = false;
    switch (s) {
        case SRC_CALENDAR: ok = fetchCalendar(log); break;
        case SRC_TODO: ok = fetchTodo(log); break;
        case SRC_WEATHER: ok = fetchWeather(log); break;
        default: break;
    }
    s_attempts[s]++;
    xSemaphoreGive(s_fetchMtx);
    return ok;
}

void fetchTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!networkOk()) continue;
        const ClockConfig cfg = clockcfg::copy();
        for (int i = 0; i < SRC_COUNT; i++) {
            const Source s = (Source)i;
            if (!enabled(s, cfg) || s_paused[i]) continue;
            const uint32_t now = millis();
            const bool forced = (s_requestMask >> i) & 1;
            if (!forced && (int32_t)(now - s_dueMs[i]) < 0) continue;
            s_requestMask &= ~(1u << i);
            s_lastStatus = 0;
            const bool ok = fetchOne(s, nullptr);
            const uint32_t interval = intervalMs(s, cfg);
            if (ok) {
                s_backoffMs[i] = 0;
                s_dueMs[i] = millis() + interval;
            } else if (s_lastStatus == 401 || s_lastStatus == 403 || s_lastStatus == 429) {
                s_paused[i] = true;
                Serial.printf("[CLK] %s: %s - keine automatischen Versuche mehr bis 'fetch' oder neue Zugangsdaten\n", sourceName(s), s_data.src[i].error);
            } else {
                s_backoffMs[i] = s_backoffMs[i] ? min(s_backoffMs[i] * 2, interval) : 60000UL;
                s_dueMs[i] = millis() + s_backoffMs[i];
                Serial.printf("[CLK] %s: %s (naechster Versuch in %lu s)\n", sourceName(s), s_data.src[i].error, (unsigned long)(s_backoffMs[i] / 1000));
            }
            break;   // one request per round: leaves room for other tasks between TLS sessions
        }
    }
}

}  // namespace

const char* sourceName(Source s) {
    static const char* const n[] = {"Kalender", "To-Do", "Wetter"};
    return s < SRC_COUNT ? n[s] : "?";
}

void begin() {
    s_mtx = xSemaphoreCreateMutex();
    s_fetchMtx = xSemaphoreCreateMutex();
    LittleFS.begin(true);
    loadCache();
    // The first save comes after the next successful fetch round, not right after the first source (that would
    // store a cache with only the calendar in it).
    s_lastCacheSave = millis();
}

void startTask() {
    xTaskCreatePinnedToCore(fetchTask, "CLKFETCH", 12288, nullptr, 1, nullptr, 1);
}

void lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mtx); }
const Data& data() { return s_data; }

void request(Source s) { s_requestMask |= (1u << s); }

void relocate() { s_located = false; }

void requestAll() {
    for (int i = 0; i < SRC_COUNT; i++) s_attemptsAtRequest[i] = s_attempts[i];
    s_requestMask |= (1u << SRC_COUNT) - 1;
}

bool waitForNetwork(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (!networkOk() && millis() - start < timeoutMs) vTaskDelay(pdMS_TO_TICKS(250));
    return networkOk();
}

bool waitForFresh(uint32_t timeoutMs) {
    const uint32_t start = millis();
    const ClockConfig cfg = clockcfg::copy();
    while (millis() - start < timeoutMs) {
        if (!networkOk()) return false;
        bool done = true;
        for (int i = 0; i < SRC_COUNT; i++)
            if (enabled((Source)i, cfg) && s_attempts[i] == s_attemptsAtRequest[i]) done = false;
        if (done) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

void fetchNowVerbose(Source s, Print& out) {
    out.printf("[CLK] Hole %s ...\n", sourceName(s));
    if (!networkOk()) out.println("[CLK] Hinweis: kein WLAN mit Internet bzw. Uhrzeit unplausibel - der Abruf wird trotzdem versucht");
    s_lastStatus = 0;
    if (fetchOne(s, &out)) s_paused[s] = false;
    else if (s_lastStatus == 401 || s_lastStatus == 403 || s_lastStatus == 429) {
        s_paused[s] = true;
        out.println("[CLK] keine automatischen Versuche bis zum naechsten 'fetch' (Nextcloud-Sperre nicht verlaengern)");
    }
}

void resume() {
    for (auto& p : s_paused) p = false;
    for (auto& b : s_backoffMs) b = 0;
}

}  // namespace clockdata
