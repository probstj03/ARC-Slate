#include "epd_driver.h"
#include "matrix.h"
#include "pins.h"
#include "spi_bus.h"

namespace epddrv {

namespace {

uint32_t s_spiHz = 2000000;
Stats s_stats;

// Busy waits of the refresh in progress: GxEPD2 calls the callback every ~5 ms while BUSY is active.
// A gap of more than 12 ms between two calls starts a new wait (PowerOn, Update, PowerOff ...).
bool s_track = false;
uint8_t s_waits = 0;
uint32_t s_waitMs[6] = {0};
uint32_t s_waitStart = 0, s_lastCall = 0;

void busyCallback(const void*) {
    const uint32_t now = millis();
    if (s_track) {
        if (s_waits == 0 || now - s_lastCall > 12) {
            if (s_waits < 6) s_waits++;
            s_waitStart = now;
        }
        s_waitMs[s_waits - 1] = now - s_waitStart;
        s_lastCall = now;
    }
    // The panel can take seconds; give the bus to the LED matrices meanwhile.
    spibus::unlock();
    vTaskDelay(pdMS_TO_TICKS(5));
    spibus::lock();
}

// The booster gives up after ~2 s ("PowerOn" wait of 1.9..2.5 s without a long update after it), or the
// panel does not answer at all (no busy phase). A healthy full refresh has a wait of 3.5-5.5 s, a partial
// one 1.5-1.7 s.
bool looksFailed() {
    if (s_waits == 0) return true;
    bool timeoutLike = false;
    for (uint8_t i = 0; i < s_waits; i++) {
        if (s_waitMs[i] > 2500) return false;                     // a real full update ran
        if (s_waitMs[i] >= 1900) timeoutLike = true;
    }
    return timeoutLike;
}

// Hardware reset + init of the panel (caller holds the SPI lock). A short dip of the panel supply resets the
// controller and loses its configuration (booster, waveform, resolution): later refreshes come out washed out or
// distorted, or the panel ignores them. A reset before every full refresh makes each picture independent of that.
void resetPanelLocked() {
    Display& d = display();
    d.init(115200, true, 20, false);            // initial = true: the next refresh writes the whole picture
    d.epd2.setBusyCallback(busyCallback);
    d.epd2.selectSPI(SPI, SPISettings(s_spiHz, MSBFIRST, SPI_MODE0));
    spibus::setDrive(spibus::drive());
}

// One refresh (the matrices keep running) with a check that the panel really refreshed.
template <typename F>
bool runChecked(F doRefresh) {
    s_waits = 0;
    s_track = true;
    doRefresh();
    s_track = false;
    return !looksFailed();
}

// Full refresh: reset first; if the panel still did not refresh, reset and try once more.
bool fullRefreshLocked(const char* what) {
    const uint32_t t0 = millis();
    s_stats.refreshes++;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        resetPanelLocked();
        ok = runChecked([] { display().display(false); });
        if (!ok) {
            s_stats.failures++;
            Serial.printf("[EPD] %s: attempt %d - the panel did not refresh%s\n", what, attempt + 1,
                          attempt == 0 ? ", resetting and trying again" : " - check the 3V3 supply of the panel");
        }
    }
    s_stats.lastMs = millis() - t0;
    return ok;
}

}  // namespace

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
    spibus::setDrive(spibus::drive());
    spibus::unlock();
}

bool refreshFull() { return fullRefreshLocked("full refresh"); }

// A partial refresh needs the panel's previous picture, so no reset before it. If it fails, the panel has probably
// lost its state: a full refresh (with reset) of the whole frame buffer repairs the picture.
bool refreshWindow(int x, int y, int w, int h) {
    const uint32_t t0 = millis();
    s_stats.refreshes++;
    if (runChecked([x, y, w, h] { display().displayWindow(x, y, w, h); })) {
        s_stats.lastMs = millis() - t0;
        return true;
    }
    s_stats.failures++;
    Serial.println("[EPD] partial refresh: the panel did not refresh - full refresh with reset");
    s_stats.refreshes--;                        // counted again by the full refresh
    return fullRefreshLocked("full refresh after a failed partial");
}

Stats stats() { return s_stats; }

MatrixPause::MatrixPause(bool active) : _was(active && mtx::enabled()) {
    if (_was) {
        mtx::setEnabled(false);
        vTaskDelay(pdMS_TO_TICKS(120));     // the matrix task sends the shutdown command within a few ms
    }
}

MatrixPause::~MatrixPause() {
    if (_was) mtx::setEnabled(true);
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
