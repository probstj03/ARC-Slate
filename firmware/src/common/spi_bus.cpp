#include "spi_bus.h"
#include "pins.h"

namespace {
SemaphoreHandle_t s_mtx = nullptr;
}

namespace spibus {

void begin() {
    s_mtx = xSemaphoreCreateMutex();
    // Keep the e-ink deselected from the very start: MOSI/SCK carry the matrix traffic before the
    // panel is initialised, and a floating CS would let it read that as commands.
    pinMode(PIN_EPD_CS, OUTPUT);
    digitalWrite(PIN_EPD_CS, HIGH);
    // No MISO: GPIO13 is the e-ink BUSY input. GxEPD2 calls SPI.begin() again
    // later; that call is a no-op because the bus is already initialised.
    SPI.begin(PIN_SPI_SCK, -1, PIN_SPI_MOSI, -1);
}

void lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mtx); }

}  // namespace spibus
