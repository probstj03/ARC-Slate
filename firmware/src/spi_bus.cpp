#include "spi_bus.h"
#include "pins.h"

namespace {
SemaphoreHandle_t s_mtx = nullptr;
}

namespace spibus {

void begin() {
    s_mtx = xSemaphoreCreateMutex();
    // No MISO: GPIO13 is the e-ink BUSY input. GxEPD2 calls SPI.begin() again
    // later; that call is a no-op because the bus is already initialised.
    SPI.begin(PIN_SPI_SCK, -1, PIN_SPI_MOSI, -1);
}

void lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mtx); }

}  // namespace spibus
