#pragma once
// ARC-Slate specific protocol on top of arc::net (ARC-Core):
// shot data from ARC-Studio, slate state / clap events to ARC-Studio, LTC user bits.
#include <Arduino.h>
#include <ArduinoJson.h>

namespace slate {

void begin();
void loop();

void sendSlateState();          // values changed at the device
void requestShotStep(int dir);  // ask ARC-Studio for the next/previous shot
void updateUserBits();          // shot (or date) user bits -> generator
String statusLine();            // e-ink status bar

}  // namespace slate
