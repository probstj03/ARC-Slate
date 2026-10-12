#pragma once
// Copy to secrets.h (not versioned). Used only on the first boot of ARC-Clock to pre-fill the
// settings; afterwards they live in NVS and are changed with the serial commands (`help`).
// WiFi networks: shared with ARC-Slate (NVS), managed in ARC-Studio or with `wifi add`.
#include "arc_net.h"
static const arc::net::DefaultNetwork kDefaultWifi[] = {
    {"MeinWLAN", "passwort", 10},
};
static const char* const kArcNetPassword = "arc-net-2026";

// Nextcloud: use an app password (Settings > Security > Devices & sessions), not your login password.
#define CLOCK_NC_URL "https://cloud.example.org"
#define CLOCK_NC_USER "julian"
#define CLOCK_NC_PASS "xxxxx-xxxxx-xxxxx-xxxxx-xxxxx"
// one CalDAV path per line
#define CLOCK_NC_CALENDARS "/remote.php/dav/calendars/julian/personal/\n"
// md file of the Obsidian vault inside the Nextcloud files
#define CLOCK_NC_TODO "Notizen/ToDo.md"

#define CLOCK_LAT 52.52f
#define CLOCK_LON 13.405f
#define CLOCK_PLACE "Berlin"
