// LTC generator, decoder and Jam Sync for ARC-Slate.
//
// Signal path: LTCGenerator (libltc, 48 kHz mono) -> LTCStereoOutput (right
// channel) -> I2S -> PCM5102 -> J2. Incoming LTC: J2 -> LM358 -> 74HC14 ->
// GPIO4 -> edge-timing ISR -> taskLTC.
//
// Timing status (see plan, part B): Free Run is frame-locked to the
// RTC-disciplined clock (arc_clock); the sub-frame phase loop (sample
// slipping) and the jam verification phase follow with ARC-Core.
#include "ltc_engine.h"
#include "ltc_esp32.h"
#include "AudioTools.h"
#include <esp_timer.h>
#include "arc_clock.h"
#include "pins.h"
#include "state.h"

namespace {

// ── state ────────────────────────────────────────────────────────────────
volatile SystemMode s_mode    = MODE_NONE;
volatile SystemMode s_reqMode = MODE_GEN_FREE_RUN;
volatile TcRate     s_rate    = RATE_25;
volatile bool       s_rateDirty = false;
volatile bool       s_running = false;
volatile bool       s_userBitsDirty = false;
volatile bool       s_muteHeld = false;   // keep DAC muted after a jam until cable is unplugged

portMUX_TYPE s_tcMux = portMUX_INITIALIZER_UNLOCKED;
Timecode     s_tc;                 // TC of the frame currently being output
uint32_t     s_anchorFrames = 0;   // frame count of s_tc
int64_t      s_anchorEspUs  = 0;   // esp_timer when that frame starts at J2

volatile uint32_t s_txUserBits = 0;
volatile uint32_t s_rxUserBits = 0;
volatile uint32_t s_jamOkCnt   = 0;
volatile float    s_rxFps      = 0.0f;

// Jam Record Run: frame anchor handed from taskLTC to applyMode().
volatile bool     s_jamAnchorPending = false;
volatile uint32_t s_jamAnchorFrames  = 0;
volatile int64_t  s_jamAnchorEspUs   = 0;

void (*s_statusCb)() = nullptr;
SemaphoreHandle_t s_audMtx = nullptr;

void notifyStatus() { if (s_statusCb) s_statusCb(); }

LTC_TV_STANDARD toStd(TcRate r) {
    switch (r) {
        case RATE_24:     return LTC_TV_FILM_24;
        case RATE_2997DF: return LTC_TV_525_DF;
        case RATE_30:     return LTC_TV_525_60;
        default:          return LTC_TV_625_50;
    }
}

int64_t frameDurationNs(TcRate r) {
    switch (r) {
        case RATE_24:     return 41666667LL;
        case RATE_2997DF: return 33366667LL;   // 1001/30000 s
        case RATE_30:     return 33333333LL;
        default:          return 40000000LL;
    }
}

uint32_t framesPerDay(TcRate r) {
    switch (r) {
        case RATE_24:     return 24u * 86400u;
        case RATE_2997DF: return 2589408u;
        case RATE_30:     return 30u * 86400u;
        default:          return 25u * 86400u;
    }
}

// Frame index of the day at a wall-clock µs-of-day (real time base).
uint32_t framesOfDayAt(int64_t usOfDay, TcRate r, uint32_t* remUs) {
    const int64_t ns = usOfDay * 1000LL;
    const int64_t dur = frameDurationNs(r);
    const uint32_t frames = (uint32_t)(ns / dur);
    if (remUs) *remUs = (uint32_t)((ns % dur) / 1000LL);
    return frames % framesPerDay(r);
}

int64_t usOfDayNow(int64_t espUs) {
    const int64_t epochUs = arcclock::nowUs() - (esp_timer_get_time() - espUs);
    return epochUs % 86400000000LL;
}

// EU daylight saving time from the (local) RTC date: last Sunday of March
// 02:00 to last Sunday of October 03:00.
bool isEuDst(const DateTime& t) {
    const int m = t.month();
    if (m < 3 || m > 10) return false;
    if (m > 3 && m < 10) return true;
    const int lastSunday = 31 - ((DateTime(t.year(), m, 31).dayOfTheWeek()) % 7);
    if (m == 3) return t.day() > lastSunday || (t.day() == lastSunday && t.hour() >= 2);
    return t.day() < lastSunday || (t.day() == lastSunday && t.hour() < 3);
}

uint32_t packUserBits(const LTCFrame& f) {
    return ((uint32_t)f.user1 << 28) | ((uint32_t)f.user2 << 24) | ((uint32_t)f.user3 << 20) |
           ((uint32_t)f.user4 << 16) | ((uint32_t)f.user5 << 12) | ((uint32_t)f.user6 << 8) |
           ((uint32_t)f.user7 << 4) | ((uint32_t)f.user8);
}

uint32_t reverseNibbles32(uint32_t v) {
    return ((v & 0xF) << 28) | (((v >> 4) & 0xF) << 24) | (((v >> 8) & 0xF) << 20) |
           (((v >> 12) & 0xF) << 16) | (((v >> 16) & 0xF) << 12) | (((v >> 20) & 0xF) << 8) |
           (((v >> 24) & 0xF) << 4) | ((v >> 28) & 0xF);
}

// Layout: [31:28] camera (A=0xA), [27:21] roll, [20:14] scene, [13:7] shot, [6:0] take.
// Must match ARC-Server videoProcessor.js (userbit matching).
uint32_t computeShotUserBits() {
    state::lock();
    const ShotInfo& s = state::shot();
    char cam = s.cam;
    if (cam < 'A' || cam > 'F') cam = 'A';
    const uint32_t camId = (uint32_t)(cam - 'A' + 10) & 0xF;
    const uint32_t bits = (camId << 28) | (((uint32_t)s.rollNum & 0x7F) << 21) |
                          (((uint32_t)s.scene & 0x7F) << 14) | (((uint32_t)s.shot.toInt() & 0x7F) << 7) |
                          ((uint32_t)s.take & 0x7F);
    state::unlock();
    return bits;
}

void applyUserBits(LTCEncoder* enc) {
    if (!enc) return;
    state::lock();
    const bool shotActive = state::shot().loaded;
    state::unlock();
    if (shotActive) {
        const uint32_t bits = computeShotUserBits();
        ltc_encoder_set_user_bits(enc, reverseNibbles32(bits));
        s_txUserBits = bits;
        return;
    }
    // No shot: date + timezone user bits (SMPTE 309M style).
    const DateTime now = arcclock::now();
    const uint8_t dd = now.day(), mm = now.month(), yy = now.year() % 100;
    const char* tz = isEuDst(now) ? "+0200" : "+0100";
    uint8_t tzCode = 0x00;
    for (int i = 0; _ltc_tz_table[i].code != 0xFF; i++) {
        if (strcmp(_ltc_tz_table[i].tz, tz) == 0) { tzCode = _ltc_tz_table[i].code; break; }
    }
    const uint32_t d = (uint32_t)(dd % 10) | ((uint32_t)(dd / 10) << 4) | ((uint32_t)(mm % 10) << 8) |
                       ((uint32_t)(mm / 10) << 12) | ((uint32_t)(yy % 10) << 16) | ((uint32_t)(yy / 10) << 20) |
                       ((uint32_t)(tzCode & 0x0F) << 24) | ((uint32_t)((tzCode >> 4) & 0x0F) << 28);
    ltc_encoder_set_user_bits(enc, d);
    LTCFrame frm;
    ltc_encoder_get_frame(enc, &frm);
    s_txUserBits = packUserBits(frm);
}

void setCurrent(const Timecode& tc, uint32_t frames, int64_t startEspUs) {
    portENTER_CRITICAL(&s_tcMux);
    s_tc = tc;
    s_anchorFrames = frames;
    s_anchorEspUs = startEspUs;
    portEXIT_CRITICAL(&s_tcMux);
}

int32_t outLatencyUs() {
    state::lock();
    const int32_t l = state::settings().ltcOutLatencyUs;
    state::unlock();
    return l > 0 ? l : 20000;   // default: ~I2S DMA depth, calibrate via loopback
}

// ── generator ────────────────────────────────────────────────────────────
class LTCGenerator : public audio_tools::SoundGenerator<int16_t> {
public:
    bool begin(audio_tools::AudioInfo cfg) {
        _sampleRate = cfg.sample_rate;
        if (!_encoder) _create(toStd(s_rate));
        _fillNextFrame();
        return audio_tools::SoundGenerator<int16_t>::begin(cfg);
    }

