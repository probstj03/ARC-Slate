#pragma once
// ARC-Clock settings in NVS (namespace "clock"). Credentials never live in the repo:
// they are entered over the serial console (`nc user ...`) or pre-filled from the
// git-ignored src/clock/secrets.h on first boot.
#include <Arduino.h>

struct ClockConfig {
    // Nextcloud (CalDAV + WebDAV)
    String ncUrl;            // https://cloud.example.org   (no trailing slash)
    String ncUser;           // user ID (in the WebDAV paths, e.g. Admin)
    String ncLogin;          // login name for the app password (e.g. the e-mail address); empty = ncUser
    const String& loginName() const { return ncLogin.length() ? ncLogin : ncUser; }
    String ncPass;           // app password
    String calendars;        // CalDAV paths, one per line: /remote.php/dav/calendars/<user>/<name>/
    String todoPath;         // path of the md file inside the user's files: Notizen/ToDo.md
    String todoHeading;      // only tasks below this heading (empty = whole file)
    bool tlsInsecure = false;  // skip certificate validation (self-signed LAN Nextcloud)

    // Weather / place
    float lat = 52.52f;
    float lon = 13.405f;
    String place = "Berlin";
    bool locAuto = true;     // place from the public IP (ip-api.com) until set with `loc LAT LON`

    // E-ink: one full refresh per day at this time (local)
    uint8_t epdHour = 5;
    uint8_t epdMinute = 30;


    // LED matrices
    uint8_t brightnessDay = 1;     // 0..15 (low on purpose: 16 modules pulse the supply)
    uint8_t brightnessNight = 1;   // between sunset and sunrise
    uint16_t idleOffMin = 0;       // matrices off after n minutes with the lid open (0 = never)
    uint8_t reminderMin = 10;      // announce events n minutes before they start (0 = off)
    bool reminderFlash = true;     // 3 short flashes (flash LED) with the reminder (never at night)

    // refresh intervals (minutes)
    uint16_t calendarMin = 15;
    uint16_t todoMin = 15;
    uint16_t weatherMin = 30;

    // measured by the slate firmware as well: which DS3231 SQW edge marks the second
    bool sqwKnown = false;
    bool sqwFalling = true;
};

namespace clockcfg {

void begin();                       // load from NVS (+ defaults from secrets.h on first boot)
ClockConfig& get();                 // not thread-safe: copy what you need (see copy())
ClockConfig copy();
void save();
void print(Stream& out);            // password masked

// Splits `calendars` into paths (at most 6).
int calendarList(const ClockConfig& c, String out[], int max);

}  // namespace clockcfg
