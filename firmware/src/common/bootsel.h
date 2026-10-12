#pragma once
// Two firmwares share the chip: ota_0 = ARC-Slate, ota_1 = ARC-Clock.
// Switching sets the boot partition (otadata, persistent) and restarts.
#include <Arduino.h>

namespace bootsel {

enum class App { Slate, Clock };

const char* name(App app);
App running();                 // the app this binary was built as
App other();                   // the one that is not running
bool installed(App app);       // partition holds a valid image (magic byte)
bool switchTo(App app);        // does not return on success (restarts)
bool switchToOther();

// True if `pin` (active LOW, pull-up) stays pressed for `ms` right after power-on.
bool heldAtBoot(int pin, uint32_t ms);

}  // namespace bootsel
