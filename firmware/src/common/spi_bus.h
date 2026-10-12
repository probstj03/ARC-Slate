#pragma once
// Shared SPI bus (FSPI on GPIO11/12) for the e-ink and both MAX7219 chains.
// Every user must hold the bus lock for the whole transfer.
#include <Arduino.h>
#include <SPI.h>

namespace spibus {

void begin();
void lock();
void unlock();

}  // namespace spibus
