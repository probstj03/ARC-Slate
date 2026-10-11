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

## Firmware (v2.0.0-dev)

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
cp src/secrets.example.h src/secrets.h   # einmalig: eigene WLANs eintragen
pio run -t upload
pio device monitor -b 115200             # "help" zeigt alle Befehle
```

### Bedienung (Drehencoder)

- **Ruhe-Ansicht:**
  - Drehen = Take −/+ (kurz als „TAKE n" auf der Timecode-Anzeige)
  - Kurz drücken = Menü auf dem E-Ink
  - Lang drücken = nach einem Jam „Kabel abgezogen" bestätigen, im Record Run Start/Stop
- **Menü:** Take · Shot · Szene · Roll · Kamera · Modus · Bildrate · Helligkeit · Blitz testen · Info · Fertig
  - Drehen = wählen, Drücken = bearbeiten, Lang drücken = schließen
  - Beim Bearbeiten: Drehen = Wert, Drücken = übernehmen, Lang drücken = verwerfen
  - Nach 10 s ohne Eingabe schließt sich das Menü.
- Menü und Statuszeile nutzen die E-Ink-Teil-Aktualisierung. Die Klappenkarte mit QR-Code wird
  komplett neu gezeichnet, 1,5 s nach der letzten Take-Änderung.
- Solange das Menü offen ist, ist die Klappe gesperrt (kein Blitz).

### Inbetriebnahme – Testreihenfolge

1. **Start:** Die Matrizen zeigen „ARC SLATE". Beim ersten Start misst die Firmware die SQW-Flanke
   (~2,5 s, Ergebnis im seriellen Monitor). Das E-Ink zeigt den Bootscreen, danach die Klappenkarte.
2. **`?`** zeigt den Status. **`i2c`** sollte 0x68 (DS3231) finden.
3. **Matrizen:**
   - **`mtx 2`** zeigt in jedem Modul seine Nummer. Erwartet:
     - Timecode-Kette: Modul 0 ganz rechts.
     - Daten-Kette: 0–2 Roll, 3–5 Shot (Reihe um 180° gedreht), 6–8 Szene/Take.
   - Passt die Anordnung nicht, die Tabelle `FIELD_*` in `src/matrix.cpp` anpassen.
   - **`mtx 1`** = alle LEDs an (Strom am Akku messen). **`mtx 0`** = normal.
4. **Encoder:** **`enc`** schaltet die Debug-Ausgabe ein. Läuft die Drehrichtung falsch: **`encrev`** und neu starten.
5. **Klappe:** Schließen → Blitz für 1 Frame, die LEDs halten den Klappen-TC 2,5 s. Öffnen → Take + 1.
   **`log`** zeigt die Ereignisse, **`c`** simuliert eine Klappe, **`f`** testet nur den Blitz.
6. **LTC-Ausgang:** **`1`** = Free Run. J2 an Recorder oder Kamera anschließen.
7. **Jam Sync mit ARC-Play:**
   1. ARC-Play in Free Run, Kabel J2 ↔ J2.
   2. Slate **`5`** (oder Menü → Modus → „Jam -> Free"). Nach ~3 s lockt die Slate und stellt ihre RTC.
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
