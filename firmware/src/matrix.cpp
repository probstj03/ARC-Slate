#include "matrix.h"
#include <SPI.h>
#include "pins.h"
#include "spi_bus.h"
#include "state.h"

namespace {

constexpr uint8_t REG_DECODE = 0x09, REG_INTENSITY = 0x0A, REG_SCAN_LIMIT = 0x0B;
constexpr uint8_t REG_SHUTDOWN = 0x0C, REG_DISPLAY_TEST = 0x0F;
constexpr int MAX_MODULES = 9;

struct ChainHw {
    int load;
    int count;
    uint8_t rows[MAX_MODULES][8];   // last frame sent
    uint8_t next[MAX_MODULES][8];   // frame to send
};

ChainHw s_chains[2] = {
    { PIN_MTX_TC_LOAD, 7, {}, {} },
    { PIN_MTX_DATA_LOAD, 9, {}, {} },
};

// Data chain layout: which modules show which field. A "flipped" row is
// mounted rotated by 180° with the chain running backwards (serpentine).
struct Field { int firstModule; int count; bool flipped; };
constexpr Field FIELD_ROLL       = { 0, 3, false };
constexpr Field FIELD_SHOT       = { 3, 3, true };
constexpr Field FIELD_SCENE_TAKE = { 6, 3, false };

const SPISettings kSpi(1000000, MSBFIRST, SPI_MODE0);

volatile uint8_t s_brightness = 8;
volatile bool s_brightnessDirty = true;
volatile uint8_t s_testMode = 0;
portMUX_TYPE s_ovMux = portMUX_INITIALIZER_UNLOCKED;
char s_overlay[24] = {0};
uint32_t s_overlayUntil = 0;
bool s_overlayActive = false;

// ── font: 5 columns per glyph, bit 6 = top row ──────────────────────────
const uint8_t FONT_DIGIT[10][5] = {
    {0x3E, 0x45, 0x49, 0x51, 0x3E}, {0x00, 0x21, 0x7F, 0x01, 0x00}, {0x21, 0x43, 0x45, 0x49, 0x31},
    {0x22, 0x41, 0x49, 0x49, 0x36}, {0x0C, 0x14, 0x24, 0x7F, 0x04}, {0x72, 0x51, 0x51, 0x51, 0x4E},
    {0x3E, 0x49, 0x49, 0x49, 0x26}, {0x40, 0x40, 0x4F, 0x50, 0x60}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x32, 0x49, 0x49, 0x49, 0x3E},
};
const uint8_t FONT_LETTER[26][5] = {
    {0x3F, 0x48, 0x48, 0x48, 0x3F}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x48, 0x48, 0x48, 0x40},
    {0x3E, 0x41, 0x49, 0x49, 0x2E}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x06, 0x01, 0x01, 0x01, 0x7E}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x01, 0x01, 0x01, 0x01},
    {0x7F, 0x20, 0x10, 0x20, 0x7F}, {0x7F, 0x10, 0x08, 0x04, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x48, 0x48, 0x48, 0x30}, {0x3E, 0x41, 0x45, 0x42, 0x3D}, {0x7F, 0x48, 0x4C, 0x4A, 0x31},
    {0x31, 0x49, 0x49, 0x49, 0x46}, {0x40, 0x40, 0x7F, 0x40, 0x40}, {0x7E, 0x01, 0x01, 0x01, 0x7E},
    {0x7C, 0x02, 0x01, 0x02, 0x7C}, {0x7F, 0x02, 0x04, 0x02, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x60, 0x10, 0x0F, 0x10, 0x60}, {0x43, 0x45, 0x49, 0x51, 0x61},
};

// Renders text into display columns (left to right). Returns used width.
int renderText(const char* text, uint8_t* cols, int width, int padLeft, bool gapAfterSecond = false) {
    memset(cols, 0, width);
    int c = padLeft;
    for (int i = 0; text[i] && c < width; i++) {
        const char ch = (char)toupper((unsigned char)text[i]);
        const uint8_t* glyph = nullptr;
        uint8_t narrow = 0;
        int w = 5;
        if (ch >= '0' && ch <= '9') glyph = FONT_DIGIT[ch - '0'];
        else if (ch >= 'A' && ch <= 'Z') glyph = FONT_LETTER[ch - 'A'];
        else if (ch == ':') { narrow = 0x14; w = 1; }
        else if (ch == '.') { narrow = 0x01; w = 1; }
        else if (ch == '-') { w = 3; }
        else { w = 3; }   // space and unknown characters
        if (c + w > width) break;
        for (int k = 0; k < w; k++) {
            if (glyph) cols[c + k] = glyph[k];
            else if (narrow) cols[c + k] = narrow;
            else if (ch == '-') cols[c + k] = 0x08;
        }
        c += w + 1;
        if (gapAfterSecond && i == 1) c++;
    }
    return c > 0 ? c - 1 : 0;
}

