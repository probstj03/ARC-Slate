#pragma once
// 7.5" e-ink (GDEW075T7, 800x480) on the shared SPI bus.
// Layout: left = production data, right = QR code (or menu), bottom = status bar.
// The menu and the status bar use partial refresh (~0.5 s); the slate card
// uses a full refresh (~4 s). While the panel is busy the SPI bus is released,
// so the LED timecode keeps running.
#include <Arduino.h>

struct MenuView {
    static constexpr int MAX_ITEMS = 12;
    String title;
    String items[MAX_ITEMS];
    String values[MAX_ITEMS];
    int count = 0;
    int selected = 0;
    bool editing = false;
    String hint;
};

namespace epd {

void begin();          // init + boot screen
void startTask();

void requestCard();    // full redraw of the slate card
void requestStatus();  // partial redraw of the status bar
void showMenu(const MenuView& view);
void setPartial(bool on);   // partial refresh for menu / status bar (default off)
bool partialEnabled();
void probePins();         // serial: epdpins (BUSY / RST check)
void recoveryTest(int seconds, bool wifiOff);   // serial: epdrecover SEC [wifioff]
void requestLab();           // serial: epdlab (same picture, different SPI clocks / matrices on/off)
void requestTest();    // hardware test picture (serial: epdtest)
void closeMenu();      // back to the slate card

void setStatusProvider(String (*provider)());

}  // namespace epd
