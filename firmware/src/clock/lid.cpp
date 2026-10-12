#include "lid.h"
#include "pins.h"

namespace {

constexpr uint32_t DEBOUNCE_MS = 120;

volatile bool s_open = true;
volatile uint32_t s_changedMs = 0;
void (*s_callback)(bool) = nullptr;

// PIN_HALL: LOW = lid closed (magnet near), see pins.h.
void lidTask(void*) {
    bool candidate = s_open;
    uint32_t candidateSince = millis();
    for (;;) {
        const bool now = digitalRead(PIN_HALL) != LOW;
        if (now != candidate) {
            candidate = now;
            candidateSince = millis();
        } else if (candidate != s_open && millis() - candidateSince >= DEBOUNCE_MS) {
            s_open = candidate;
            s_changedMs = millis();
            Serial.printf("[LID] Klappe %s\n", s_open ? "geoeffnet" : "geschlossen");
            if (s_callback) s_callback(s_open);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

namespace lid {

void begin() {
    pinMode(PIN_HALL, INPUT_PULLUP);
    // The sync flash stays off in clock mode.
    pinMode(PIN_FLASH, OUTPUT);
    digitalWrite(PIN_FLASH, LOW);
    s_open = digitalRead(PIN_HALL) != LOW;
    s_changedMs = millis();
    xTaskCreatePinnedToCore(lidTask, "LID", 3072, nullptr, 2, nullptr, 1);
}

bool open() { return s_open; }
uint32_t sinceMs() { return millis() - s_changedMs; }
void onChange(void (*callback)(bool)) { s_callback = callback; }

}  // namespace lid
