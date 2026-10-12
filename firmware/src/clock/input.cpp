#include "input.h"
#include <Preferences.h>
#include "pins.h"

namespace {

constexpr uint32_t LONG_PRESS_MS = 1500;
constexpr uint32_t DEBOUNCE_MS = 25;

QueueHandle_t s_queue = nullptr;
bool s_reverse = false;

void push(input::Event e) {
    if (s_queue) xQueueSend(s_queue, &e, 0);
}

void inputTask(void*) {
    // Quadrature decoding with a transition table; the KY-040 gives 4 transitions per detent.
    static const int8_t kTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    uint8_t state = ((digitalRead(PIN_ENC_CLK) ? 2 : 0) | (digitalRead(PIN_ENC_DT) ? 1 : 0));
    int acc = 0;

    bool btn = false;
    uint32_t btnSince = 0;
    bool longSent = false;
    bool btnRaw = false;
    uint32_t rawSince = 0;

    for (;;) {
        state = ((state << 2) | (digitalRead(PIN_ENC_CLK) ? 2 : 0) | (digitalRead(PIN_ENC_DT) ? 1 : 0)) & 0x0F;
        acc += kTable[state];
        if (acc >= 4) { push(s_reverse ? input::TURN_CCW : input::TURN_CW); acc = 0; }
        else if (acc <= -4) { push(s_reverse ? input::TURN_CW : input::TURN_CCW); acc = 0; }

        const bool raw = digitalRead(PIN_ENC_SW) == LOW;
        const uint32_t now = millis();
        if (raw != btnRaw) { btnRaw = raw; rawSince = now; }
        if (btnRaw != btn && now - rawSince >= DEBOUNCE_MS) {
            btn = btnRaw;
            if (btn) { btnSince = now; longSent = false; }
            else if (!longSent) push(input::PRESS_SHORT);
        }
        if (btn && !longSent && now - btnSince >= LONG_PRESS_MS) {
            longSent = true;
            push(input::PRESS_LONG);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

}  // namespace

namespace input {

void begin() {
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT, INPUT_PULLUP);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);
    Preferences p;
    if (p.begin("slate", true)) {          // same direction setting as the slate firmware
        s_reverse = p.getBool("encRev", false);
        p.end();
    }
    s_queue = xQueueCreate(16, sizeof(Event));
    xTaskCreatePinnedToCore(inputTask, "INPUT", 3072, nullptr, 3, nullptr, 1);
}

bool poll(Event& e) {
    return s_queue && xQueueReceive(s_queue, &e, 0) == pdTRUE;
}

}  // namespace input
