#pragma once
// Shared SPI bus (FSPI on GPIO11/12) for the e-ink and both MAX7219 chains.
// Every user must hold the bus lock for the whole transfer.
#include <Arduino.h>
#include <SPI.h>

namespace spibus {

void begin();
void lock();
void unlock();
// Drive strength of SCK/MOSI, 0..3 (default 0, weakest). GxEPD2 init re-attaches the pins: call again after it.
void setDrive(uint8_t level);
uint8_t drive();

}  // namespace spibus
