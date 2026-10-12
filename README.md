# ARC-Slate

ARC-Slate ist die elektronische Filmklappe des ARC-Systems. Sie zeigt an:
- den laufenden **Timecode** auf 7 LED-Matrizen
- **Roll / Scene / Shot / Take** auf weiteren 9 Matrizen
- Produktionsdaten und einen **QR-Code** auf einem 7,5"-E-Ink-Display

Beim Schließen der Klappe erkennt ein Hallsensor den Moment und löst einen Blitz aus. Den
Zeitpunkt protokolliert die Slate mit Timecode. Gesteuert wird sie aus **ARC-Studio**
([ARC-Server](https://github.com/probstj03/ARC-Server)) oder am Gerät über einen Drehencoder.

## Hardware

- Platine **ARC_AP**, dieselbe wie bei [ARC-Play](https://github.com/probstj03/ARC-Play), unverändert, ESP32-S3-DevKitC-1 N16R8.
  Ein paar Teile werden nicht bestückt und die Header werden anders belegt:

  | Header | Bei ARC-Play | Bei ARC-Slate |
  |---|---|---|
  | TFT-Header U4 | Display | E-Ink + SPI-Bus der Matrizen |
  | SD-Header J1 | SD-Karte | Hallsensor, Blitz, Daten-Matrix |
  | J3 | Playback-Buchse | entfällt |

- **Verdrahtung:** [`hardware/wiring/ARC-Slate_Verdrahtung.svg`](hardware/wiring/ARC-Slate_Verdrahtung.svg)
- **Bauteile und Pins:** [`hardware/components.txt`](hardware/components.txt)
- **Gehäuse:** `hardware/3D-Objekte` (FreeCAD + STL + G-Code)
- `hardware/AS1.1`: alte Platine mit klassischem ESP32, **stillgelegt**, nur noch als Referenz

## Firmware (v2.1.0-dev)

PlatformIO-Projekt in `firmware/` (Arduino-Framework, ESP32-S3). Zeitbasis, Timecode, Jam Sync und
Netzwerk kommen aus **[ARC-Core](https://github.com/probstj03/ARC-Core)**. Das Repo muss neben
`ARC-Slate` liegen, weil `platformio.ini` es per `symlink://../../ARC-Core` einbindet.

| Modul | Aufgabe |
|---|---|
| ARC-Core | RTC-Uhr, phasengeregelter LTC-Generator, Decoder, Jam Sync mit Prüfphase, Latenz-Loopback, WLAN/ARC-Net, Protokoll v2, Ereignis-Log |
| `matrix` | beide MAX7219-Ketten über Hardware-SPI, Layout-Tabelle für die 3×3-Anzeige |
| `epaper` | Klappenkarte mit QR-Code (`ARC2\|code\|roll\|scene\|shot\|take`), Menü, Statuszeile |
| `ui` | Drehencoder + Menü |
| `clap` | Hallsensor, Blitz, Klappen-Ereignisse (RTC-µs, TC + Position im Frame) |
| `slate_link` | Slate-Protokoll: Shot-Daten, Slate-Zustand, Userbits |
| `state` | Shot-Daten + Einstellungen im NVS |

```bash
cd firmware
cp src/slate/secrets.example.h src/slate/secrets.h   # einmalig: eigene WLANs eintragen
pio run -e slate -t upload      # erstes Mal: Bootloader, Partitionstabelle und die Slate-App (ota_0)
pio device monitor -b 115200    # "help" zeigt alle Befehle
```

### Zwei Firmwares auf einem Gerät

Auf dem ESP32-S3 (16 MB) liegen zwei Apps (`partitions_16mb_dual.csv`). Es läuft immer nur eine, die andere bleibt im Flash.

| Partition | Firmware | Quellcode | Build/Upload |
|---|---|---|---|
| `ota_0` | **ARC-Slate** (Filmklappe) | `src/slate/` | `pio run -e slate -t upload` |
| `ota_1` | **ARC-Clock** (Uhr/Kalender) | `src/clock/` | `pio run -e clock -t upload` (schreibt nur die App, ändert die Auswahl nicht) |
| – | gemeinsam: Pins, SPI, App-Wechsel | `src/common/` | |

NVS (u. a. bekannte WLANs) und LittleFS sind für beide gemeinsam.

**Umschalten:**
- Slate-Menü „Uhr-Modus" bzw. seriell `mode clock`
- ARC-Clock: seriell `mode slate`
- beim Einschalten den Encoder-Knopf ≥ 1,5 s gedrückt halten wechselt zur jeweils anderen Firmware (Notausstieg)

Ist die andere App nicht installiert, bleibt das Gerät in der aktuellen Firmware und meldet das auf der Anzeige und seriell.
Die Auswahl bleibt über Neustarts erhalten (`otadata`).

> Beim ersten Flashen mit der neuen Partitionstabelle wird LittleFS verschoben: `/project.json` und `/events.jsonl`
> auf dem Gerät gehen verloren. Takes vorher in ARC-Studio abholen und das Projekt danach neu senden.

### Bedienung (Drehencoder)

- **Ruhe-Ansicht:**
  - Drehen = Take −/+ (kurz als „TAKE n" auf der Timecode-Anzeige)
  - Kurz drücken = Menü auf dem E-Ink
  - Lang drücken = nach einem Jam „Kabel abgezogen" bestätigen, im Record Run Start/Stop
- **Menü:** Take · Shot · Szene · Roll · Kamera · Modus · Bildrate · Helligkeit · Blitz testen · Uhr-Modus · Info · Fertig
  - Drehen = wählen, Drücken = bearbeiten, Lang drücken = schließen
  - Beim Bearbeiten: Drehen = Wert, Drücken = übernehmen, Lang drücken = verwerfen
  - Nach 10 s ohne Eingabe schließt sich das Menü.
- Menü und Statuszeile nutzen die E-Ink-Teil-Aktualisierung. Die Klappenkarte mit QR-Code wird
  komplett neu gezeichnet, 1,5 s nach der letzten Take-Änderung.
- Solange das Menü offen ist, ist die Klappe gesperrt (kein Blitz).
- **Offline:**
  - Mit einem Projekt aus ARC-Studio („Projekt auf Geräte“) blättert der Menüpunkt „Shot“ ohne Verbindung
    durch die gespeicherte Shotliste. Die nächste Take-Nummer kommt aus dem eigenen Klappen-Log.
  - Beim nächsten Verbinden holt ARC-Studio die Klappen ab.

### Inbetriebnahme – Testreihenfolge

1. **Start:** Die Matrizen zeigen „ARC SLATE". Beim ersten Start misst die Firmware die SQW-Flanke
   (~2,5 s, Ergebnis im seriellen Monitor). Das E-Ink zeigt den Bootscreen, danach die Klappenkarte.
2. **`?`** zeigt den Status. **`i2c`** sollte 0x68 (DS3231) finden.
3. **Matrizen:**
   - **`mtx 2`** zeigt in jedem Modul seine Nummer. Erwartet:
     - Timecode-Kette: Modul 0 ganz rechts.
     - Daten-Kette: 0–2 Roll, 3–5 Shot (Reihe um 180° gedreht), 6–8 Szene/Take.
   - Passt die Anordnung nicht, die Tabelle `FIELD_*` in `src/slate/matrix.cpp` anpassen.
   - **`mtx 1`** = alle LEDs an (Strom am Akku messen). **`mtx 0`** = normal.
4. **Encoder:** **`enc`** schaltet die Debug-Ausgabe ein. Läuft die Drehrichtung falsch: **`encrev`** und neu starten.
5. **Klappe:** Schließen → Blitz für 1 Frame, die LEDs halten den Klappen-TC 2,5 s. Öffnen → Take + 1.
   **`log`** zeigt die Ereignisse, **`c`** simuliert eine Klappe, **`f`** testet nur den Blitz.
6. **LTC-Ausgang:** **`1`** = Free Run. J2 an Recorder oder Kamera anschließen.
7. **Jam Sync mit ARC-Play:**
   1. ARC-Play in Free Run, Kabel J2 ↔ J2.
   2. Slate **`5`** (oder Menü → Modus → „Jam -> Free"). Nach ~3 s lockt die Slate und stellt ihre RTC, danach prüft sie 30 s lang („Jam pruefen“ in der Statuszeile).
   3. Die Statuszeile zeigt „KABEL ABZIEHEN". Kabel ziehen, dann Encoder lang drücken (oder **`u`**).
      Erst danach gibt die Slate wieder TC aus.
8. **Netz:**
   - **`wifi`** listet die bekannten Netze.
   - Ohne bekanntes Netz spannt die Slate „ARC-Net" auf (Passwort aus `secrets.h`). Laptop dort
     verbinden, ARC-Studio → Gerät `192.168.4.1` hinzufügen.

### Stand und Grenzen

- **Synchronität:** Der Generator ist auf ±0,75 Samples (≈16 µs) an die RTC-Zeitachse gekoppelt.
  - Die Ausgangslatenz kalibriert sich selbst über J2 (Loopback). `?` zeigt Latenz, Phasenfehler und Slips.
  - Jam Sync stellt die RTC per Frame-Zeitstempel und prüft danach 30 s lang Restversatz und Drift.
    Dabei trimmt er das DS3231-Aging-Register.
  - Noch nicht auf echter Hardware gemessen: Bitte Schritt 7 der Inbetriebnahme mit einem Stereo-Recorder
    an beiden J2-Ausgängen prüfen.
- **ARC-Studio:** Die aktuelle Version versteht `newTake` (wird beim Klappen zusätzlich gesendet),
  `nextShot`/`previousShot` und `status`. Die neuen Nachrichten sind `hello`, `event`, `slateState`,
  `pong`, `wifi_config_ack` sowie geplante Befehle mit `at`. ARC-Studio wertet sie mit Teil H des Plans aus.

## ARC-Clock (Uhr und Kalender)

Zweite Firmware für dieselbe Hardware (`src/clock/`, Umgebung `clock`). Aus der Klappe wird eine Schreibtischuhr:

| Klappe | Anzeige |
|---|---|
| **offen** | LED-Matrizen: oben `HH:MM:SS` (RTC-genau) mit Sekundenbalken in der obersten Zeile. Darunter links eine **Wetter-Animation**, in der Mitte Temperatur / gefühlt / Regen / Wind, rechts Mond, nächster Termin, Datum und Sonnenauf-/-untergang im Wechsel. |
| **geschlossen** | Matrizen aus. Das **E-Ink** zeigt die Tagesübersicht: Datum, Mondphase, Wetter mit Sonnenaufgang/-untergang und Tagesverlauf, Termine der nächsten Tage (Nextcloud), To-Do-Liste (Obsidian). |

Das E-Ink hat **keine Uhr** und wird nur **einmal am Tag** voll aktualisiert (Standard 05:30, `epdtime HH:MM`),
dazu nach einem Stromausfall, wenn der Tagesstand fehlt, und auf Befehl (`epd`). Es wird nie bei offener Klappe
angefasst. Alles Schnelle läuft auf den Matrizen.

**Matrizen im Detail:** 10 Minuten vor einem Termin läuft sein Titel als Laufschrift über die Zeitanzeige und das rechte
Feld blinkt (`remind MIN`). Beim ersten Öffnen am Tag funkeln alle Module kurz. Die Helligkeit wechselt zwischen
Sonnenauf- und -untergang (Tag/Nacht getrennt einstellbar).

**Encoder:** drehen = Helligkeit, kurz drücken = nächste Info-Seite, lang drücken = zur Klappen-Firmware wechseln.

### Einrichtung

Zugangsdaten stehen nie im Repo. Entweder einmalig vorbelegen (`cp src/clock/secrets.example.h src/clock/secrets.h`,
wird nur beim ersten Start gelesen) oder über die serielle Konsole eingeben:

```
nc url https://cloud.example.org
nc user julian
nc pass xxxxx-xxxxx-xxxxx-xxxxx-xxxxx      # App-Passwort (Nextcloud: Einstellungen > Sicherheit), nicht das Login-Passwort
cal add /remote.php/dav/calendars/julian/personal/
todo path Notizen/ToDo.md                  # Pfad der Obsidian-Datei in den Nextcloud-Dateien
todo head Heute                            # optional: nur Aufgaben unter dieser Überschrift
loc 52.52 13.405 Berlin                    # Standort für das Wetter
fetch                                      # alles jetzt abrufen und anzeigen (zeigt auch Fehler im Klartext)
epd force                                  # E-Ink sofort aktualisieren
```

`help` listet alle Befehle, `cfg` zeigt die Einstellungen (Passwort maskiert), `?` den Zustand der Datenquellen.
Die bekannten WLANs sind dieselben wie bei der Slate (NVS, werden in ARC-Studio gepflegt oder mit `wifi add` gesetzt).

### Datenquellen

- **Kalender:** CalDAV-`REPORT` mit `<c:expand>`. Die Nextcloud löst Wiederholungen auf, die Firmware braucht keinen
  RRULE-Parser. Mehrere Kalender möglich (`cal add` mehrfach). Abruf alle 15 min.
- **To-Do:** WebDAV-`GET` der Markdown-Datei mit ETag (unverändert = 304). Erkannt werden `- [ ]` und `* [/]`, Fälligkeiten der
  Obsidian-Tasks-Erweiterung (`📅 2026-10-15`, `⏳`), Dataview-Felder (`[due:: 2026-10-15]`) und Prioritäten (⏫ 🔼).
  Sortierung: überfällig, heute, 7 Tage, später, ohne Datum. Nur Anzeige (kein Zurückschreiben).
- **Wetter:** [Open-Meteo](https://open-meteo.com) (kein Schlüssel nötig), alle 30 min.
- **Mond:** lokal berechnet.
- Der letzte gute Stand liegt in LittleFS (`/clock_cache.bin`) und erscheint nach einem Neustart sofort. Fehlt der
  Abruf, steht in der Fußzeile des E-Inks ein „!" hinter der Quelle, bei einem Fehler zusätzlich der Grund.
- Abrufe laufen nur in einem **echten WLAN mit Internet** (nicht in ARC-Net) und nacheinander (TLS braucht viel RAM).
- Sonderzeichen: Die E-Ink-Schriften haben keine Umlaute. Texte werden zu ASCII gefaltet (ä→ae, ß→ss, Emoji entfallen).

### Sicherheit der Verbindung

HTTPS wird gegen eine kuratierte Liste gängiger Wurzelzertifikate geprüft (`src/clock/ca_roots.h`, erzeugt mit
`tools/gen_ca_roots.py`). Für eine eigene CA (selbst gehostete Nextcloud) die PEM-Datei als `/ca.pem` ins LittleFS legen
(`pio run -e clock -t uploadfs` mit einem `data/`-Ordner). Nur wenn gar nichts anderes geht: `nc insecure 1` schaltet die
Prüfung ab. Das App-Passwort geht dann im Klartext-Zertifikatsvertrauen raus, nur im eigenen LAN verwenden.

### Entwicklung ohne Hardware

`py test_host/run.py` baut und prüft die reinen Module (`src/clock/core/`: Datum/Zeitzone, iCal/CalDAV, To-Do, Wetter,
Mond, Agenda, Animationen) auf dem PC und rendert das E-Ink-Layout mit der echten Adafruit-GFX-Bibliothek nach
`test_host/out/epd_preview*.png`. Gebraucht werden ein C++-Compiler (`g++`/`clang++`, sonst `pip install ziglang`) und Pillow.
Einmal vorher `pio run -e clock`, damit die Bibliotheken in `.pio/libdeps` liegen.

### Stand der Uhr

- Gebaut (ohne Warnungen) und am PC getestet, **noch nicht auf der Hardware**. Beim ersten Test prüfen:
  - Ausrichtung der Anzeige: Sekundenbalken in der **obersten** Zeile, Wetter-Animation richtig herum
  - Klappe offen/zu schaltet die Matrizen
  - `fetch` gegen die eigene Nextcloud (Login, Kalenderpfad, To-Do-Datei)
  - ein kompletter Tageszyklus des E-Inks
- Nicht enthalten: Schlafmodus für Akkubetrieb (die Uhr nimmt dauerhafte Versorgung an), Abhaken von Aufgaben auf dem Gerät,
  ARC-Studio-Karte für die Uhr (ARC-Studio ignoriert den Gerätetyp `clock`).
