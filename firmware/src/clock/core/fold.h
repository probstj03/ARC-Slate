#pragma once
// UTF-8 -> ASCII for the GFX fonts (Adafruit GFX fonts only contain 0x20..0x7E).
// ä->ae ö->oe ü->ue ß->ss, accents dropped, typographic quotes/dashes simplified,
// emoji and other symbols removed. Pure, host-testable.
#include <stddef.h>

namespace ac {

// Appends the folded form of `in` to `out` (capacity `cap` incl. terminator). Returns the new length.
size_t foldAppend(char* out, size_t cap, size_t len, const char* in, size_t inLen);

// Whole-string convenience: folds `in` into `out`.
size_t foldAscii(char* out, size_t cap, const char* in);

}  // namespace ac
