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

## Firmware

PlatformIO-Projekt in `firmware/` (Arduino-Framework, ESP32-S3).

```bash
cd firmware
pio run -t upload
pio device monitor -b 115200
```

Die aktuelle Firmware ist aus ARC-Play abgeleitet. Sie hat LTC-Generator, Decoder und Jam Sync und
zeigt den Timecode auf der LED-Matrix. Gerade wird sie für die ARC_AP-Belegung neu aufgebaut.

## Geplante Bedienung (Drehencoder)

- **Ruhe-Ansicht:**
  - Drehen = Take −/+
  - Kurz drücken = Menü auf dem E-Ink
  - Lang drücken = zurück
- **Menü:** Take, Shot, Roll, Kamera, Run-Modus/TC, Jam Sync, Helligkeit, Netzwerk/Info.
  - Drehen = wählen bzw. Wert ändern, Drücken = bearbeiten bzw. übernehmen.
- **Anzeige:** Das E-Ink aktualisiert im Menü nur Teilbereiche (schnell). Beim Verlassen des Menüs
  zeichnet es einmal komplett neu.

## Roadmap

Diese Punkte sind geplant:
- Gemeinsame Library **ARC-Core** mit:
  - RTC-disziplinierter Zeitbasis
  - subframe-genauem Jam Sync mit ARC-Play
  - Klappen-Ereignissen mit µs-Zeitstempel
  - Protokoll v2
- Treiber für beide Matrix-Ketten (16 Module über Hardware-SPI), E-Ink mit QR-Code im Format `ARC2|code|roll|scene|shot|take`
- WLAN-Liste vom Server, eigenes Netz „ARC-Net" als Rückfall, Offline-Projekte
