#include "ui.h"
#include "bootsel.h"
#include "clap.h"
#include "epaper.h"
#include <ArcCore.h>
#include "matrix.h"
#include "matrix_slate.h"
#include "slate_link.h"
#include "pins.h"
#include "state.h"

namespace {

// ── encoder (full-step state table, robust against contact bounce) ─────────
constexpr uint8_t R_START = 0x0, R_CW_FINAL = 0x1, R_CW_BEGIN = 0x2, R_CW_NEXT = 0x3;
constexpr uint8_t R_CCW_BEGIN = 0x4, R_CCW_FINAL = 0x5, R_CCW_NEXT = 0x6;
constexpr uint8_t DIR_CW = 0x10, DIR_CCW = 0x20;
const uint8_t kTable[7][4] = {
    {R_START, R_CW_BEGIN, R_CCW_BEGIN, R_START},
    {R_CW_NEXT, R_START, R_CW_FINAL, R_START | DIR_CW},
    {R_CW_NEXT, R_CW_BEGIN, R_START, R_START},
    {R_CW_NEXT, R_CW_BEGIN, R_CW_FINAL, R_START},
    {R_CCW_NEXT, R_START, R_CCW_BEGIN, R_START},
    {R_CCW_NEXT, R_CCW_FINAL, R_START, R_START | DIR_CCW},
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};
volatile uint8_t s_encState = R_START;
volatile int32_t s_encSteps = 0;
portMUX_TYPE s_encMux = portMUX_INITIALIZER_UNLOCKED;
bool s_reverse = false;
bool s_debug = false;

void IRAM_ATTR isrEncoder() {
    const uint8_t pins = (digitalRead(PIN_ENC_DT) << 1) | digitalRead(PIN_ENC_CLK);
    s_encState = kTable[s_encState & 0x0F][pins];
    const uint8_t dir = s_encState & 0x30;
    portENTER_CRITICAL_ISR(&s_encMux);
    if (dir == DIR_CW) s_encSteps = s_encSteps + 1;
    else if (dir == DIR_CCW) s_encSteps = s_encSteps - 1;
    portEXIT_CRITICAL_ISR(&s_encMux);
}

// ── menu model ────────────────────────────────────────────────────────────
enum Item { IT_TAKE, IT_SHOT, IT_SCENE, IT_ROLL, IT_CAM, IT_MODE, IT_RATE, IT_BRIGHT, IT_FLASH, IT_APP, IT_INFO, IT_DONE, IT_COUNT };
// The menu is shown on the LED matrices: the TC chain shows the item while browsing ("-TAKE-" style labels,
// max. 9 characters = 56 columns) and the value while editing; the data chain keeps showing roll/shot/scene/take.
const char* const kItemNames[IT_COUNT] = {"TAKE", "SHOT", "SCENE", "ROLL", "CAMERA", "MODE",
                                          "FPS", "BRIGHT", "FLASH", "CLOCK", "INFO", "EXIT"};
const char* const kOverlay[IT_COUNT] = {"TAKE", "SHOT", "SC", "ROLL", "CAM", "", "", "BRT", "", "", "IP", ""};

using arc::engine::Mode;
using arc::TcRate;
const Mode kModes[] = {Mode::FreeRun, Mode::RecordRun, Mode::JamFree, Mode::JamRec, Mode::Slave};
const char* const kModeNames[] = {"FREE RUN", "REC RUN", "JAM FREE", "JAM REC", "SLAVE"};
const TcRate kRates[] = {TcRate::R24, TcRate::R25, TcRate::R2997DF, TcRate::R30};
const char* const kRateNames[] = {"24 FPS", "25 FPS", "29.97 DF", "30 FPS"};

enum UiState { UI_IDLE, UI_MENU, UI_EDIT };
UiState s_ui = UI_IDLE;
int s_sel = 0;
uint32_t s_lastInputMs = 0;
uint32_t s_cardDueMs = 0;     // deferred full e-ink refresh after take changes

// Edit state for items that are applied on confirm (mode / rate).
int s_pendingIndex = 0;
// Value backup for "discard".
ShotInfo s_backupShot;
uint8_t s_backupBright = 8, s_backupBrightNight = 1;

int indexOfMode(Mode m) {
    for (int i = 0; i < 5; i++) if (kModes[i] == m) return i;
    return 0;
}
int indexOfRate(TcRate r) {
    for (int i = 0; i < 4; i++) if (kRates[i] == r) return i;
    return 1;
}

String valueText(int item) {
    char buf[24];
    state::lock();
    const ShotInfo& s = state::shot();
    const Settings& st = state::settings();
    switch (item) {
        case IT_TAKE:  snprintf(buf, sizeof(buf), "%d", s.take); break;
        case IT_SHOT:  snprintf(buf, sizeof(buf), "%s", s.shot.c_str()); break;
        case IT_SCENE: snprintf(buf, sizeof(buf), "%d", s.scene); break;
        case IT_ROLL:  snprintf(buf, sizeof(buf), "%c%03d", s.cam, s.rollNum); break;
        case IT_CAM:   snprintf(buf, sizeof(buf), "%c", s.cam); break;
        case IT_BRIGHT: snprintf(buf, sizeof(buf), "%s%u", slatematrix::isNight() ? "N" : "T", slatematrix::isNight() ? st.brightnessNight : st.brightness); break;
        case IT_INFO:  snprintf(buf, sizeof(buf), "%s", arc::net::ipString().c_str()); break;
        default: buf[0] = 0;
    }
    state::unlock();
    if (item == IT_MODE) return (s_ui == UI_EDIT && s_sel == IT_MODE) ? kModeNames[s_pendingIndex] : kModeNames[indexOfMode(arc::engine::mode())];
    if (item == IT_RATE) return (s_ui == UI_EDIT && s_sel == IT_RATE) ? kRateNames[s_pendingIndex] : kRateNames[indexOfRate(arc::engine::rate())];
    return String(buf);
}

void renderMenu() {
    if (s_ui != UI_EDIT) {
        // browsing: item name; the live value of take/shot/scene/roll is on the data chain anyway
        mtx::showOverlay(String("-") + kItemNames[s_sel] + "-", 0);
    } else if (kOverlay[s_sel][0]) {
        mtx::showOverlay(String(kOverlay[s_sel]) + " " + valueText(s_sel), 0);
    } else {
        mtx::showOverlay(valueText(s_sel), 0);   // mode, frame rate
    }
}

void openMenu() {
    s_ui = UI_MENU;
    s_sel = 0;
    renderMenu();
}

void closeMenu() {
    s_ui = UI_IDLE;
    mtx::clearOverlay();
}

// Live value changes (take, scene, roll, camera, brightness).
void stepValue(int item, int delta) {
    bool userBits = false;
    state::lock();
    ShotInfo& s = state::shot();
    switch (item) {
        case IT_TAKE:  s.take = constrain(s.take + delta, 0, 99); userBits = true; break;
        case IT_SCENE: s.scene = constrain(s.scene + delta, 0, 99); userBits = true; break;
        case IT_ROLL:  s.rollNum = constrain(s.rollNum + delta, 1, 127); userBits = true; break;
        case IT_CAM: {
            // A–F: the camera is a single hex nibble in the LTC user bits.
            int c = s.cam - 'A' + delta;
            c = (c % 6 + 6) % 6;
            s.cam = (char)('A' + c);
            userBits = true;
            break;
        }
        case IT_SHOT: {
            const int n = constrain(s.shot.toInt() + delta, 0, 99);
            s.shot = String(n);
            userBits = true;
            break;
        }
        case IT_BRIGHT: {           // the value of the current phase (day / night); the renderer applies it
            Settings& st = state::settings();
            uint8_t& b = slatematrix::isNight() ? st.brightnessNight : st.brightness;
            b = (uint8_t)constrain((int)b + delta, 0, 15);
            break;
        }
        default: break;
    }
    if (userBits) s.loaded = true;
    state::touch();
    state::unlock();
    if (userBits) {
        slate::updateUserBits();
        slate::sendSlateState();
        s_cardDueMs = millis() + 1500;
    }
}

void onTurn(int steps) {
    if (s_ui == UI_IDLE) {
        stepValue(IT_TAKE, steps);
        state::lock();
        const int take = state::shot().take;
        state::unlock();
        mtx::showOverlay("TAKE " + String(take), 1200);
        return;
    }
    if (s_ui == UI_MENU) {
        s_sel = ((s_sel + steps) % IT_COUNT + IT_COUNT) % IT_COUNT;
        renderMenu();
        return;
    }
    // UI_EDIT
    if (s_sel == IT_MODE) {
        s_pendingIndex = ((s_pendingIndex + steps) % 5 + 5) % 5;
    } else if (s_sel == IT_RATE) {
        s_pendingIndex = ((s_pendingIndex + steps) % 4 + 4) % 4;
    } else if (s_sel == IT_SHOT && arc::net::studioConnected()) {
        // ARC-Studio owns the shot list: ask it for the neighbouring shot.
        slate::requestShotStep(steps > 0 ? 1 : -1);
    } else if (s_sel == IT_SHOT && slate::stepShotOffline(steps > 0 ? 1 : -1)) {
        // offline project: next/previous shot of the stored shot list
    } else {
        stepValue(s_sel, steps);
    }
    renderMenu();
}

void onShortPress() {
    if (s_ui == UI_IDLE) { openMenu(); return; }
    if (s_ui == UI_MENU) {
        switch (s_sel) {
            case IT_DONE: closeMenu(); return;
            case IT_FLASH: clap::testFlash(); return;
            case IT_APP:
                // Boots the other firmware (ARC-Clock); no return on success.
                mtx::showOverlay("CLOCK", 0);
                if (!bootsel::switchToOther()) mtx::showOverlay("CLOCK N/A", 2500);
                return;
            case IT_MODE: s_pendingIndex = indexOfMode(arc::engine::mode()); break;
            case IT_RATE: s_pendingIndex = indexOfRate(arc::engine::rate()); break;
            default: break;
        }
        state::lock();
        s_backupShot = state::shot();
        s_backupBright = state::settings().brightness;
        s_backupBrightNight = state::settings().brightnessNight;
        state::unlock();
        s_ui = UI_EDIT;
        renderMenu();
        return;
    }
    // UI_EDIT: apply
    if (s_sel == IT_MODE) arc::engine::requestMode(kModes[s_pendingIndex]);
    if (s_sel == IT_RATE) arc::engine::setRate(kRates[s_pendingIndex]);
    s_ui = UI_MENU;
    renderMenu();
}

void onLongPress() {
    if (s_ui == UI_IDLE) {
        if (arc::engine::jam().muteHeld) {
            arc::engine::confirmUnplug();
            mtx::showOverlay("TC ON J2", 1500);
            arc::net::broadcastStatus();
        } else if (arc::engine::mode() == Mode::RecordRun) {
            const bool run = !arc::engine::running();
            if (run) arc::engine::start(); else arc::engine::stop();
            mtx::showOverlay(run ? "REC" : "STOP", 1000);
        }
        return;
    }
    if (s_ui == UI_MENU) { closeMenu(); return; }
    // UI_EDIT: discard
    state::lock();
    state::shot() = s_backupShot;
    state::settings().brightness = s_backupBright;
    state::settings().brightnessNight = s_backupBrightNight;
    state::touch();
    state::unlock();
    slate::updateUserBits();
    s_ui = UI_MENU;
    renderMenu();
}

void uiTask(void*) {
    bool lastPressed = false;
    uint32_t pressStart = 0, lastChange = 0;
    bool longFired = false;
    uint32_t lastRev = state::revision();
    for (;;) {
        const uint32_t now = millis();

        // Encoder steps
        int32_t steps = 0;
        portENTER_CRITICAL(&s_encMux);
        steps = s_encSteps;
        s_encSteps = 0;
        portEXIT_CRITICAL(&s_encMux);
        if (steps) {
            if (s_reverse) steps = -steps;
            if (s_debug) Serial.printf("[ENC] %+ld\n", (long)steps);
            s_lastInputMs = now;
            onTurn(steps);
        }

        // Button: debounce 30 ms, long press at 800 ms
        const bool pressed = digitalRead(PIN_ENC_SW) == LOW;
        if (pressed != lastPressed && now - lastChange > 30) {
            lastChange = now;
            lastPressed = pressed;
            if (pressed) { pressStart = now; longFired = false; }
            else if (!longFired) {
                if (s_debug) Serial.println("[ENC] kurz");
                s_lastInputMs = now;
                onShortPress();
            }
        }
        if (pressed && !longFired && now - pressStart > 800) {
            longFired = true;
            if (s_debug) Serial.println("[ENC] lang");
            s_lastInputMs = now;
            onLongPress();
        }

        // Menu timeout
        if (s_ui != UI_IDLE && now - s_lastInputMs > 10000) closeMenu();

        // Values changed elsewhere (ARC-Studio): refresh the open menu.
        const uint32_t rev = state::revision();
        if (rev != lastRev) {
            lastRev = rev;
            if (s_ui != UI_IDLE) renderMenu();
        }

        // Deferred slate card refresh (QR code) after take changes; not while the menu is open,
        // so the e-ink does not flash on every value change (it follows when the menu closes).
        if (s_cardDueMs && s_ui == UI_IDLE && (int32_t)(now - s_cardDueMs) >= 0) {
            s_cardDueMs = 0;
            epd::requestCard();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

namespace ui {

void begin() {
    state::lock();
    s_reverse = state::settings().encReverse;
    state::unlock();
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT, INPUT_PULLUP);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), isrEncoder, CHANGE);
    xTaskCreatePinnedToCore(uiTask, "UI", 4096, nullptr, 2, nullptr, 0);
}

bool menuOpen() { return s_ui != UI_IDLE; }

void setEncoderDebug(bool on) { s_debug = on; }

}  // namespace ui
