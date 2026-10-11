#include "clap.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include "arc_clock.h"
#include "epaper.h"
#include "ltc_engine.h"
#include "matrix.h"
#include "net.h"
#include "pins.h"
#include "state.h"
#include "ui.h"

namespace {

constexpr int64_t DEBOUNCE_US = 30000;
constexpr size_t LOG_MAX_BYTES = 256 * 1024;

struct ClapEvent {
    bool closing;
    int64_t espUs;
};

QueueHandle_t s_queue = nullptr;
esp_timer_handle_t s_flashOff = nullptr;
volatile bool s_closed = false;
volatile int64_t s_lastEdgeUs = 0;
volatile bool s_armed = true;          // false while the menu is open
volatile int64_t s_flashUs = 40000;    // one frame
volatile uint32_t s_events = 0;
uint32_t s_seq = 0;

void flashOffCb(void*) { gpio_set_level((gpio_num_t)PIN_FLASH, 0); }

void IRAM_ATTR isrHall() {
    const int64_t t = esp_timer_get_time();
    const bool isClosed = gpio_get_level((gpio_num_t)PIN_HALL) == 0;
    if (isClosed == s_closed) return;
    if (t - s_lastEdgeUs < DEBOUNCE_US) return;
    s_lastEdgeUs = t;
    s_closed = isClosed;
    if (isClosed && s_armed) gpio_set_level((gpio_num_t)PIN_FLASH, 1);   // off: clapTask
    ClapEvent ev{isClosed, t};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_queue, &ev, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void appendLog(JsonDocument& doc) {
    if (LittleFS.exists("/events.jsonl")) {
        File f = LittleFS.open("/events.jsonl", "r");
        const size_t size = f ? f.size() : 0;
        if (f) f.close();
        if (size > LOG_MAX_BYTES) {
            LittleFS.remove("/events.old.jsonl");
            LittleFS.rename("/events.jsonl", "/events.old.jsonl");
        }
    }
    File f = LittleFS.open("/events.jsonl", "a");
    if (!f) return;
    serializeJson(doc, f);
    f.print('\n');
    f.close();
}

void handleClose(int64_t espUs) {
    int32_t flashLatency;
    state::lock();
    flashLatency = state::settings().flashLatencyUs;
    const ShotInfo s = state::shot();
    state::unlock();
    const String roll = state::rollString();

    const int64_t rtcUs = arcclock::nowUs() - (esp_timer_get_time() - espUs);
    uint32_t subUs = 0;
    const Timecode tc = ltc::timecodeAt(espUs, subUs);
    const String tcStr = ltc::tcToString(tc);
    const DateTime d = DateTime((uint32_t)(rtcUs / 1000000LL));
    char date[12], ub[9];
    snprintf(date, sizeof(date), "%02d.%02d.%04d", d.day(), d.month(), d.year());
    snprintf(ub, sizeof(ub), "%08lX", (unsigned long)ltc::txUserBits());

    // Protocol v2 event (RTC time in µs, TC + position inside the frame)
    JsonDocument ev;
    ev["type"] = "event";
    ev["kind"] = "clap_close";
    ev["device"] = "slate";
    ev["id"] = ++s_seq;
    ev["rtcUs"] = rtcUs;
    ev["flashUs"] = rtcUs + flashLatency;
    ev["tc"] = tcStr;
    ev["subframeUs"] = subUs;
    ev["fps"] = ltc::rateKey();
    ev["scene"] = s.scene;
    ev["shot"] = s.shot;
    ev["take"] = s.take;
    ev["roll"] = roll;
    ev["userbit"] = ub;
    net::send(ev);
    appendLog(ev);

    // Legacy message understood by the current ARC-Studio (adds the take).
    JsonDocument legacy;
    legacy["type"] = "newTake";
    legacy["take"] = s.take;
    legacy["timecode"] = tcStr;
    legacy["roll"] = roll;
    legacy["date"] = date;
    legacy["userbit"] = ub;
    net::send(legacy);

    mtx::showOverlay(tcStr, 2500);   // hold the clap timecode on the LEDs
    s_events++;
    Serial.printf("[CLAP] zu  TC %s +%lu us  Take %d\n", tcStr.c_str(), (unsigned long)subUs, s.take);
}

void handleOpen(int64_t espUs) {
    state::lock();
    ShotInfo& s = state::shot();
    if (s.take < 99) s.take++;
    const int take = s.take;
    state::touch();
    state::unlock();
    ltc::refreshUserBits();
    net::sendSlateState();
    epd::requestCard();   // QR code of the next take

    JsonDocument ev;
    ev["type"] = "event";
    ev["kind"] = "clap_open";
    ev["device"] = "slate";
    ev["id"] = ++s_seq;
    ev["rtcUs"] = arcclock::nowUs() - (esp_timer_get_time() - espUs);
    ev["take"] = take;
    net::send(ev);
    appendLog(ev);
    Serial.printf("[CLAP] auf -> Take %d\n", take);
}

// Switches the flash off one frame after `onEspUs` (esp_timer, task context).
void scheduleFlashOff(int64_t onEspUs) {
    int64_t remaining = s_flashUs - (esp_timer_get_time() - onEspUs);
    if (remaining < 1) remaining = 1;
    esp_timer_stop(s_flashOff);
    esp_timer_start_once(s_flashOff, remaining);
}

void clapTask(void*) {
    ClapEvent ev;
    for (;;) {
        // The flash must not fire while the menu is in use.
        s_armed = !ui::menuOpen();
        s_flashUs = (int64_t)(1000000.0f / ltc::fps());
        if (!xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(50))) continue;
        if (ev.closing) {
            if (s_armed) { scheduleFlashOff(ev.espUs); handleClose(ev.espUs); }
            else digitalWrite(PIN_FLASH, LOW);
        } else {
            if (s_armed) handleOpen(ev.espUs);
        }
    }
}

}  // namespace

namespace clap {

void begin() {
    pinMode(PIN_FLASH, OUTPUT);
    digitalWrite(PIN_FLASH, LOW);
    pinMode(PIN_HALL, INPUT_PULLUP);
    esp_timer_create_args_t args = {};
    args.callback = flashOffCb;
    args.name = "flashOff";
    esp_timer_create(&args, &s_flashOff);
    s_closed = digitalRead(PIN_HALL) == LOW;
    s_queue = xQueueCreate(8, sizeof(ClapEvent));
    attachInterrupt(digitalPinToInterrupt(PIN_HALL), isrHall, CHANGE);
    xTaskCreatePinnedToCore(clapTask, "CLAP", 6144, nullptr, 3, nullptr, 1);
}

void simulate() {
    const int64_t t = esp_timer_get_time();
    gpio_set_level((gpio_num_t)PIN_FLASH, 1);
    ClapEvent close{true, t};
    xQueueSend(s_queue, &close, 0);
    delay(300);
    ClapEvent open{false, esp_timer_get_time()};
    xQueueSend(s_queue, &open, 0);
}

void testFlash() {
    gpio_set_level((gpio_num_t)PIN_FLASH, 1);
    scheduleFlashOff(esp_timer_get_time());
}

bool closed() { return s_closed; }
uint32_t eventCount() { return s_events; }

}  // namespace clap
