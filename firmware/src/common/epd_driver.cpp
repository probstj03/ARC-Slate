#include "epd_driver.h"
#include "matrix.h"
#include "pins.h"
#include "spi_bus.h"

namespace epddrv {

namespace {

uint32_t s_spiHz = 2000000;
Pause s_pause = Pause::PowerOn;
Stats s_stats;

// Busy waits of the refresh in progress: GxEPD2 calls the callback every ~5 ms while BUSY is active.
// A gap of more than 12 ms between two calls starts a new wait (PowerOn, Update, PowerOff ...).
bool s_track = false;
uint8_t s_waits = 0;
uint32_t s_waitMs[6] = {0};
uint32_t s_waitStart = 0, s_lastCall = 0;
uint32_t s_resumeAt = 0;           // millis() at which the matrices come back on (0 = not paused)

constexpr uint32_t POWER_ON_WINDOW_MS = 300;   // covers the booster start (~140 ms) with margin
constexpr uint32_t MATRIX_OFF_DELAY_MS = 60;   // the matrix task needs a few ms to send the shutdown

void busyCallback(const void*) {
    const uint32_t now = millis();
    if (s_track) {
        if (s_waits == 0 || now - s_lastCall > 12) {
            if (s_waits < 6) s_waits++;
            s_waitStart = now;
        }
        s_waitMs[s_waits - 1] = now - s_waitStart;
        s_lastCall = now;
        if (s_resumeAt && (int32_t)(now - s_resumeAt) >= 0) {
            s_resumeAt = 0;
            mtx::setEnabled(true);
        }
    }
    // The panel can take seconds; give the bus to the LED matrices meanwhile.
    spibus::unlock();
    vTaskDelay(pdMS_TO_TICKS(5));
    spibus::lock();
}

// Switches the matrices off while the caller holds the SPI lock (the matrix task needs the bus for it).
// Returns true if they were on.
bool matricesOffLocked() {
    if (!mtx::enabled()) return false;
    mtx::setEnabled(false);
    spibus::unlock();
    vTaskDelay(pdMS_TO_TICKS(MATRIX_OFF_DELAY_MS));
    spibus::lock();
    return true;
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

bool s_suspended = false;      // after a booster failure: no more refreshes until resume() / restart

// One refresh with the configured matrix pause and a check that the panel really refreshed. On a booster failure
// the panel is put to sleep and further refreshes are suspended: repeated attempts on a failing booster washed out
// the picture that was already on the panel. A deep sleep + hardware reset did not clear the state in tests; it
// needs a stronger supply (or the power removed for a moment).
template <typename F>
bool guardedRefresh(F doRefresh, const char* what) {
    if (s_suspended) return false;
    const uint32_t t0 = millis();
    s_stats.refreshes++;
    const bool wasOn = s_pause != Pause::Off && matricesOffLocked();
    s_resumeAt = (wasOn && s_pause == Pause::PowerOn) ? millis() + POWER_ON_WINDOW_MS : 0;
    s_waits = 0;
    s_track = true;
    doRefresh();
    s_track = false;
    if (wasOn) {
        s_resumeAt = 0;
        mtx::setEnabled(true);
    }
    s_stats.lastMs = millis() - t0;
    if (!looksFailed()) return true;
    s_stats.failures++;
    s_suspended = true;
    display().hibernate();
    Serial.printf("[EPD] %s: the panel booster did not start (supply too weak) - e-ink refreshes suspended, "
                  "the last picture stays. 'epdresume' or a restart tries again.\n", what);
    return false;
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
    spibus::unlock();
}

void setPause(Pause mode) { s_pause = mode; }
Pause pause() { return s_pause; }

const char* pauseName(Pause mode) {
    switch (mode) {
        case Pause::Off: return "off";
        case Pause::PowerOn: return "power-on (0.3 s)";
        default: return "full refresh";
    }
}

bool refreshFull() {
    return guardedRefresh([] { display().display(false); }, "full refresh");
}

bool refreshWindow(int x, int y, int w, int h) {
    return guardedRefresh([x, y, w, h] { display().displayWindow(x, y, w, h); }, "partial refresh");
}

Stats stats() { return s_stats; }

void resume() { s_suspended = false; }
bool suspended() { return s_suspended; }

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
