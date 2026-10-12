#include "bootsel.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace bootsel {

namespace {

constexpr uint8_t IMAGE_MAGIC = 0xE9;   // ESP_IMAGE_HEADER_MAGIC

const esp_partition_t* partitionOf(App app) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    app == App::Slate ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                    nullptr);
}

}  // namespace

const char* name(App app) { return app == App::Slate ? "ARC-Slate" : "ARC-Clock"; }

App running() {
#if defined(ARC_APP_CLOCK)
    return App::Clock;
#else
    return App::Slate;
#endif
}

App other() { return running() == App::Slate ? App::Clock : App::Slate; }

bool installed(App app) {
    const esp_partition_t* p = partitionOf(app);
    if (!p) return false;
    uint8_t magic = 0;
    return esp_partition_read(p, 0, &magic, 1) == ESP_OK && magic == IMAGE_MAGIC;
}

bool switchTo(App app) {
    const esp_partition_t* p = partitionOf(app);
    if (!p || !installed(app)) {
        Serial.printf("[BOOT] %s ist nicht installiert (Partition leer)\n", name(app));
        return false;
    }
    if (esp_ota_set_boot_partition(p) != ESP_OK) {
        Serial.printf("[BOOT] Boot-Partition für %s konnte nicht gesetzt werden\n", name(app));
        return false;
    }
    Serial.printf("[BOOT] Wechsel zu %s ...\n", name(app));
    Serial.flush();
    delay(100);
    ESP.restart();
    return true;
}

bool switchToOther() { return switchTo(other()); }

bool heldAtBoot(int pin, uint32_t ms) {
    pinMode(pin, INPUT_PULLUP);
    delay(20);
    if (digitalRead(pin) != LOW) return false;
    const uint32_t start = millis();
    while (millis() - start < ms) {
        if (digitalRead(pin) != LOW) return false;
        delay(10);
    }
    return true;
}

}  // namespace bootsel
