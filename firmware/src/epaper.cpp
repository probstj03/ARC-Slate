#include "epaper.h"
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansOblique18pt7b.h>
#include <qrcode.h>   // ESP-IDF esp_qrcode
#include "arc_clock.h"
#include "assets/bootscreen.h"
#include "assets/logo.h"
#include "pins.h"
#include "spi_bus.h"
#include "state.h"

namespace {

GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> s_display(
    GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// Layout (pixels). Partial windows must start/end on multiples of 8 in x.
constexpr int W = 800, H = 480;
constexpr int LEFT_W = 336;                 // production column
constexpr int RIGHT_X = 344, RIGHT_W = W - RIGHT_X;
constexpr int STATUS_Y = 448, STATUS_H = H - STATUS_Y;
constexpr int PANEL_H = STATUS_Y - 4;       // right panel height (QR / menu)

constexpr uint32_t BIT_CARD = 1, BIT_STATUS = 2, BIT_MENU = 4, BIT_CLOSE_MENU = 8;
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

// The panel can take seconds; give the bus to the LED matrices meanwhile.
void busyCallback(const void*) {
    spibus::unlock();
    vTaskDelay(pdMS_TO_TICKS(5));
    spibus::lock();
}

void printAt(int x, int y, const GFXfont* font, const String& text) {
    s_display.setFont(font);
    s_display.setCursor(x, y);
    s_display.print(text);
}

void printCentered(int cx, int y, const GFXfont* font, const String& text) {
    int16_t bx, by; uint16_t bw, bh;
    s_display.setFont(font);
    s_display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    s_display.setCursor(cx - (int)bw / 2 - bx, y);
    s_display.print(text);
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
    const DateTime now = arcclock::now();
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
    s_display.drawInvertedBitmap(RIGHT_X + (RIGHT_W - 400) / 2, 60, logo, 400, 130, GxEPD_BLACK);
    printCentered(RIGHT_X + RIGHT_W / 2, 290, &FreeSansBold18pt7b, "KEIN TAKE GEWAEHLT");
    printCentered(RIGHT_X + RIGHT_W / 2, 330, &FreeSans9pt7b, "Projekt in ARC-Studio laden oder Take am Encoder waehlen");
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
    s_display.display(false);
    s_partials = 0;
}

void partial(int x, int y, int w, int h) {
    if (++s_partials > PARTIALS_BEFORE_FULL) {
        s_display.display(false);
        s_partials = 0;
    } else {
        s_display.displayWindow(x, y, w, h);
    }
}

void epdTask(void*) {
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &bits, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(150));   // coalesce bursts (encoder turns)
        uint32_t more = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &more, 0);
        bits |= more;

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
    spibus::lock();
    s_display.init(0, true, 2, false);
    s_display.epd2.setBusyCallback(busyCallback);
    s_display.setRotation(0);
    s_display.setFullWindow();
    s_display.fillScreen(GxEPD_WHITE);
    s_display.drawInvertedBitmap(0, 0, bootscreen_V2, W, H, GxEPD_BLACK);
    s_display.display(false);
    spibus::unlock();
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

void setStatusProvider(String (*provider)()) { s_statusProvider = provider; }

}  // namespace epd
