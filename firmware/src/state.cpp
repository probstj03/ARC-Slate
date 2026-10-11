#include "state.h"
#include <Preferences.h>

namespace {
SemaphoreHandle_t s_mtx = nullptr;
ShotInfo s_shot;
Settings s_settings;
volatile uint32_t s_rev = 1;
volatile bool s_dirty = false;
uint32_t s_dirtySinceMs = 0;
Preferences s_prefs;

void load() {
    s_prefs.begin("slate", true);
    s_shot.code        = s_prefs.getString("code", "-");
    s_shot.production  = s_prefs.getString("prod", "");
    s_shot.director    = s_prefs.getString("dir", "");
    s_shot.camera      = s_prefs.getString("camname", "");
    s_shot.environment = s_prefs.getString("env", "");
    s_shot.daynight    = s_prefs.getString("dn", "");
    s_shot.scene       = s_prefs.getInt("scene", 0);
    s_shot.shot        = s_prefs.getString("shot", "0");
    s_shot.take        = s_prefs.getInt("take", 0);
    s_shot.cam         = (char)s_prefs.getUChar("cam", 'A');
    s_shot.rollNum     = s_prefs.getInt("roll", 1);
    s_shot.fps         = s_prefs.getFloat("fps", 25.0f);
    s_shot.mos         = s_prefs.getBool("mos", false);
    s_shot.loaded      = s_prefs.getBool("loaded", false);
    s_settings.brightness      = s_prefs.getUChar("bright", 8);
    s_settings.sqwEdgeKnown    = s_prefs.getBool("sqwKnown", false);
    s_settings.sqwFalling      = s_prefs.getBool("sqwFall", true);
    s_settings.flashLatencyUs  = s_prefs.getInt("flashLat", 0);
    s_settings.ltcOutLatencyUs = s_prefs.getInt("ltcLat", 0);
    s_settings.encReverse      = s_prefs.getBool("encRev", false);
    s_prefs.end();
}

void save() {
    s_prefs.begin("slate", false);
    s_prefs.putString("code", s_shot.code);
    s_prefs.putString("prod", s_shot.production);
    s_prefs.putString("dir", s_shot.director);
    s_prefs.putString("camname", s_shot.camera);
    s_prefs.putString("env", s_shot.environment);
    s_prefs.putString("dn", s_shot.daynight);
    s_prefs.putInt("scene", s_shot.scene);
    s_prefs.putString("shot", s_shot.shot);
    s_prefs.putInt("take", s_shot.take);
    s_prefs.putUChar("cam", (uint8_t)s_shot.cam);
    s_prefs.putInt("roll", s_shot.rollNum);
    s_prefs.putFloat("fps", s_shot.fps);
    s_prefs.putBool("mos", s_shot.mos);
    s_prefs.putBool("loaded", s_shot.loaded);
    s_prefs.putUChar("bright", s_settings.brightness);
    s_prefs.putBool("sqwKnown", s_settings.sqwEdgeKnown);
    s_prefs.putBool("sqwFall", s_settings.sqwFalling);
    s_prefs.putInt("flashLat", s_settings.flashLatencyUs);
    s_prefs.putInt("ltcLat", s_settings.ltcOutLatencyUs);
    s_prefs.putBool("encRev", s_settings.encReverse);
    s_prefs.end();
}
}  // namespace

namespace state {

void begin() {
    s_mtx = xSemaphoreCreateRecursiveMutex();
    load();
}

void lock() { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(s_mtx); }
ShotInfo& shot() { return s_shot; }
Settings& settings() { return s_settings; }

void touch() {
    s_rev++;
    if (!s_dirty) s_dirtySinceMs = millis();
    s_dirty = true;
}

uint32_t revision() { return s_rev; }

void saveIfDirty() {
    // Debounce NVS writes: encoder turns change the take many times per second.
    if (!s_dirty || millis() - s_dirtySinceMs < 1500) return;
    lock();
    s_dirty = false;
    save();
    unlock();
}

String rollString() {
    lock();
    char buf[8];
    snprintf(buf, sizeof(buf), "%c%03d", s_shot.cam, s_shot.rollNum);
    unlock();
    return String(buf);
}

void setRollFromString(ShotInfo& s, const String& roll) {
    for (unsigned i = 0; i < roll.length(); i++) {
        char c = (char)toupper((unsigned char)roll[i]);
        if (c >= 'A' && c <= 'Z') { s.cam = c; break; }
    }
    for (unsigned i = 0; i < roll.length(); i++) {
        if (isdigit((unsigned char)roll[i])) {
            s.rollNum = roll.substring(i).toInt();
            break;
        }
    }
    if (s.rollNum < 1) s.rollNum = 1;
    if (s.rollNum > 127) s.rollNum = 127;   // 7 bits in the LTC user bits
}

}  // namespace state
