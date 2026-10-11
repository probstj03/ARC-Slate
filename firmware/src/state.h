#pragma once
// Shared slate state (shot data + settings), persisted in NVS.
// All access to shot()/settings() must happen between lock() and unlock().
// Every change bumps revision(), which the display tasks poll.
#include <Arduino.h>

struct ShotInfo {
    String code;          // project code (QR code / file names)
    String production;
    String director;
    String camera;        // camera operator / DOP name for the e-ink
    String environment;   // INT / EXT
    String daynight;      // DAY / NIGHT
    int    scene   = 0;
    String shot    = "0";
    int    take    = 0;
    char   cam     = 'A'; // camera letter (roll prefix)
    int    rollNum = 1;
    float  fps     = 25.0f;
    bool   mos     = false;
    String starttime;     // shot start TC (from ARC-Studio)
    String endtime;
    float  speed   = 1.0f;
    bool   loaded  = false; // true once shot data was received or restored
};

struct Settings {
    uint8_t brightness      = 8;     // MAX7219 intensity 0..15
    bool    sqwEdgeKnown    = false; // DS3231 second edge auto-detected?
    bool    sqwFalling      = true;  // true: falling SQW edge marks the second change
    int32_t flashLatencyUs  = 0;     // delay hall edge -> LED fully on (calibrate)
    bool    encReverse      = false; // swap encoder direction
};

namespace state {

void begin();
void lock();
void unlock();
ShotInfo& shot();        // lock() required
Settings& settings();    // lock() required

// Marks the state as changed: bumps the revision and schedules an NVS save.
void touch();
uint32_t revision();

// Writes pending changes to NVS. Call from loop() only.
void saveIfDirty();

// "A001" style roll name for the current cam/rollNum (takes the lock itself).
String rollString();
// Parses "A001" into cam/rollNum (lock() required).
void setRollFromString(ShotInfo& s, const String& roll);

}  // namespace state
