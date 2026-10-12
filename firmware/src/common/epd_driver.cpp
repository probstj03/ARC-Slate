#include "epd_driver.h"
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

Display& display() {
    static Display d(GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));
    return d;
}

void init() {
    Display& d = display();
    spibus::lock();
    d.init(0, true, 2, false);
    d.epd2.setBusyCallback(busyCallback);
    d.setRotation(0);
    spibus::unlock();
}

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
