// Renders the ARC-Clock e-ink overview with sample data into a PGM image (host preview).
// The real layout code and the real Adafruit_GFX library draw; only the pixels go to a buffer.
//   py test_host/run.py        -> test_host/out/epd_preview.png
#include <Adafruit_GFX.h>
#include <stdio.h>
#include <string>
#include "civil.h"
#include "fold.h"
#include "ics.h"
#include "todo.h"
#include "weather.h"
#include "../src/clock/core/data_model.h"
#include "../src/clock/layout/epd_layout.h"

using namespace ac;

namespace {

class Surface : public Adafruit_GFX {
public:
    Surface() : Adafruit_GFX(epdlayout::W, epdlayout::H), _px(epdlayout::W * epdlayout::H, 255) {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (x < 0 || y < 0 || x >= epdlayout::W || y >= epdlayout::H) return;
        _px[y * epdlayout::W + x] = color == 0 ? 0 : 255;
    }
    bool save(const char* path) const {
        FILE* f = fopen(path, "wb");
        if (!f) return false;
        fprintf(f, "P5\n%d %d\n255\n", epdlayout::W, epdlayout::H);
        fwrite(_px.data(), 1, _px.size(), f);
        fclose(f);
        return true;
    }

private:
    std::vector<uint8_t> _px;
};

int64_t L(int y, int mo, int d, int h = 0, int mi = 0) {
    Civil c; c.year = y; c.month = mo; c.day = d; c.hour = h; c.minute = mi;
    return toSec(c);
}

Event ev(int64_t start, int64_t end, bool allDay, const char* title, const char* loc = "") {
    Event e;
    e.start = start; e.end = end; e.allDay = allDay;
    foldAscii(e.summary, sizeof(e.summary), title);      // UTF-8 -> Latin-1 like the iCal parser
    foldAscii(e.location, sizeof(e.location), loc);
    return e;
}

std::string weatherJson(int wmo, bool day) {
    std::string j = "{\"current\":{\"time\":\"2026-10-12T05:30\",\"temperature_2m\":9.4,\"relative_humidity_2m\":82,"
                    "\"apparent_temperature\":7.1,\"is_day\":" + std::string(day ? "1" : "0") + ",\"weather_code\":" + std::to_string(wmo) +
                    ",\"wind_speed_10m\":14.2},\"hourly\":{\"time\":[";
    char b[64];
    for (int i = 0; i < 48; i++) { snprintf(b, sizeof(b), "%s\"2026-10-%02dT%02d:00\"", i ? "," : "", 12 + i / 24, i % 24); j += b; }
    j += "],\"temperature_2m\":[";
    for (int i = 0; i < 48; i++) {
        const double h = i % 24;
        snprintf(b, sizeof(b), "%s%.1f", i ? "," : "", 8.0 + 7.0 * sin((h - 9.0) / 24.0 * 2 * 3.14159265) + (i >= 24 ? -2 : 0));
        j += b;
    }
    j += "],\"precipitation_probability\":[";
    for (int i = 0; i < 48; i++) { snprintf(b, sizeof(b), "%s%d", i ? "," : "", (i % 24 >= 13 && i % 24 <= 17) ? 40 + (i % 24 - 13) * 12 : 5); j += b; }
    j += "],\"weather_code\":[";
    for (int i = 0; i < 48; i++) { snprintf(b, sizeof(b), "%s%d", i ? "," : "", wmo); j += b; }
    j += "]},\"daily\":{\"time\":[\"2026-10-12\",\"2026-10-13\",\"2026-10-14\"],\"weather_code\":[" + std::to_string(wmo) + ",3,61],"
         "\"temperature_2m_max\":[15.2,13.0,12.1],\"temperature_2m_min\":[8.1,7.4,6.9],"
         "\"sunrise\":[\"2026-10-12T07:12\",\"2026-10-13T07:14\",\"2026-10-14T07:16\"],"
         "\"sunset\":[\"2026-10-12T18:40\",\"2026-10-13T18:38\",\"2026-10-14T18:36\"],\"precipitation_probability_max\":[80,30,55]}}";
    return j;
}

const char* kTodoMd =
    "## Heute\n"
    "- [ ] Steuererkl\xC3\xA4rung abgeben \xF0\x9F\x93\x85 2026-10-09 \xE2\x8F\xAB\n"
    "- [ ] Bericht an **Herrn M\xC3\xBCller** senden [[Projekt X|Projekt]] \xF0\x9F\x93\x85 2026-10-12\n"
    "- [ ] Drehgenehmigung Studio Nord pr\xC3\xBC" "fen \xF0\x9F\x93\x85 2026-10-12\n"
    "- [ ] Mikrofone laden \xF0\x9F\x93\x85 2026-10-13\n"
    "- [ ] Rechnung Verleih bezahlen \xF0\x9F\x93\x85 2026-10-16\n"
    "- [ ] Ein sehr langer Aufgabentext der garantiert nicht in die Spalte passt und gek\xC3\xBCrzt werden muss #privat\n"
    "- [ ] Ersatzakku bestellen\n"
    "- [ ] Neue Kabeltrommel kaufen\n"
    "- [ ] Lizenz verl\xC3\xA4ngern \xE2\x8F\xB3 2026-10-22\n"
    "- [ ] Foto-Backup pr\xC3\xBC" "fen\n"
    "- [ ] Aufgabe elf\n"
    "- [ ] Aufgabe zw\xC3\xB6lf\n";

}  // namespace