    int16_t readSample() override {
        if (!s_running) return 0;
        if (s_rateDirty || _pos >= _len) {
            if (s_rateDirty) { _recreate(); s_rateDirty = false; }
            _fillNextFrame();
        }
        return ((int16_t)(_buf[_pos++]) - 128) << 8;
    }

    // Free Run: jump to the TC that will be on air after the output latency.
    void alignToClock() {
        if (!_encoder) return;
        const int64_t esp = esp_timer_get_time() + outLatencyUs();
        const Timecode tc = ltc::framesToTc(framesOfDayAt(usOfDayNow(esp), s_rate, nullptr), s_rate);
        setTimecode(tc);
    }

    void setTimecode(const Timecode& tc) {
        if (!_encoder) return;
        SMPTETimecode st;
        ltc_encoder_get_timecode(_encoder, &st);
        st.hours = tc.h; st.mins = tc.m; st.secs = tc.s; st.frame = tc.f;
        ltc_encoder_set_timecode(_encoder, &st);
        _pos = _len;   // next sample starts the new frame
    }

    Timecode encoderTimecode() {
        Timecode tc;
        if (!_encoder) return tc;
        SMPTETimecode st;
        ltc_encoder_get_timecode(_encoder, &st);
        tc.h = st.hours; tc.m = st.mins; tc.s = st.secs; tc.f = st.frame;
        return tc;
    }

