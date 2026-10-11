#pragma once
// Copy to secrets.h (not versioned) and fill in your networks.
// Used only on first boot; afterwards the list lives in NVS and is managed
// from ARC-Server ("Bekannte Netzwerke") or via the serial command "wifi".
struct DefaultWifi { const char* ssid; const char* pass; int priority; };
static const DefaultWifi kDefaultWifi[] = {
    {"MeinWLAN", "passwort", 10},
};
static const char* const kArcNetPassword = "arc-net-2026";
