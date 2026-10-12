/*
 * ARC-Clock – desk clock / calendar firmware for the clapperboard hardware.
 * Second app on the chip (ota_1); the slate firmware (ota_0) is the other one.
 *
 * Work in progress: skeleton that can switch back to the slate.
 */
#include <Arduino.h>
#include <ArcCore.h>
#include "bootsel.h"
#include "pins.h"

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nARC-Clock " ARC_FW_VERSION " startet ...");
    if (bootsel::heldAtBoot(PIN_ENC_SW, 1500) && bootsel::switchToOther()) return;
    Serial.println("Befehle: mode slate");
}

void loop() {
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line == "mode slate") bootsel::switchTo(bootsel::App::Slate);
    }
    delay(10);
}