static clockdata::Data* g_data;
static void onTask(const Task& t, void*) { if (g_data->taskCount < clockdata::kMaxTasks) g_data->tasks[g_data->taskCount++] = t; }

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "epd_preview.pgm";
    const int variant = argc > 2 ? atoi(argv[2]) : 0;      // 0 normal, 1 empty/offline, 2 busy night/snow

    const int64_t now = L(2026, 10, 12, 5, 30);
    const int64_t today = dayStart(now);
    clockdata::Data* data = new clockdata::Data();
    g_data = data;

    if (variant != 1) {
        Agenda& a = data->agenda;
        a.add(ev(L(2026, 10, 12), L(2026, 10, 13), true, "Geburtstag Oma"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 12, 9, 30), L(2026, 10, 12, 10, 30), false, "Team-Meeting", "Raum 4"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 12, 14, 30), L(2026, 10, 12, 15, 30), false, "Zahnarzt Dr. Müller & Kollegen", "Hauptstraße 5"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 13, 8, 0), L(2026, 10, 13, 18, 0), false, "Dreh Szene 12 (Studio Nord)", "Hamburg"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 13, 11, 0), L(2026, 10, 13, 11, 30), false, "Telefonat Versicherung"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 14), L(2026, 10, 16), true, "Urlaub Anna"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 15, 7, 42), L(2026, 10, 15, 11, 15), false, "Zug nach Hamburg ICE 574 Gleis 8", "Hbf"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 16, 19, 0), L(2026, 10, 16, 22, 0), false, "Kino mit Freunden"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 17, 10, 0), L(2026, 10, 17, 11, 0), false, "Frühstück"), today, today + 7 * 86400);
        a.add(ev(L(2026, 10, 18, 10, 0), L(2026, 10, 18, 11, 0), false, "Sonntagsspaziergang"), today, today + 7 * 86400);
        a.finalize();
        data->src[clockdata::SRC_CALENDAR].ok = true;
        data->src[clockdata::SRC_CALENDAR].at = now - 60;

        TodoParser tp("Heute", onTask, nullptr);
        tp.feed(kTodoMd, strlen(kTodoMd));
        tp.finish();
        sortTasks(data->tasks, data->taskCount, today);
        data->src[clockdata::SRC_TODO].ok = true;
        data->src[clockdata::SRC_TODO].at = now - 60;

        const int wmo = variant == 2 ? 73 : 3;
        const std::string json = weatherJson(wmo, variant != 2 ? true : false);
        parseOpenMeteo(json.c_str(), json.size(), now, data->weather);
        data->src[clockdata::SRC_WEATHER].ok = data->weather.valid;
        data->src[clockdata::SRC_WEATHER].at = now - 120;
    } else {
        strcpy(data->src[clockdata::SRC_CALENDAR].error, "Login abgelehnt (App-Passwort?)");
        strcpy(data->src[clockdata::SRC_TODO].error, "Pfad nicht gefunden");
        strcpy(data->src[clockdata::SRC_WEATHER].error, "connection refused");
    }

    Surface s;
    epdlayout::Options opt;
    opt.place = "Berlin";
    opt.version = "ARC-Clock 2.1.0-dev";
    epdlayout::draw(s, *data, now, opt);
    if (!s.save(out)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    printf("wrote %s\n", out);
    delete data;
    return 0;
}