void rotateCW(uint8_t* m) {
    uint8_t t[8] = {0};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            if (m[i] & (1 << j)) t[7 - j] |= (1 << i);
    memcpy(m, t, 8);
}

// Puts `width` columns onto `count` modules starting at `first`.
// Same orientation as the original firmware: module `first` shows the
// rightmost 8 columns; flipped rows are rotated by 180° and run backwards.
void placeColumns(ChainHw& ch, const uint8_t* cols, int width, int first, int count, bool flipped) {
    for (int j = 0; j < count; j++) {
        uint8_t m[8];
        for (int k = 0; k < 8; k++) {
            const int idx = width - 1 - (j * 8 + k);
            m[k] = idx >= 0 ? cols[idx] : 0;
        }
        rotateCW(m);
        if (flipped) { rotateCW(m); rotateCW(m); }
        const int module = flipped ? first + (count - 1 - j) : first + j;
        memcpy(ch.next[module], m, 8);
    }
}

void sendAll(ChainHw& ch, uint8_t reg, uint8_t value) {
    SPI.beginTransaction(kSpi);
    digitalWrite(ch.load, LOW);
    for (int d = ch.count - 1; d >= 0; d--) SPI.transfer16(((uint16_t)reg << 8) | value);
    digitalWrite(ch.load, HIGH);
    SPI.endTransaction();
}

void flush(ChainHw& ch, bool force) {
    for (int r = 0; r < 8; r++) {
        bool changed = force;
        for (int d = 0; d < ch.count && !changed; d++) changed = ch.next[d][r] != ch.rows[d][r];
        if (!changed) continue;
        SPI.beginTransaction(kSpi);
        digitalWrite(ch.load, LOW);
        for (int d = ch.count - 1; d >= 0; d--) {
            SPI.transfer16(((uint16_t)(r + 1) << 8) | ch.next[d][r]);
            ch.rows[d][r] = ch.next[d][r];
        }
        digitalWrite(ch.load, HIGH);
        SPI.endTransaction();
    }
}

void initChain(ChainHw& ch) {
    pinMode(ch.load, OUTPUT);
    digitalWrite(ch.load, HIGH);
    sendAll(ch, REG_DISPLAY_TEST, 0);
    sendAll(ch, REG_SCAN_LIMIT, 7);
    sendAll(ch, REG_DECODE, 0);
    sendAll(ch, REG_INTENSITY, s_brightness);
    sendAll(ch, REG_SHUTDOWN, 1);
    memset(ch.next, 0, sizeof(ch.next));
    flush(ch, true);
}

void renderField(ChainHw& ch, const Field& f, const char* text, bool gapAfterSecond = false) {
    uint8_t cols[MAX_MODULES * 8];
    const int width = f.count * 8;
    const int used = renderText(text, cols, width, 0, gapAfterSecond);
    // Center the text inside the field.
    const int pad = used < width ? (width - used) / 2 : 0;
    renderText(text, cols, width, pad, gapAfterSecond);
    placeColumns(ch, cols, width, f.firstModule, f.count, f.flipped);
}

void renderData() {
    state::lock();
    const ShotInfo& s = state::shot();
    char roll[8], shot[8], sceneTake[8];
    snprintf(roll, sizeof(roll), "%c%03d", s.cam, s.rollNum);
    snprintf(shot, sizeof(shot), "%s", s.shot.c_str());
    snprintf(sceneTake, sizeof(sceneTake), "%2d%2d", s.scene % 100, s.take % 100);
    state::unlock();
    ChainHw& ch = s_chains[mtx::CHAIN_DATA];
    renderField(ch, FIELD_ROLL, roll);
    renderField(ch, FIELD_SHOT, shot);
    renderField(ch, FIELD_SCENE_TAKE, sceneTake, true);
}

void renderTimecode(const Timecode& tc) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u:%02u", tc.h, tc.m, tc.s, tc.f);
    uint8_t cols[56];
    renderText(buf, cols, 56, 2);   // 2 + 8x(5+1) + 3x(1+1) = 56 columns
    placeColumns(s_chains[mtx::CHAIN_TC], cols, 56, 0, 7, false);
}

void renderOverlay(const char* text) {
    uint8_t cols[56];
    const int used = renderText(text, cols, 56, 0);
    renderText(text, cols, 56, used < 56 ? (56 - used) / 2 : 0);
    placeColumns(s_chains[mtx::CHAIN_TC], cols, 56, 0, 7, false);
}

