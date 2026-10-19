#include "clock_config.h"
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

ClockConfig s_cfg;
SemaphoreHandle_t s_mtx = nullptr;
Preferences s_prefs;

}  // namespace

namespace clockcfg {

void begin() {
    s_mtx = xSemaphoreCreateMutex();
    s_prefs.begin("clock", true);
    const bool first = !s_prefs.isKey("init");
    s_cfg.ncUrl = s_prefs.getString("ncUrl", "");
    s_cfg.ncUser = s_prefs.getString("ncUser", "");
    s_cfg.ncLogin = s_prefs.getString("ncLogin", "");
    s_cfg.ncPass = s_prefs.getString("ncPass", "");
    s_cfg.calendars = s_prefs.getString("cals", "");
    s_cfg.todoPath = s_prefs.getString("todo", "");
    s_cfg.todoHeading = s_prefs.getString("todoHead", "");
    s_cfg.tlsInsecure = s_prefs.getBool("tlsInsec", false);
    s_cfg.lat = s_prefs.getFloat("lat", s_cfg.lat);
    s_cfg.lon = s_prefs.getFloat("lon", s_cfg.lon);
    s_cfg.place = s_prefs.getString("place", s_cfg.place);
    s_cfg.locAuto = s_prefs.getBool("locAuto", true);
    s_cfg.epdHour = s_prefs.getUChar("epdH", s_cfg.epdHour);
    s_cfg.epdMinute = s_prefs.getUChar("epdM", s_cfg.epdMinute);
    s_cfg.reminderFlash = s_prefs.getBool("remFlash", true);
    s_cfg.brightnessDay = s_prefs.getUChar("brD1", s_cfg.brightnessDay);
    s_cfg.brightnessNight = s_prefs.getUChar("brN1", s_cfg.brightnessNight);
    s_cfg.idleOffMin = s_prefs.getUShort("idleOff", 0);
    s_cfg.reminderMin = s_prefs.getUChar("remind", s_cfg.reminderMin);
    s_cfg.calendarMin = s_prefs.getUShort("calMin", s_cfg.calendarMin);
    s_cfg.todoMin = s_prefs.getUShort("todoMin", s_cfg.todoMin);
    s_cfg.weatherMin = s_prefs.getUShort("wxMin", s_cfg.weatherMin);
    // The slate firmware measured the SQW edge and keeps it in its own namespace.
    s_cfg.sqwKnown = s_prefs.getBool("sqwKnown", false);
    s_cfg.sqwFalling = s_prefs.getBool("sqwFall", true);
    s_prefs.end();

    if (!s_cfg.sqwKnown) {
        Preferences slate;
        if (slate.begin("slate", true)) {
            if (slate.getBool("sqwKnown", false)) {
                s_cfg.sqwKnown = true;
                s_cfg.sqwFalling = slate.getBool("sqwFall", true);
            }
            slate.end();
        }
    }

#if defined(CLOCK_NC_URL)
    if (first) {          // pre-fill from secrets.h once; later edits happen over serial
        s_cfg.ncUrl = CLOCK_NC_URL;
        s_cfg.ncUser = CLOCK_NC_USER;
        s_cfg.ncPass = CLOCK_NC_PASS;
        s_cfg.calendars = CLOCK_NC_CALENDARS;
        s_cfg.todoPath = CLOCK_NC_TODO;
    }
#endif
#if defined(CLOCK_LAT)
    if (first) {
        s_cfg.lat = CLOCK_LAT;
        s_cfg.lon = CLOCK_LON;
        s_cfg.place = CLOCK_PLACE;
        s_cfg.locAuto = false;
    }
#endif
    if (first) save();
}

ClockConfig& get() { return s_cfg; }

ClockConfig copy() {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ClockConfig c = s_cfg;
    xSemaphoreGive(s_mtx);
    return c;
}

void save() {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_prefs.begin("clock", false);
    s_prefs.putBool("init", true);
    s_prefs.putString("ncUrl", s_cfg.ncUrl);
    s_prefs.putString("ncUser", s_cfg.ncUser);
    s_prefs.putString("ncLogin", s_cfg.ncLogin);
    s_prefs.putString("ncPass", s_cfg.ncPass);
    s_prefs.putString("cals", s_cfg.calendars);
    s_prefs.putString("todo", s_cfg.todoPath);
    s_prefs.putString("todoHead", s_cfg.todoHeading);
    s_prefs.putBool("tlsInsec", s_cfg.tlsInsecure);
    s_prefs.putFloat("lat", s_cfg.lat);
    s_prefs.putFloat("lon", s_cfg.lon);
    s_prefs.putString("place", s_cfg.place);
    s_prefs.putBool("locAuto", s_cfg.locAuto);
    s_prefs.putUChar("epdH", s_cfg.epdHour);
    s_prefs.putUChar("epdM", s_cfg.epdMinute);
    s_prefs.putBool("remFlash", s_cfg.reminderFlash);
    s_prefs.putUChar("brD1", s_cfg.brightnessDay);
    s_prefs.putUChar("brN1", s_cfg.brightnessNight);
    s_prefs.putUShort("idleOff", s_cfg.idleOffMin);
    s_prefs.putUChar("remind", s_cfg.reminderMin);
    s_prefs.putUShort("calMin", s_cfg.calendarMin);
    s_prefs.putUShort("todoMin", s_cfg.todoMin);
    s_prefs.putUShort("wxMin", s_cfg.weatherMin);
    s_prefs.end();
    xSemaphoreGive(s_mtx);
}

void print(Stream& out) {
    const ClockConfig c = copy();
    out.printf("Nextcloud : %s  Benutzer-ID %s  Login %s  Passwort %s  TLS-Pruefung %s\n", c.ncUrl.isEmpty() ? "(nicht gesetzt)" : c.ncUrl.c_str(),
               c.ncUser.isEmpty() ? "-" : c.ncUser.c_str(), c.loginName().isEmpty() ? "-" : c.loginName().c_str(),
               c.ncPass.isEmpty() ? "-" : "***", c.tlsInsecure ? "AUS" : "an");
    String cals[6];
    const int n = calendarList(c, cals, 6);
    for (int i = 0; i < n; i++) out.printf("Kalender  : %s\n", cals[i].c_str());
    if (n == 0) out.println("Kalender  : (keiner)");
    out.printf("To-Do     : %s%s%s\n", c.todoPath.isEmpty() ? "(nicht gesetzt)" : c.todoPath.c_str(),
               c.todoHeading.isEmpty() ? "" : "  Abschnitt ", c.todoHeading.c_str());
    out.printf("Ort       : %s  %.4f, %.4f  (%s)\n", c.place.c_str(), c.lat, c.lon,
               c.locAuto ? "automatisch aus der IP, 'loc LAT LON' setzt ihn fest" : "fest, 'loc auto' = aus der IP");
    out.printf("E-Ink     : Wetter-Stand taeglich %02u:%02u, sonst nur bei Aenderungen (Pruefung alle 15 min)\n", c.epdHour, c.epdMinute);
    out.printf("Matrix    : Helligkeit Tag %u / Nacht %u (nach Sonnenstand), Auto-Aus %u min, Erinnerung %u min vorher, Blitz %s\n",
               c.brightnessDay, c.brightnessNight, c.idleOffMin, c.reminderMin, c.reminderFlash ? "an" : "aus");
    out.printf("Intervalle: Kalender %u min, To-Do %u min, Wetter %u min\n", c.calendarMin, c.todoMin, c.weatherMin);
}

int calendarList(const ClockConfig& c, String out[], int max) {
    int n = 0;
    int start = 0;
    const String& s = c.calendars;
    while (start <= (int)s.length() && n < max) {
        int nl = s.indexOf('\n', start);
        if (nl < 0) nl = s.length();
        String line = s.substring(start, nl);
        line.trim();
        if (line.length()) out[n++] = line;
        start = nl + 1;
    }
    return n;
}

}  // namespace clockcfg