    void applyUserBitsNow() { applyUserBits(_encoder); }

private:
    LTCEncoder* _encoder = nullptr;
    ltcsnd_sample_t* _buf = nullptr;
    int _len = 0, _pos = 0;
    double _sampleRate = 48000.0;

    void _create(LTC_TV_STANDARD std) {
        _encoder = ltc_encoder_create(_sampleRate, ltc_standard_fps(std), std, 0);
        ltc_encoder_set_filter(_encoder, 0.0);
        ltc_encoder_set_volume(_encoder, -3.0);
        applyUserBits(_encoder);
    }

    void _recreate() {
        const Timecode keep = encoderTimecode();
        if (_encoder) ltc_encoder_free(_encoder);
        _encoder = nullptr;
        _create(toStd(s_rate));
        setTimecode(keep);
        _pos = _len = 0;
    }

    void _fillNextFrame() {
        if (!_encoder) return;
        if (s_userBitsDirty) { s_userBitsDirty = false; applyUserBits(_encoder); }

        // Free Run: keep the encoder on the clock. Hysteresis of 2 frames so
        // jitter of the copy loop does not cause flapping.
        if (s_mode == MODE_GEN_FREE_RUN) {
            const int64_t onAir = esp_timer_get_time() + outLatencyUs();
            const uint32_t ideal = framesOfDayAt(usOfDayNow(onAir), s_rate, nullptr);
            const uint32_t have = ltc::tcToFrames(encoderTimecode(), s_rate);
            const int32_t diff = (int32_t)ideal - (int32_t)have;
            if (diff >= 2 || diff <= -2) {
                const Timecode tc = ltc::framesToTc(ideal, s_rate);
                SMPTETimecode st;
                ltc_encoder_get_timecode(_encoder, &st);
                st.hours = tc.h; st.mins = tc.m; st.secs = tc.s; st.frame = tc.f;
                ltc_encoder_set_timecode(_encoder, &st);
            }
        }

        ltc_encoder_encode_frame(_encoder);
        _len = ltc_encoder_get_bufferptr(_encoder, &_buf, 1);
        _pos = 0;

        const Timecode tc = encoderTimecode();
        setCurrent(tc, ltc::tcToFrames(tc, s_rate), esp_timer_get_time() + outLatencyUs());
        ltc_encoder_inc_timecode(_encoder);
    }
};

// Mono LTC -> stereo I2S frame. The slate has no playback output (J3 is not
// fitted), so LTC goes to both DAC channels and reaches J2 regardless of the
// channel mapping of the PCM5102 module.
class LTCStereoOutput : public audio_tools::AudioOutput {
public:
    explicit LTCStereoOutput(Stream& sink) : _sink(sink) {}
    bool begin() override { return true; }
    void setAudioInfo(audio_tools::AudioInfo info) override { _inInfo = info; }

