#pragma once
// Rotary encoder + button (KY-040 on SW1, pins.h), polled in a task.
//   turn = brightness, short press = next page, long press = switch to the clapperboard firmware
#include <Arduino.h>

namespace input {

enum Event : uint8_t { NONE, TURN_CW, TURN_CCW, PRESS_SHORT, PRESS_LONG };

void begin();
bool poll(Event& e);          // non-blocking, false if nothing happened

}  // namespace input
