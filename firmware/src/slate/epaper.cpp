#include "epaper.h"
#include "epd_driver.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansOblique18pt7b.h>
#include <qrcode.h>   // ESP-IDF esp_qrcode
#include <ArcCore.h>
#include "assets/bootscreen.h"
#include "assets/logo.h"
#include "pins.h"
#include "spi_bus.h"
#include "state.h"

namespace {

epddrv::Display& s_display = epddrv::display();
using epddrv::printAt;
using epddrv::printCentered;

// Layout (pixels). Partial windows must start/end on multiples of 8 in x.
constexpr int W = epddrv::W, H = epddrv::H;
constexpr int LEFT_W = 336;                 // production column
constexpr int RIGHT_X = 344, RIGHT_W = W - RIGHT_X;
constexpr int STATUS_Y = 448, STATUS_H = H - STATUS_Y;
constexpr int PANEL_H = STATUS_Y - 4;       // right panel height (QR / menu)

constexpr uint32_t BIT_CARD = 1, BIT_STATUS = 2, BIT_MENU = 4, BIT_CLOSE_MENU = 8, BIT_TEST = 16, BIT_LAB = 32;
constexpr int PARTIALS_BEFORE_FULL = 25;    // full refresh against ghosting

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_viewMtx = nullptr;
MenuView s_menu;
bool s_menuOpen = false;
String (*s_statusProvider)() = nullptr;
int s_partials = 0;

// ── QR code via ESP-IDF ─────────────────────────────────────────────────
constexpr int QR_MAX = 45;                  // up to version 7
uint8_t s_qrModules[QR_MAX][QR_MAX];
int s_qrSize = 0;

void qrCapture(esp_qrcode_handle_t qr) {
    s_qrSize = esp_qrcode_get_size(qr);
    if (s_qrSize > QR_MAX) { s_qrSize = 0; return; }
    for (int y = 0; y < s_qrSize; y++)
        for (int x = 0; x < s_qrSize; x++) s_qrModules[y][x] = esp_qrcode_get_module(qr, x, y) ? 1 : 0;
}

bool makeQr(const String& text) {
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qrCapture;
    cfg.max_qrcode_version = 7;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    s_qrSize = 0;
    return esp_qrcode_generate(&cfg, text.c_str()) == ESP_OK && s_qrSize > 0;
}

void drawStatus() {
    s_display.fillRect(0, STATUS_Y - 4, W, STATUS_H + 4, GxEPD_WHITE);
    s_display.fillRect(0, STATUS_Y - 4, W, 2, GxEPD_BLACK);
    const String line = s_statusProvider ? s_statusProvider() : String("");
    printAt(8, STATUS_Y + 22, &FreeSans9pt7b, line);
}

void drawLeftColumn(const ShotInfo& s) {
    const char* labels[] = {"PRODUCTION:", "DIRECTOR:", "CAMERA:", "DATE:"};
    char date[16];
    const DateTime now = arc::clock::now();
    snprintf(date, sizeof(date), "%02d.%02d.%04d", now.day(), now.month(), now.year());
    const String values[] = {s.production, s.director, s.camera, String(date)};
    for (int i = 0; i < 4; i++) {
        const int y0 = i * 86;
        if (i > 0) s_display.fillRect(0, y0, LEFT_W - 8, 3, GxEPD_BLACK);
        printAt(4, y0 + 22, &FreeSansBold9pt7b, labels[i]);
        printAt(12, y0 + 68, &FreeSansOblique18pt7b, values[i]);
    }
    s_display.fillRect(0, 4 * 86, LEFT_W - 8, 3, GxEPD_BLACK);
    if (s.scene != 0) {
        printAt(4, 392, &FreeSansBold24pt7b, s.environment);
        printAt(4, 438, &FreeSansBold24pt7b, s.daynight);
    }
    if (s.take != 0) {
        char fpsText[12];
        if (fabsf(s.fps - 29.97f) < 0.01f) snprintf(fpsText, sizeof(fpsText), "29.97");
        else snprintf(fpsText, sizeof(fpsText), "%dFPS", (int)(s.fps + 0.5f));
        printAt(170, 392, &FreeSansBold24pt7b, fpsText);
        if (s.mos) printAt(170, 438, &FreeSansBold24pt7b, "MOS");
    }
    s_display.fillRect(LEFT_W - 4, 0, 3, STATUS_Y - 4, GxEPD_BLACK);
}

void drawRightPanelCard(const ShotInfo& s, const String& roll) {
    s_display.fillRect(RIGHT_X, 0, RIGHT_W, PANEL_H, GxEPD_WHITE);
    const bool haveTake = s.take != 0 && s.code.length() > 0 && s.code != "-";
    if (haveTake) {
        char payload[96];
        snprintf(payload, sizeof(payload), "ARC2|%s|%s|%d|%s|%d", s.code.c_str(), roll.c_str(), s.scene,
                 s.shot.c_str(), s.take);
        if (makeQr(String(payload))) {
            const int px = min(PANEL_H, RIGHT_W) / (s_qrSize + 2);   // + quiet zone
            const int size = px * s_qrSize;
            const int x0 = RIGHT_X + (RIGHT_W - size) / 2;
            const int y0 = (PANEL_H - size) / 2;
            for (int y = 0; y < s_qrSize; y++)
                for (int x = 0; x < s_qrSize; x++)
                    if (s_qrModules[y][x]) s_display.fillRect(x0 + x * px, y0 + y * px, px, px, GxEPD_BLACK);
            return;
        }
    }
    s_display.drawInvertedBitmap(RIGHT_X + (RIGHT_W - 170) / 2, 70, logo, 170, 170, GxEPD_BLACK);
    printCentered(RIGHT_X + RIGHT_W / 2, 300, &FreeSansBold18pt7b, "NO TAKE SELECTED");
    printCentered(RIGHT_X + RIGHT_W / 2, 336, &FreeSans9pt7b, "Load a project in ARC-Studio or pick a take with the encoder");
}

void drawMenuPanel(const MenuView& m) {
    s_display.fillRect(RIGHT_X, 0, RIGHT_W, PANEL_H, GxEPD_WHITE);
    s_display.fillRect(RIGHT_X, 0, RIGHT_W, 44, GxEPD_BLACK);
    s_display.setTextColor(GxEPD_WHITE);
    printAt(RIGHT_X + 14, 31, &FreeSansBold12pt7b, m.title);
    s_display.setTextColor(GxEPD_BLACK);
    const int rowH = 32, top = 54;
    for (int i = 0; i < m.count; i++) {
        const int y = top + i * rowH;
        const bool sel = i == m.selected;
        if (sel) {
            if (m.editing) s_display.drawRect(RIGHT_X + 6, y, RIGHT_W - 12, rowH - 2, GxEPD_BLACK);
            else s_display.fillRect(RIGHT_X + 6, y, RIGHT_W - 12, rowH - 2, GxEPD_BLACK);
        }
        s_display.setTextColor(sel && !m.editing ? GxEPD_WHITE : GxEPD_BLACK);
        printAt(RIGHT_X + 16, y + 22, &FreeSansBold12pt7b, m.items[i]);
        if (m.values[i].length()) {
            int16_t bx, by; uint16_t bw, bh;
            s_display.getTextBounds(m.values[i], 0, 0, &bx, &by, &bw, &bh);
            const String v = (sel && m.editing) ? "< " + m.values[i] + " >" : m.values[i];
            s_display.getTextBounds(v, 0, 0, &bx, &by, &bw, &bh);
            s_display.setCursor(W - 20 - (int)bw, y + 22);
            s_display.print(v);
        }
        s_display.setTextColor(GxEPD_BLACK);
    }
    if (m.hint.length()) printAt(RIGHT_X + 14, PANEL_H - 12, &FreeSans9pt7b, m.hint);
}

// Test picture for hardware debugging: border, diagonals, ruler, stripes, checkerboard, text.
// What goes wrong shows what is wrong: noise = SPI signal (clock too fast, loading by the matrices),
// shifted or wrapped picture = wrong panel type / window, nothing = wiring / reset.
void renderTestPattern() {
    s_display.setFullWindow();
    s_display.fillScreen(GxEPD_WHITE);
    s_display.setTextColor(GxEPD_BLACK);
    s_display.drawRect(0, 0, W, H, GxEPD_BLACK);
    s_display.drawRect(3, 3, W - 6, H - 6, GxEPD_BLACK);
    s_display.drawLine(0, 0, W - 1, H - 1, GxEPD_BLACK);
    s_display.drawLine(0, H - 1, W - 1, 0, GxEPD_BLACK);
    for (int x = 100; x < W; x += 100) {                       // ruler along the top and bottom
        s_display.drawFastVLine(x, 0, 24, GxEPD_BLACK);
        s_display.drawFastVLine(x, H - 24, 24, GxEPD_BLACK);
        printAt(x + 4, 40, &FreeSans9pt7b, String(x));
    }
    for (int y = 100; y < H; y += 100) {
        s_display.drawFastHLine(0, y, 24, GxEPD_BLACK);
        s_display.drawFastHLine(W - 24, y, 24, GxEPD_BLACK);
        printAt(30, y + 5, &FreeSans9pt7b, String(y));
    }
    for (int i = 0; i < 16; i++)                                 // stripes of growing width
        s_display.fillRect(60 + i * 10 + i * (i - 1) / 2, 80, i + 1, 60, GxEPD_BLACK);
    for (int y = 0; y < 8; y++)                                  // checkerboard
        for (int x = 0; x < 8; x++)
            if ((x + y) & 1) s_display.fillRect(560 + x * 12, 300 + y * 12, 12, 12, GxEPD_BLACK);
    s_display.fillRect(600, 80, 120, 60, GxEPD_BLACK);           // solid block
    printCentered(W / 2, 200, &FreeSansBold24pt7b, "ARC-SLATE E-INK TEST");
    printCentered(W / 2, 240, &FreeSansBold12pt7b, String("800x480  SPI ") + String(epddrv::spiHz() / 1000) + " kHz");
    printCentered(W / 2, 280, &FreeSans9pt7b, "lines, ruler, stripes and checkerboard must be sharp and undistorted");
    s_display.display(false);
    s_partials = 0;
}

// Lab: the same picture with different SPI clocks, matrices on / off. The setting is printed into the picture,
// so "which numbers are sharp?" tells what the distortion depends on.
void runLab() {
    const uint32_t speeds[] = {4000000, 2000000, 1000000, 500000};
    const uint32_t saved = epddrv::spiHz();
    int n = 0;
    for (uint32_t hz : speeds) {
        for (int matricesOn = 1; matricesOn >= 0; matricesOn--) {
            n++;
            epddrv::setSpiHz(hz);
            epddrv::MatrixPause pause(!matricesOn);
            spibus::lock();
            s_display.setFullWindow();
            s_display.fillScreen(GxEPD_WHITE);
            s_display.drawInvertedBitmap(0, 0, bootscreen_V2, W, H, GxEPD_BLACK);
            s_display.fillRect(0, 420, W, 60, GxEPD_WHITE);
            s_display.drawRect(0, 420, W, 60, GxEPD_BLACK);
            printCentered(W / 2, 462, &FreeSansBold18pt7b,
                          String("LAB ") + n + "/8   SPI " + (hz / 1000) + " kHz   matrices " + (matricesOn ? "ON" : "OFF"));
            const uint32_t t0 = millis();
            s_display.display(false);
            spibus::unlock();
            Serial.println(String("[LAB] ") + n + "/8 SPI " + (hz / 1000) + " kHz, matrices " + (matricesOn ? "on" : "off") +
                           ": " + (millis() - t0) + " ms");
        }
    }
    epddrv::setSpiHz(saved);
    s_partials = 0;
    Serial.println("[LAB] done");
}

void renderCard() {
    state::lock();
    const ShotInfo s = state::shot();
    state::unlock();
    const String roll = state::rollString();
    s_display.setFullWindow();
    s_display.fillScreen(GxEPD_WHITE);
    s_display.setTextColor(GxEPD_BLACK);
    drawLeftColumn(s);
    drawRightPanelCard(s, roll);
    drawStatus();
    const uint32_t t0 = millis();
    s_display.display(false);
    s_partials = 0;
    Serial.println(String("[EPD] card drawn in ") + (millis() - t0) + " ms");
}

// Partial refresh is switched off until it is proven to look right on this panel (serial: epdpartial 1):
// without it every change is a full refresh (about 4 s) but the picture cannot get garbled.
bool s_partialOk = false;

void partial(int x, int y, int w, int h) {
    if (!s_partialOk || ++s_partials > PARTIALS_BEFORE_FULL) {
        s_display.display(false);
        s_partials = 0;
    } else {
        s_display.displayWindow(x, y, w, h);
    }
}

void drawBootPicture() {
    epddrv::MatrixPause pause;
    spibus::lock();
    s_display.setFullWindow();
    s_display.fillScreen(GxEPD_WHITE);
    s_display.drawInvertedBitmap(0, 0, bootscreen_V2, W, H, GxEPD_BLACK);
    const uint32_t t0 = millis();
    s_display.display(false);
    spibus::unlock();
    Serial.println(String("[EPD] boot picture drawn in ") + (millis() - t0) + " ms");
}

void epdTask(void*) {
    // Connecting to the WiFi draws current peaks that dip the 3V3 rail. Refreshes done meanwhile came out
    // garbled, so the first picture waits until the network is up (or 30 s have passed).
    const uint32_t start = millis();
    while (arc::net::role() == arc::net::Role::Offline && millis() - start < 30000) vTaskDelay(pdMS_TO_TICKS(250));
    vTaskDelay(pdMS_TO_TICKS(2000));
    drawBootPicture();
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &bits, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(150));   // coalesce bursts (encoder turns)
        uint32_t more = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &more, 0);
        bits |= more;

        // full refreshes (card, test picture, or every change while partial refresh is off) run with the matrices dark
        const bool fullRefresh = (bits & (BIT_CARD | BIT_TEST)) || !s_partialOk;
        epddrv::MatrixPause pause(fullRefresh);
        if (bits & BIT_LAB) {
            runLab();
            continue;
        }
        if (bits & BIT_TEST) {
            spibus::lock();
            renderTestPattern();
            spibus::unlock();
            continue;
        }
        spibus::lock();
        if (bits & BIT_CLOSE_MENU) {
            s_menuOpen = false;
            bits |= BIT_CARD;
        }
        if (bits & BIT_MENU) {
            xSemaphoreTake(s_viewMtx, portMAX_DELAY);
            const MenuView m = s_menu;
            xSemaphoreGive(s_viewMtx);
            s_menuOpen = true;
            drawMenuPanel(m);
            if (bits & BIT_STATUS) drawStatus();
            partial(RIGHT_X, 0, RIGHT_W, (bits & BIT_STATUS) ? H : PANEL_H);
        } else if (bits & BIT_CARD) {
            if (s_menuOpen) {
                // Card data changed while the menu is open: draw it later.
                drawStatus();
                partial(0, STATUS_Y - 4, W, STATUS_H + 4);
            } else {
                renderCard();
            }
        } else if (bits & BIT_STATUS) {
            drawStatus();
            partial(0, STATUS_Y - 4, W, STATUS_H + 4);
        }
        spibus::unlock();
    }
}

}  // namespace

