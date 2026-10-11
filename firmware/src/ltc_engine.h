#pragma once
// LTC generator (I2S -> PCM5102 -> J2), LTC decoder (J2 -> GPIO4) and Jam Sync.
#include <Arduino.h>

enum SystemMode {
    MODE_GEN_FREE_RUN,         // generator, TC = time of day from the RTC
    MODE_GEN_RECORD_RUN,       // generator, TC only advances while running
    MODE_SLAVE,                // decoder only, display follows incoming LTC
    MODE_JAM_SYNC_FREE_RUN,    // lock onto incoming LTC, set RTC, then Free Run
    MODE_JAM_SYNC_RECORD_RUN,  // lock onto incoming LTC, then Record Run from there
    MODE_NONE
};

enum TcRate { RATE_24, RATE_25, RATE_2997DF, RATE_30 };

struct Timecode {
    uint8_t h = 0, m = 0, s = 0, f = 0;
};

namespace ltc {

void begin();
void loop();                       // applies pending mode changes (call from loop())

void requestMode(SystemMode m);
SystemMode mode();
const char* modeKey();             // "gen_free", "gen_rec", "slave", "jam_free", "jam_rec"
bool modeFromKey(const String& key, SystemMode& out);

void setRate(TcRate r);
TcRate rate();
const char* rateKey();             // "24", "25", "29.97DF", "30"
bool rateFromKey(const String& key, TcRate& out);
bool rateFromFps(float fps, TcRate& out);
float fps();                       // nominal (29.97 for DF)
uint8_t fpsInt();                  // frames per second label (30 for DF)

void start();
void stop();
bool running();
void setTimecode(const Timecode& tc);
void resetTimecode();

// After a jam the DAC stays muted (both devices would drive J2) until the
// cable is unplugged and this is confirmed.
bool muteHeld();
void releaseMuteHold();

Timecode current();                // TC currently shown/output
// TC (plus position inside the frame) at an esp_timer instant; used for events.
Timecode timecodeAt(int64_t espUs, uint32_t& subframeUs);

void refreshUserBits();            // shot data changed
uint32_t txUserBits();
uint32_t rxUserBits();
uint32_t jamOkCount();
float rxFps();

void onStatusChange(void (*cb)());

// TC <-> frame count helpers (drop-frame aware)
uint32_t tcToFrames(const Timecode& tc, TcRate r);
Timecode framesToTc(uint32_t frames, TcRate r);
String tcToString(const Timecode& tc);
bool parseTc(const String& s, Timecode& out);

}  // namespace ltc
