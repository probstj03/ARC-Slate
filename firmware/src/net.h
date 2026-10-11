#pragma once
// WiFi (known networks from NVS / ARC-Server, fallback "ARC-Net" access point),
// mDNS and the WebSocket server for ARC-Studio.
#include <Arduino.h>
#include <ArduinoJson.h>

namespace net {

void begin();                 // starts the connection task (non-blocking)
void loop();                  // heartbeat, deferred work (call from loop())

bool studioConnected();       // at least one WebSocket client
String statusLine();          // one line for the e-ink status bar
String ipString();

void broadcastStatus();
void sendSlateState();        // slate values changed at the device
void send(JsonDocument& doc); // to all WebSocket clients
void requestShotStep(int dir); // ask ARC-Studio for the next/previous shot
void requestNtpSync();        // forced NTP sync (station mode only)

// Known networks (persisted in NVS)
void addNetwork(const String& ssid, const String& pass, int priority);
void clearNetworks();
void printNetworks();

}  // namespace net