namespace epd {

void begin() {
    s_viewMtx = xSemaphoreCreateMutex();
    epddrv::init();           // the first picture is drawn by the task once the WiFi has settled
}

void startTask() {
    xTaskCreatePinnedToCore(epdTask, "EPD", 6144, nullptr, 1, &s_task, 0);
}

void requestCard() { if (s_task) xTaskNotify(s_task, BIT_CARD, eSetBits); }
void requestStatus() { if (s_task) xTaskNotify(s_task, BIT_STATUS, eSetBits); }

void showMenu(const MenuView& view) {
    xSemaphoreTake(s_viewMtx, portMAX_DELAY);
    s_menu = view;
    xSemaphoreGive(s_viewMtx);
    if (s_task) xTaskNotify(s_task, BIT_MENU, eSetBits);
}

void closeMenu() { if (s_task) xTaskNotify(s_task, BIT_CLOSE_MENU, eSetBits); }

void setPartial(bool on) {
    s_partialOk = on;
    Serial.println(on ? "E-Ink partial refresh: on" : "E-Ink partial refresh: off (full refresh only)");
}
bool partialEnabled() { return s_partialOk; }

void requestLab() { if (s_task) xTaskNotify(s_task, BIT_LAB, eSetBits); }

void requestTest() { if (s_task) xTaskNotify(s_task, BIT_TEST, eSetBits); }

void setStatusProvider(String (*provider)()) { s_statusProvider = provider; }

}  // namespace epd

