#pragma once
// Slate lid (hall sensor): open / closed, debounced.
// Open  = LED matrices on. Closed = matrices dark, only the e-ink shows its daily overview.
#include <Arduino.h>

namespace lid {

void begin();
bool open();
uint32_t sinceMs();                       // time since the last change
void onChange(void (*callback)(bool open));

}  // namespace lid
