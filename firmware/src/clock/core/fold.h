#pragma once
// UTF-8 -> Latin-1 for the e-ink fonts (Lato, glyphs 0x20..0xFF): umlauts, ß, ° and accented letters stay
// (one byte each), typographic quotes/dashes are simplified, characters outside Latin-1 (emoji, symbols) are
// dropped. Pure, host-testable. The LED matrix font is ASCII only; mtx::renderText maps the umlauts there.
#include <stddef.h>

namespace ac {

// Appends the folded form of `in` to `out` (capacity `cap` incl. terminator). Returns the new length.
size_t foldAppend(char* out, size_t cap, size_t len, const char* in, size_t inLen);

// Whole-string convenience: folds `in` into `out`.
size_t foldAscii(char* out, size_t cap, const char* in);

}  // namespace ac
