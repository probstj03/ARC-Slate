// Renders the ARC-Clock e-ink overview with sample data into a PGM image (host preview).
// The real layout code and the real Adafruit_GFX library draw; only the pixels go to a buffer.
//   py test_host/run.py        -> test_host/out/epd_preview.png
#include <Adafruit_GFX.h>
#include <stdio.h>
#include <string>
#include "civil.h"
#include "fold.h"
#include "ics.h"
#include "doc.h"
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
    "---\n"
    "banner: \"[[DSC_2394.jpg]]\"\n"
    "banner_position: 26\n"
    "icon: \xF0\x9F\x93\x8B" "\n"
    "---\n"
    "# To-Do\xC2\xB4" "s\n"
    "\n"
    "- [ ] Claude exportieren\n"
    "- [ ] Gitarren neu besaiten\n"
    "- [ ] Kleiderschrank aufr\xC3\xA4" "umen\n"
    "- [ ] Teleskop Alt Projekt\n"
    "- [ ] Linux installieren\n"
    "- [x] Kabel Andi!!\n"
    "\n"
    "- [ ] Datei Backup\n"
    "- [ ] Passw\xC3\xB6" "rter verbessern\n"
    "\n"
    "- [ ] n8n (Automatisierung)\n"
    "- [ ] Paperless\n"
    "- [ ] Stirling.pdf\n"
    "- [ ] Open Webui\n"
    "- [ ] Invoice ninja\n"
    "\n"
    "Mitnehmen\n"
    "\n"
    "- Rohr\n"
    "- Deckel\n"
    "- Schiene 1\n"
    "- Schiene 2\n"
    "- Alte schiene\n"
    "- Linse\n"
    "- Flansch\n"
    "- Koffer 1\n"
    "- Koffer 2\n"
    "- Telrad\n"
    "- Montierung\n"
    "- EQ-5? \n"
    "- Stativ\n"
    "- Filmklappe\n"
    "- Arc-Play\n"
    "- MacBook\n"
    "\n"
    "## Hobby\n"
    "\n"
    "- [ ] Lage der Nation App probieren Direktzahlung\n"
    "- [ ] Berlin Arc Play fragen\n"
    "- [ ] Filmbox testen Alexa auf Film\n"
    "- [ ] Nikon Plakat\n"
    "\n"
    "# Projekte\n"
    " [[Einkaufsliste]]![[projects.base]]\n"
    "# Notes\n"
    "![[notes.base]]\n"
    "\n"
    "# Resources\n"
    "![[resources.base]]\n"
    "\n"
    "\n"
    "\n"
    "## Website Privat\n"
    "\n"
    "[Google Search Console](https://search.google.com/search-console?resource_id=sc-domain%3Ajulianprobst.de&after_verification_success=)\n"
    "\n"
    "### Portfolio:\n"
    "\n"
    "- [ ] <span style=\"color:rgb(112, 48, 160)\">Gottesdienststreaming</span> Corona (Prio 2)\n"
    "- [ ] <span style=\"color:rgb(112, 48, 160)\">Visitenkarten</span> (Prio 3)\n"
    "- [ ] Video <span style=\"color:rgb(112, 48, 160)\">Felix</span> (Prio 3)\n"
    "\n"
    "`#7a57ed`\n"
    "\n"
    "`#221345` (Backfarbe)\n"
    "\n"
    "# Merkliste\n"
    "\n"
    "- [ ] [Filterwheel](https://www.thingiverse.com/thing:3679901)\n"
    "- [ ] **Eine Geschichte des Universums in 100 Sterne**!\n"
    "\n"
    "# Einweihungsfeier\n"
    "\n"
    "Anzahl: 15-20\n"
    "\n"
    "**Einkaufsliste:**\n"
    "- [ ] Tomaten\n"
    "- [ ] Rotes Pesto\n"
    "- [ ] Getrocknete Tomaten\n"
    "- [ ] Mozzarella\n"
    "- [ ] Kasten Bier (Keller?)\n"
    "- [ ] Limo (Keller?)\n"
    "- [ ] Wein 3x wei\xC3\x9F" " 3x rot\n"
    "- [ ] Kichererbsen \n"
    "- [ ] Zitrone\n"
    "- [ ] Aperol?\n"
    "- [ ] Sekt\n"
    "- [ ] Orange\n"
    "\n"
    "**To-Do:**\n"
    "- [ ] Lampen\n";

}  // namespace

static clockdata::Data* g_data;
static void onDocLine(const DocLine& l, void*) { if (g_data->docCount < kMaxDocLines) g_data->doc[g_data->docCount++] = l; }

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "epd_preview.pgm";
    const int variant = argc > 2 ? atoi(argv[2]) : 0;      // 0 normal, 1 empty/offline, 2 busy night/snow, 3 night events, 4 waxing gibbous moon

    const int64_t now = variant == 4 ? L(2026, 10, 20, 5, 30) : variant == 3 ? L(2026, 10, 12, 18, 30) : L(2026, 10, 12, 5, 30);
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
        if (variant == 3) {                      // events at night: the night strip appears
            a.add(ev(L(2026, 10, 12, 22, 30), L(2026, 10, 12, 23, 30), false, "Nachtschicht Schnitt"), today, today + 7 * 86400);
            a.add(ev(L(2026, 10, 13, 5, 45), L(2026, 10, 13, 7, 0), false, "Flug nach Hamburg"), today, today + 7 * 86400);
        }
        a.finalize();
        data->src[clockdata::SRC_CALENDAR].ok = true;
        data->src[clockdata::SRC_CALENDAR].at = now - 60;

        DocParser dp(onDocLine, nullptr);
        std::string md = kTodoMd;                                        // optional 3rd argument: a markdown file instead
        if (argc > 3) {
            if (FILE* mf = fopen(argv[3], "rb")) {
                md.clear();
                char chunk[512];
                size_t n;
                while ((n = fread(chunk, 1, sizeof(chunk), mf)) > 0) md.append(chunk, n);
                fclose(mf);
            }
        }
        dp.feed(md.c_str(), md.size());
        dp.finish();
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
