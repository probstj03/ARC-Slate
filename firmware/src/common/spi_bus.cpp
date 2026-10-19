#include "spi_bus.h"
#include "pins.h"
#include <driver/gpio.h>

namespace {
SemaphoreHandle_t s_mtx = nullptr;
uint8_t s_drive = 0;   // GPIO drive strength of SCK/MOSI: 0 weakest (~5 mA) .. 3 strongest (~40 mA, ESP default 2)
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
    setDrive(s_drive);
}

void setDrive(uint8_t level) {
    // weaker drivers = slower edges = less ringing on the long, loaded SCK/MOSI lines (double clocking shifts
    // the e-ink picture by a few pixels from the faulty byte on)
    const gpio_drive_cap_t cap = (gpio_drive_cap_t)(level > 3 ? 3 : level);
    gpio_set_drive_capability((gpio_num_t)PIN_SPI_SCK, cap);
    gpio_set_drive_capability((gpio_num_t)PIN_SPI_MOSI, cap);
    s_drive = level > 3 ? 3 : level;
}
uint8_t drive() { return s_drive; }

void lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mtx); }

}  // namespace spibus
