#include "arc_clock.h"
#include <Wire.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "pins.h"

namespace {
RTC_DS3231 s_rtc;
SemaphoreHandle_t s_i2cMtx = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_rtcOk = false;

volatile bool    s_falling    = true;   // which edge is the second change
volatile int64_t s_edgeUs     = 0;      // esp_timer at the last second edge
volatile int64_t s_epochSec   = 0;      // RTC seconds valid at s_edgeUs
volatile bool    s_epochValid = false;
volatile uint32_t s_edgeCount = 0;
volatile int64_t s_lastRiseUs = 0;
volatile int64_t s_lastFallUs = 0;

// Fallback when SQW is missing: RTC second read at boot + esp_timer.
int64_t s_fallbackBaseSec = 0;
int64_t s_fallbackBaseUs  = 0;

void IRAM_ATTR isrSqw() {
    const int64_t t = esp_timer_get_time();
    const bool high = gpio_get_level((gpio_num_t)PIN_RTC_SQW);
    if (high) s_lastRiseUs = t; else s_lastFallUs = t;
    if (high == s_falling) return;          // not the second edge
    portENTER_CRITICAL_ISR(&s_mux);
    s_edgeUs = t;
    if (s_epochValid) s_epochSec = s_epochSec + 1;
    s_edgeCount = s_edgeCount + 1;
    portEXIT_CRITICAL_ISR(&s_mux);
}

bool waitForEdge(uint32_t timeoutMs) {
    const uint32_t c0 = s_edgeCount;
    const uint32_t t0 = millis();
    while (s_edgeCount == c0) {
        if (millis() - t0 > timeoutMs) return false;
        delay(1);
    }
    return true;
}

// Aligns s_epochSec with the RTC right after a second edge.
void resync() {
    if (!s_rtcOk) return;
    if (waitForEdge(1500)) {
        arcclock::lockI2C();
        const DateTime dt = s_rtc.now();
        arcclock::unlockI2C();
        portENTER_CRITICAL(&s_mux);
        s_epochSec = (int64_t)dt.unixtime();
        s_epochValid = true;
        portEXIT_CRITICAL(&s_mux);
    } else {
        arcclock::lockI2C();
        const DateTime dt = s_rtc.now();
        arcclock::unlockI2C();
        s_fallbackBaseSec = (int64_t)dt.unixtime();
        s_fallbackBaseUs = esp_timer_get_time();
        s_epochValid = false;
        Serial.println("[CLK] Keine SQW-Flanken – Zeitstempel nur sekundengenau!");
    }
}
}  // namespace

namespace arcclock {

void lockI2C() { xSemaphoreTake(s_i2cMtx, portMAX_DELAY); }
void unlockI2C() { xSemaphoreGive(s_i2cMtx); }
RTC_DS3231& rtc() { return s_rtc; }
bool rtcPresent() { return s_rtcOk; }

bool sqwActive() {
    return s_epochValid && (esp_timer_get_time() - s_edgeUs) < 1500000;
}

bool begin(bool sqwFalling) {
    s_i2cMtx = xSemaphoreCreateMutex();
    s_falling = sqwFalling;
    Wire.begin(PIN_RTC_SDA, PIN_RTC_SCL, 400000);
    s_rtcOk = s_rtc.begin(&Wire);
    if (!s_rtcOk) {
        Serial.println("[CLK] DS3231 nicht gefunden!");
        s_fallbackBaseUs = esp_timer_get_time();
        return false;
    }
    if (s_rtc.lostPower()) {
        Serial.println("[CLK] RTC hatte keinen Strom – Zeit muss gestellt werden (NTP/Studio/Jam)");
    }
    s_rtc.writeSqwPinMode(DS3231_SquareWave1Hz);
    pinMode(PIN_RTC_SQW, INPUT_PULLUP);     // SQW is open drain
    attachInterrupt(digitalPinToInterrupt(PIN_RTC_SQW), isrSqw, CHANGE);
    resync();
    return true;
}

void useEdge(bool falling) {
    s_falling = falling;
    resync();
}

int64_t nowUs() {
    if (!s_epochValid) {
        return (s_fallbackBaseSec * 1000000LL) + (esp_timer_get_time() - s_fallbackBaseUs);
    }
    portENTER_CRITICAL(&s_mux);
    const int64_t e = s_edgeUs;
    const int64_t sec = s_epochSec;
    portEXIT_CRITICAL(&s_mux);
    int64_t d = esp_timer_get_time() - e;
    if (d < 0) d = 0;
    return sec * 1000000LL + d;
}

DateTime now() { return DateTime((uint32_t)(nowUs() / 1000000LL)); }

uint32_t usInSecond() { return (uint32_t)(nowUs() % 1000000LL); }

void setTimeUs(int64_t epochUs) {
    if (!s_rtcOk) return;
    // Writing the seconds register restarts the DS3231 second countdown, so the
    // write has to happen exactly at a whole second of the target time.
    const int64_t targetOffset = epochUs - esp_timer_get_time();  // target = esp + offset
    int64_t t = esp_timer_get_time() + targetOffset;
    int64_t nextSec = (t / 1000000LL + 1) * 1000000LL;
    int64_t waitUntilEsp = nextSec - targetOffset;
    while (esp_timer_get_time() < waitUntilEsp - 2000) delay(1);
    while (esp_timer_get_time() < waitUntilEsp) { }
    lockI2C();
    s_rtc.adjust(DateTime((uint32_t)(nextSec / 1000000LL)));
    unlockI2C();
    resync();
}

void setTime(const DateTime& dt) { setTimeUs((int64_t)dt.unixtime() * 1000000LL); }

bool detectSqwEdge(bool& falling, int32_t& offsetMs) {
    if (!s_rtcOk) return false;
    lockI2C();
    uint8_t lastSec = s_rtc.now().second();
    unlockI2C();
    int64_t prevPollUs = esp_timer_get_time();
    const uint32_t t0 = millis();
    while (millis() - t0 < 2500) {
        lockI2C();
        const uint8_t sec = s_rtc.now().second();
        unlockI2C();
        const int64_t pollUs = esp_timer_get_time();
        if (sec != lastSec) {
            const int64_t changeUs = (prevPollUs + pollUs) / 2;   // ±1 ms
            delay(5);
            const int64_t dRise = llabs(changeUs - s_lastRiseUs) % 1000000LL;
            const int64_t dFall = llabs(changeUs - s_lastFallUs) % 1000000LL;
            const int64_t eRise = dRise > 500000 ? 1000000 - dRise : dRise;
            const int64_t eFall = dFall > 500000 ? 1000000 - dFall : dFall;
            if (s_lastRiseUs == 0 && s_lastFallUs == 0) return false;
            falling = eFall < eRise;
            offsetMs = (int32_t)((falling ? eFall : eRise) / 1000);
            return true;
        }
        lastSec = sec;
        prevPollUs = pollUs;
        delayMicroseconds(500);
    }
    return false;
}

}  // namespace arcclock
