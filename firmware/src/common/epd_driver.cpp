#include "epd_driver.h"
#include "matrix.h"
#include "pins.h"
#include "spi_bus.h"

namespace epddrv {

namespace {

// The panel can take seconds; give the bus to the LED matrices meanwhile.
void busyCallback(const void*) {
    spibus::unlock();
    vTaskDelay(pdMS_TO_TICKS(5));
    spibus::lock();
}

}  // namespace

uint32_t s_spiHz = 2000000;

MatrixPause::MatrixPause(bool active) : _was(active && mtx::enabled()) {
    if (_was) {
        mtx::setEnabled(false);
        vTaskDelay(pdMS_TO_TICKS(120));     // the matrix task sends the shutdown command within a few ms
    }
}

MatrixPause::~MatrixPause() {
    if (_was) mtx::setEnabled(true);
}

Display& display() {
    static Display d(GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));
    return d;
}

void init() {
    Display& d = display();
    // Right after power-up the 3V3 rail and the panel need time: with the default 2 ms reset the first
    // refresh after a reset came out garbled (later refreshes were fine) and BUSY sometimes never released.
    vTaskDelay(pdMS_TO_TICKS(300));
    spibus::lock();
    d.init(115200, true, 20, false);            // 20 ms reset pulse and settle time (115200: GxEPD2 prints the busy times)
    vTaskDelay(pdMS_TO_TICKS(200));
    d.epd2.setBusyCallback(busyCallback);
    d.setRotation(0);
    d.epd2.selectSPI(SPI, SPISettings(s_spiHz, MSBFIRST, SPI_MODE0));
    spibus::unlock();
}

void setSpiHz(uint32_t hz) {
    s_spiHz = hz < 100000 ? 100000 : hz > 8000000 ? 8000000 : hz;
    display().epd2.selectSPI(SPI, SPISettings(s_spiHz, MSBFIRST, SPI_MODE0));
}

uint32_t spiHz() { return s_spiHz; }

void printAt(int x, int y, const GFXfont* font, const String& text) {
    Display& d = display();
    d.setFont(font);
    d.setCursor(x, y);
    d.print(text);
}

int textWidth(const GFXfont* font, const String& text) {
    int16_t bx, by; uint16_t bw, bh;
    Display& d = display();
    d.setFont(font);
    d.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    return (int)bw;
}

void printCentered(int cx, int y, const GFXfont* font, const String& text) {
    int16_t bx, by; uint16_t bw, bh;
    Display& d = display();
    d.setFont(font);
    d.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    d.setCursor(cx - (int)bw / 2 - bx, y);
    d.print(text);
}

void printRight(int xRight, int y, const GFXfont* font, const String& text) {
    int16_t bx, by; uint16_t bw, bh;
    Display& d = display();
    d.setFont(font);
    d.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    d.setCursor(xRight - (int)bw - bx, y);
    d.print(text);
}

}  // namespace epddrv
