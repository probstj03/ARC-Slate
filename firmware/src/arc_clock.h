#pragma once
// RTC-disciplined microsecond clock.
//
// The DS3231 1 Hz SQW output marks every second change with one of its edges.
// An ISR timestamps that edge with esp_timer; between edges the time is
// interpolated with the ESP32 timer. Result: wall-clock time (RTC local time)
// in microseconds without any I2C access on the hot path.
//
// Which SQW edge belongs to the second change is measured once by
// detectSqwEdge() and stored in the settings (see state.h).
#include <Arduino.h>
#include <RTClib.h>

namespace arcclock {

bool begin(bool sqwFalling);
bool rtcPresent();
bool sqwActive();            // second edges arriving?

int64_t nowUs();             // local wall-clock µs since 1970-01-01 (RTC time)
DateTime now();
uint32_t usInSecond();       // 0..999999

// Sets the RTC so that `epochUs` (local time) is exact at the moment of the
// write; waits for the right sub-second instant (≤ 1 s).
void setTimeUs(int64_t epochUs);
void setTime(const DateTime& dt);

// Measures which SQW edge coincides with the RTC second change (takes ~2.5 s).
// Returns false if the RTC or SQW is not working.
bool detectSqwEdge(bool& falling, int32_t& offsetMs);
void useEdge(bool falling);

// Raw RTC access for other modules (e.g. temperature, aging offset).
void lockI2C();
void unlockI2C();
RTC_DS3231& rtc();

}  // namespace arcclock
