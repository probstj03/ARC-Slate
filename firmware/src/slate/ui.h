#pragma once
// Rotary encoder operation (KY-040 on SW1) with the menu on the e-ink.
//
// Idle:  turn = take -/+ · short press = menu · long press = confirm "cable
//        unplugged" after a jam, otherwise start/stop in Record Run
// Menu:  turn = select · press = edit/execute · long press = close
// Edit:  turn = change value · press = apply · long press = discard
// The menu closes after 10 s without input.
#include <Arduino.h>

namespace ui {

void begin();
bool menuOpen();
void setEncoderDebug(bool on);

}  // namespace ui