namespace epd {

// Electrical check of the panel: BUSY level at rest, then a reset pulse and every BUSY transition for 1.5 s.
// A living UC8179 panel pulls BUSY low for a moment around the reset and releases it afterwards.
void probePins() {
    epddrv::MatrixPause pause;
    spibus::lock();
    pinMode(PIN_EPD_BUSY, INPUT);
    Serial.println(String("[EPD] BUSY at rest      : ") + digitalRead(PIN_EPD_BUSY) + "  (idle level is 1 for this panel)");
    pinMode(PIN_EPD_RST, OUTPUT);
    digitalWrite(PIN_EPD_RST, LOW);
    delay(20);
    Serial.println(String("[EPD] BUSY with RST low : ") + digitalRead(PIN_EPD_BUSY));
    digitalWrite(PIN_EPD_RST, HIGH);
    uint32_t times[16];
    uint8_t levels[16];
    int n = 0;
    int last = digitalRead(PIN_EPD_BUSY);
    const uint32_t t0 = micros();
    while (micros() - t0 < 1500000 && n < 16) {
        const int v = digitalRead(PIN_EPD_BUSY);
        if (v != last) { times[n] = micros() - t0; levels[n++] = (uint8_t)v; last = v; }
    }
    Serial.println(String("[EPD] BUSY after RST high: ") + (n ? "starts" : "stays at") + " " + (n ? (int)!levels[0] : last) +
                   ", " + n + " transition(s) in 1.5 s");
    for (int i = 0; i < n; i++) Serial.println(String("       t=") + (times[i] / 1000.0f) + " ms -> " + levels[i]);
    spibus::unlock();
    Serial.println("[EPD] the panel needs a re-init now: run 'epd' or reset the board");
}

}  // namespace epd