void renderTest(uint8_t mode) {
    for (int c = 0; c < 2; c++) {
        ChainHw& ch = s_chains[c];
        for (int d = 0; d < ch.count; d++) {
            if (mode == 1) { memset(ch.next[d], 0xFF, 8); continue; }
            uint8_t cols[8];
            char num[3];
            snprintf(num, sizeof(num), "%d", d);
            renderText(num, cols, 8, 2);
            uint8_t m[8];
            for (int k = 0; k < 8; k++) m[k] = cols[7 - k];
            rotateCW(m);
            memcpy(ch.next[d], m, 8);
        }
    }
}

void displayTask(void*) {
    uint32_t lastRev = 0;
    Timecode lastTc;
    bool first = true;
    uint8_t lastTest = 0;
    for (;;) {
        bool dataDirty = false, tcDirty = false;
        if (lastTest != s_testMode) {
            if (lastTest && !s_testMode) { first = true; lastRev = 0; }
            lastTest = s_testMode;
        }

        const uint32_t rev = state::revision();
        if (rev != lastRev) { lastRev = rev; dataDirty = true; }

        const uint32_t nowMs = millis();
        portENTER_CRITICAL(&s_ovMux);
        bool overlay = s_overlayActive;
        if (overlay && s_overlayUntil && (int32_t)(nowMs - s_overlayUntil) >= 0) {
            s_overlayActive = overlay = false;
        }
        portEXIT_CRITICAL(&s_ovMux);
        static bool wasOverlay = false;
        if (wasOverlay && !overlay) first = true;   // redraw TC after an overlay
        wasOverlay = overlay;

        if (s_testMode) {
            renderTest(s_testMode);
            tcDirty = dataDirty = true;
        } else if (overlay) {
            char text[sizeof(s_overlay)];
            portENTER_CRITICAL(&s_ovMux);
            memcpy(text, s_overlay, sizeof(text));
            portEXIT_CRITICAL(&s_ovMux);
            renderOverlay(text);
            tcDirty = true;
            if (dataDirty) renderData();
        } else {
            const Timecode tc = ltc::current();
            if (first || tc.f != lastTc.f || tc.s != lastTc.s || tc.m != lastTc.m || tc.h != lastTc.h) {
                lastTc = tc;
                renderTimecode(tc);
                tcDirty = true;
            }
            if (dataDirty) renderData();
        }

        if (tcDirty || dataDirty || s_brightnessDirty) {
            spibus::lock();
            if (s_brightnessDirty) {
                s_brightnessDirty = false;
                for (auto& ch : s_chains) sendAll(ch, REG_INTENSITY, s_brightness);
            }
            // Re-init every few seconds: recovers modules after brown-outs/glitches.
            static uint32_t lastInit = 0;
            if (millis() - lastInit > 10000) {
                lastInit = millis();
                for (auto& ch : s_chains) {
                    sendAll(ch, REG_SCAN_LIMIT, 7);
                    sendAll(ch, REG_DECODE, 0);
                    sendAll(ch, REG_SHUTDOWN, 1);
                }
                flush(s_chains[0], true);
                flush(s_chains[1], true);
            } else {
                if (tcDirty) flush(s_chains[0], false);
                if (dataDirty) flush(s_chains[1], false);
            }
            spibus::unlock();
        }
        first = false;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

}  // namespace

namespace mtx {

void begin() {
    state::lock();
    s_brightness = state::settings().brightness;
    state::unlock();
    spibus::lock();
    initChain(s_chains[0]);
    initChain(s_chains[1]);
    spibus::unlock();
}

void startTask() {
    xTaskCreatePinnedToCore(displayTask, "MTX", 4096, nullptr, 2, nullptr, 0);
}

void setBrightness(uint8_t level) {
    s_brightness = level > 15 ? 15 : level;
    s_brightnessDirty = true;
}

void showOverlay(const String& text, uint32_t durationMs) {
    char buf[sizeof(s_overlay)];
    strlcpy(buf, text.c_str(), sizeof(buf));
    portENTER_CRITICAL(&s_ovMux);
    memcpy(s_overlay, buf, sizeof(buf));
    s_overlayUntil = durationMs ? millis() + durationMs : 0;
    if (durationMs && s_overlayUntil == 0) s_overlayUntil = 1;
    s_overlayActive = true;
    portEXIT_CRITICAL(&s_ovMux);
}

void clearOverlay() {
    portENTER_CRITICAL(&s_ovMux);
    s_overlayActive = false;
    portEXIT_CRITICAL(&s_ovMux);
}

void setTestMode(uint8_t mode) { s_testMode = mode; }

}  // namespace mtx