    size_t write(const uint8_t* data, size_t len) override {
        if (len == 0 || _inInfo.bits_per_sample != 16) return len;
        const int16_t* in = (const int16_t*)data;
        const int n = len / 2;
        int16_t out[256];
        int pos = 0;
        for (int i = 0; i < n; i++) {
            out[pos++] = in[i];
            out[pos++] = in[i];
            if (pos >= 256) { _flush(out, pos); pos = 0; }
        }
        if (pos > 0) _flush(out, pos);
        return len;
    }

private:
    Stream& _sink;
    audio_tools::AudioInfo _inInfo = {48000, 1, 16};

    void _flush(int16_t* out, int samples) {
        size_t written = 0;
        const size_t total = (size_t)samples * 2;
        while (written < total) {
            const size_t w = _sink.write((uint8_t*)out + written, total - written);
            if (w > 0) written += w; else vTaskDelay(1);
        }
    }
};

audio_tools::I2SStream i2s;
audio_tools::AudioInfo encInfo(48000, 1, 16);   // mono generator -> stereo in LTCStereoOutput
LTCGenerator ltcGen;
audio_tools::GeneratedSoundStream<int16_t> ltcStream(ltcGen);
LTCStereoOutput ltcStereo(i2s);
audio_tools::StreamCopy encCopier(ltcStereo, ltcStream);

// ── decoder ──────────────────────────────────────────────────────────────
struct LTCFrameDec {
    uint8_t tc[10];
    bool    isReverse;
    int64_t syncEspUs;   // esp_timer at the end of the sync word
};
QueueHandle_t s_ltcQueue = nullptr;

uint8_t flip8(uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

// Edge-timing biphase-mark decoder (init -> calibrate half-bit -> read bits).
void IRAM_ATTR isrLTC() {
    const uint32_t now = micros();
    static uint32_t lastEdge = 0;
    const uint32_t cellTime = now - lastEdge;
    if (cellTime < 100) return;   // noise filter
    lastEdge = now;

    static uint8_t shiftReg[10];
    static uint8_t st = 0, counter = 0, zCount = 0;
    static bool eatEdge = false;
    static uint32_t accum = 0, cellOne = 0, cellMin = 0, cellDet = 0, cellMax = 0;

    if (st == 0) {
        for (int i = 0; i < 10; i++) shiftReg[i] = 0;
        eatEdge = false; counter = 0; zCount = 0; accum = 0; cellOne = 0;
        st = 1;
    } else if (st == 1) {
        if (++counter > 40) {
            if (zCount > 1) cellOne = (accum / (zCount - 1)) / 2;
            else { st = 0; return; }
            cellMin = cellOne / 2; cellDet = cellMin * 3; cellMax = cellMin * 5;
            st = 2;
        } else if (cellTime >= cellOne) {
            if (++zCount > 1) accum += cellTime;
            else cellOne = (cellTime * 2) / 3;
        }
    } else {
        if (cellTime < cellMin || cellTime > cellMax) { st = 0; return; }
        uint8_t newBit = 0;
        if (cellTime > cellDet) {
            if (eatEdge) { st = 0; return; }
        } else {
            if (!eatEdge) { eatEdge = true; return; }
            eatEdge = false; newBit = 0x80;
        }
        for (int idx = 9; idx > 0; idx--) shiftReg[idx] = (shiftReg[idx] >> 1) | ((shiftReg[idx - 1] & 1) << 7);
        shiftReg[0] = (shiftReg[0] >> 1) | newBit;

        const uint16_t syncFwd = ((uint16_t)shiftReg[0] << 8 | shiftReg[1]);
        const uint16_t syncRvs = ((uint16_t)shiftReg[8] << 8 | shiftReg[9]);
        bool isRvs = false, found = false;
        if (syncFwd == 0xBFFC) { found = true; isRvs = false; }
        else if (syncRvs == 0x3FFD) { found = true; isRvs = true; }
        if (found) {
            LTCFrameDec frame;
            for (int i = 0; i < 10; ++i) frame.tc[i] = shiftReg[i];
            frame.isReverse = isRvs;
            frame.syncEspUs = esp_timer_get_time();
            BaseType_t woken = pdFALSE;
            xQueueSendFromISR(s_ltcQueue, &frame, &woken);
            for (int i = 0; i < 10; ++i) shiftReg[i] = 0;
            if (woken) portYIELD_FROM_ISR();
        }
        eatEdge = false;
    }
}

bool isDecodeMode(SystemMode m) {
    return m == MODE_SLAVE || m == MODE_JAM_SYNC_FREE_RUN || m == MODE_JAM_SYNC_RECORD_RUN;
}

void handleJamLock(uint32_t frameAfter, int64_t frameStartEspUs, TcRate detected) {
    if (detected != s_rate) { s_rate = detected; s_rateDirty = true; }
    if (s_mode == MODE_JAM_SYNC_FREE_RUN) {
        // The master runs Free Run, so its TC is its time of day. Set our RTC
        // so that the decoded frame starts exactly at frameStartEspUs.
        const int64_t todUs = (int64_t)frameAfter * frameDurationNs(detected) / 1000LL;
        const int64_t localNow = arcclock::nowUs();
        const int64_t dayStart = localNow - (localNow % 86400000000LL);
        int64_t targetAtEdge = dayStart + todUs;
        // Midnight wrap: master just after midnight, we just before (or vice versa).
        const int64_t diff = targetAtEdge - localNow;
        if (diff > 43200000000LL) targetAtEdge -= 86400000000LL;
        if (diff < -43200000000LL) targetAtEdge += 86400000000LL;
        const int64_t targetNow = targetAtEdge + (esp_timer_get_time() - frameStartEspUs);
        Serial.printf("[JAM] Lock: setze RTC (Abweichung vorher %lld ms)\n",
                      (long long)((targetNow - arcclock::nowUs()) / 1000));
        arcclock::setTimeUs(targetNow);
        s_muteHeld = true;
        s_reqMode = MODE_GEN_FREE_RUN;
    } else {
        s_jamAnchorFrames = frameAfter;
        s_jamAnchorEspUs = frameStartEspUs;
        s_jamAnchorPending = true;
        s_muteHeld = true;
        s_reqMode = MODE_GEN_RECORD_RUN;
    }
    Serial.println("[JAM] Jam Sync erfolgreich – Kabel abziehen und bestätigen (Encoder drücken / 'u')");
}

void taskLTC(void*) {
    LTCFrameDec frame;
    int64_t lastSyncUs = 0;
    uint32_t windowCnt = 0;
    uint8_t maxFrame = 0;
    uint8_t estFps = 0;            // 0 = not yet estimated
    uint32_t lastFrames = 0;
    bool haveLast = false;

    for (;;) {
        if (!xQueueReceive(s_ltcQueue, &frame, pdMS_TO_TICKS(100))) continue;
        const int64_t period = frame.syncEspUs - lastSyncUs;
        lastSyncUs = frame.syncEspUs;

        uint8_t tc[10];
        if (frame.isReverse) { for (int i = 0; i < 10; i++) tc[i] = flip8(frame.tc[9 - i]); }
        else                 { for (int i = 0; i < 10; i++) tc[i] = frame.tc[i]; }

        Timecode t;
        t.h = ((tc[2] & 0x03) * 10) + (tc[3] & 0x0F);
        t.m = ((tc[4] & 0x07) * 10) + (tc[5] & 0x0F);
        t.s = ((tc[6] & 0x07) * 10) + (tc[7] & 0x0F);
        t.f = ((tc[8] & 0x03) * 10) + (tc[9] & 0x0F);
        const bool df = (tc[8] & 0x04) != 0;

        if (t.f > maxFrame) maxFrame = t.f;
        if (++windowCnt >= 32) {
            estFps = df ? 30 : (uint8_t)(maxFrame + 1);
            maxFrame = 0; windowCnt = 0;
        }
        TcRate detected = s_rate;
        if (df) detected = RATE_2997DF;
        else if (estFps == 24) detected = RATE_24;
        else if (estFps == 25) detected = RATE_25;
        else if (estFps == 30) detected = RATE_30;

        s_rxUserBits = ((uint32_t)(tc[2] >> 4) & 0xF) | (((uint32_t)(tc[3] >> 4) & 0xF) << 4) |
                       (((uint32_t)(tc[4] >> 4) & 0xF) << 8) | (((uint32_t)(tc[5] >> 4) & 0xF) << 12) |
                       (((uint32_t)(tc[6] >> 4) & 0xF) << 16) | (((uint32_t)(tc[7] >> 4) & 0xF) << 20) |
                       (((uint32_t)(tc[8] >> 4) & 0xF) << 24) | (((uint32_t)(tc[9] >> 4) & 0xF) << 28);

        if (!isDecodeMode(s_mode)) { haveLast = false; continue; }

        // The sync word ends the frame: the *next* frame starts right now.
        const uint32_t frames = ltc::tcToFrames(t, detected);
        const uint32_t next = (frames + 1) % framesPerDay(detected);
        const Timecode nextTc = ltc::framesToTc(next, detected);
        setCurrent(nextTc, next, frame.syncEspUs);
        s_rxFps = df ? 29.97f : (float)(estFps ? estFps : 0);

        // Lock criteria: continuous TC and a plausible frame period.
        const int64_t nominalUs = frameDurationNs(detected) / 1000LL;
        const bool periodOk = period > nominalUs * 9 / 10 && period < nominalUs * 11 / 10;
        const bool continuous = haveLast && frames == (lastFrames + 1) % framesPerDay(detected);
        lastFrames = frames;
        haveLast = true;
        s_jamOkCnt = (continuous && periodOk && estFps) ? s_jamOkCnt + 1 : 0;

        if ((s_mode == MODE_JAM_SYNC_FREE_RUN || s_mode == MODE_JAM_SYNC_RECORD_RUN) && s_jamOkCnt >= 75) {
            s_jamOkCnt = 0;
            handleJamLock(next, frame.syncEspUs, detected);
            xQueueReset(s_ltcQueue);
            haveLast = false;
        }
    }
}

void taskEncode(void*) {
    for (;;) {
        if ((s_mode == MODE_GEN_FREE_RUN || s_mode == MODE_GEN_RECORD_RUN) && s_running) {
            xSemaphoreTake(s_audMtx, portMAX_DELAY);
            encCopier.copy();
            xSemaphoreGive(s_audMtx);
            vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void setMute(bool mute) { digitalWrite(PIN_PCM_XSMT, mute ? LOW : HIGH); }

void applyMode(SystemMode newMode) {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));   // let the encoder task drain

    xSemaphoreTake(s_audMtx, portMAX_DELAY);
    if (s_mode == MODE_GEN_FREE_RUN || s_mode == MODE_GEN_RECORD_RUN) i2s.end();
    s_mode = newMode;
    s_jamOkCnt = 0;

    if (newMode == MODE_GEN_FREE_RUN || newMode == MODE_GEN_RECORD_RUN) {
        auto cfg = i2s.defaultConfig(audio_tools::TX_MODE);
        cfg.pin_bck = PIN_DAC_BCK; cfg.pin_ws = PIN_DAC_LRCK; cfg.pin_data = PIN_DAC_DIN;
        cfg.sample_rate = 48000; cfg.channels = 2; cfg.bits_per_sample = 16;
        i2s.begin(cfg);
        if (newMode == MODE_GEN_FREE_RUN) {
            ltcGen.alignToClock();
            s_running = true;
        } else if (s_jamAnchorPending) {
            // Continue the jammed TC: frames elapsed since the jam edge.
            s_jamAnchorPending = false;
            const int64_t elapsedNs = (esp_timer_get_time() + outLatencyUs() - s_jamAnchorEspUs) * 1000LL;
            const uint32_t f = (s_jamAnchorFrames + (uint32_t)(elapsedNs / frameDurationNs(s_rate))) %
                               framesPerDay(s_rate);
            ltcGen.setTimecode(ltc::framesToTc(f, s_rate));
            s_running = true;
        }
    }
    xSemaphoreGive(s_audMtx);

    setMute(isDecodeMode(newMode) || s_muteHeld);
    notifyStatus();
}

}  // namespace

// ── public API ───────────────────────────────────────────────────────────
namespace ltc {

void begin() {
    s_audMtx = xSemaphoreCreateMutex();
    pinMode(PIN_PCM_XSMT, OUTPUT);
    setMute(true);

    state::lock();
    TcRate r;
    if (rateFromFps(state::shot().fps, r)) s_rate = r;
    state::unlock();

    s_ltcQueue = xQueueCreate(20, sizeof(LTCFrameDec));
    ltcGen.begin(encInfo);
    xTaskCreatePinnedToCore(taskLTC, "LTCDEC", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(taskEncode, "LTCENC", 8192, nullptr, 2, nullptr, 1);
    pinMode(PIN_LTC_IN, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_LTC_IN), isrLTC, CHANGE);
    s_reqMode = MODE_GEN_FREE_RUN;
}

void loop() {
    if (s_reqMode != MODE_NONE) {
        const SystemMode m = s_reqMode;
        s_reqMode = MODE_NONE;
        applyMode(m);
    }
}

void requestMode(SystemMode m) { s_reqMode = m; }
SystemMode mode() { return s_mode; }

const char* modeKey() {
    switch (s_mode) {
        case MODE_GEN_FREE_RUN:        return "gen_free";
        case MODE_GEN_RECORD_RUN:      return "gen_rec";
        case MODE_SLAVE:               return "slave";
        case MODE_JAM_SYNC_FREE_RUN:   return "jam_free";
        case MODE_JAM_SYNC_RECORD_RUN: return "jam_rec";
        default:                       return "none";
    }
}

bool modeFromKey(const String& key, SystemMode& out) {
    if (key == "gen_free") out = MODE_GEN_FREE_RUN;
    else if (key == "gen_rec") out = MODE_GEN_RECORD_RUN;
    else if (key == "slave") out = MODE_SLAVE;
    else if (key == "jam_free") out = MODE_JAM_SYNC_FREE_RUN;
    else if (key == "jam_rec") out = MODE_JAM_SYNC_RECORD_RUN;
    else return false;
    return true;
}

void setRate(TcRate r) {
    if (r == s_rate) return;
    s_rate = r;
    s_rateDirty = true;
    state::lock();
    state::shot().fps = fps();
    state::touch();
    state::unlock();
    notifyStatus();
}

TcRate rate() { return s_rate; }

const char* rateKey() {
    switch (s_rate) {
        case RATE_24:     return "24";
        case RATE_2997DF: return "29.97DF";
        case RATE_30:     return "30";
        default:          return "25";
    }
}

bool rateFromKey(const String& key, TcRate& out) {
    if (key == "24") out = RATE_24;
    else if (key == "25") out = RATE_25;
    else if (key == "29.97DF" || key == "29.97") out = RATE_2997DF;
    else if (key == "30") out = RATE_30;
    else return false;
    return true;
}

bool rateFromFps(float f, TcRate& out) {
    if (f <= 0.0f) return false;
    if (fabsf(f - 24.0f) < 0.01f) out = RATE_24;
    else if (fabsf(f - 25.0f) < 0.01f) out = RATE_25;
    else if (fabsf(f - 29.97f) < 0.01f) out = RATE_2997DF;
    else if (fabsf(f - 30.0f) < 0.01f) out = RATE_30;
    else return false;
    return true;
}

float fps() { return s_rate == RATE_2997DF ? 29.97f : (float)fpsInt(); }

uint8_t fpsInt() {
    switch (s_rate) {
        case RATE_24: return 24;
        case RATE_25: return 25;
        default:      return 30;
    }
}

void start() {
    if (s_mode != MODE_GEN_RECORD_RUN && s_mode != MODE_GEN_FREE_RUN) return;
    s_running = true;
    notifyStatus();
}

void stop() {
    if (s_mode != MODE_GEN_RECORD_RUN) return;   // Free Run never stops
    s_running = false;
    notifyStatus();
}

bool running() { return s_running; }

void setTimecode(const Timecode& tc) {
    xSemaphoreTake(s_audMtx, portMAX_DELAY);
    ltcGen.setTimecode(tc);
    xSemaphoreGive(s_audMtx);
    setCurrent(tc, tcToFrames(tc, s_rate), esp_timer_get_time());
    notifyStatus();
}

void resetTimecode() { setTimecode(Timecode()); }

void releaseMuteHold() {
    s_muteHeld = false;
    setMute(isDecodeMode(s_mode));
}

bool muteHeld() { return s_muteHeld; }

Timecode current() {
    portENTER_CRITICAL(&s_tcMux);
    const Timecode tc = s_tc;
    portEXIT_CRITICAL(&s_tcMux);
    return tc;
}

Timecode timecodeAt(int64_t espUs, uint32_t& subframeUs) {
    if (s_mode == MODE_GEN_FREE_RUN) {
        uint32_t rem = 0;
        const uint32_t f = framesOfDayAt(usOfDayNow(espUs), s_rate, &rem);
        subframeUs = rem;
        return framesToTc(f, s_rate);
    }
    portENTER_CRITICAL(&s_tcMux);
    const uint32_t frames = s_anchorFrames;
    const int64_t anchor = s_anchorEspUs;
    portEXIT_CRITICAL(&s_tcMux);
    const bool advancing = s_running || isDecodeMode(s_mode);
    int64_t ns = (espUs - anchor) * 1000LL;
    if (!advancing || ns < 0) ns = 0;
    const int64_t dur = frameDurationNs(s_rate);
    subframeUs = (uint32_t)((ns % dur) / 1000LL);
    return framesToTc((frames + (uint32_t)(ns / dur)) % framesPerDay(s_rate), s_rate);
}

void refreshUserBits() { s_userBitsDirty = true; }
uint32_t txUserBits() { return s_txUserBits; }
uint32_t rxUserBits() { return s_rxUserBits; }
uint32_t jamOkCount() { return s_jamOkCnt; }
float rxFps() { return s_rxFps; }
void onStatusChange(void (*cb)()) { s_statusCb = cb; }

uint32_t tcToFrames(const Timecode& tc, TcRate r) {
    if (r == RATE_2997DF) {
        const uint32_t totalMinutes = 60u * tc.h + tc.m;
        return (uint32_t)((tc.h * 3600u + tc.m * 60u + tc.s) * 30u + tc.f) - 2u * (totalMinutes - totalMinutes / 10u);
    }
    const uint32_t f = (r == RATE_24) ? 24u : (r == RATE_25) ? 25u : 30u;
    return (tc.h * 3600u + tc.m * 60u + tc.s) * f + tc.f;
}

Timecode framesToTc(uint32_t frames, TcRate r) {
    Timecode tc;
    uint32_t base = 30;
    if (r == RATE_2997DF) {
        const uint32_t d = frames / 17982u;
        const uint32_t m = frames % 17982u;
        frames += 18u * d + (m >= 2u ? 2u * ((m - 2u) / 1798u) : 0u);
    } else {
        base = (r == RATE_24) ? 24u : (r == RATE_25) ? 25u : 30u;
    }
    tc.f = frames % base;
    tc.s = (frames / base) % 60u;
    tc.m = (frames / (base * 60u)) % 60u;
    tc.h = (frames / (base * 3600u)) % 24u;
    return tc;
}

String tcToString(const Timecode& tc) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u:%02u", tc.h, tc.m, tc.s, tc.f);
    return String(buf);
}

bool parseTc(const String& s, Timecode& out) {
    if (s.length() < 11) return false;
    out.h = (uint8_t)s.substring(0, 2).toInt();
    out.m = (uint8_t)s.substring(3, 5).toInt();
    out.s = (uint8_t)s.substring(6, 8).toInt();
    out.f = (uint8_t)s.substring(9, 11).toInt();
    return true;
}

}  // namespace ltc
