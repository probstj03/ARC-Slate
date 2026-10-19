#include "fold.h"
#include <stdint.h>
#include <string.h>

namespace ac {

namespace {

const char* mapCodepoint(uint32_t cp, char* one) {
    if (cp == 0xA0) return " ";
    if (cp == 0xAD) return "";          // soft hyphen
    if (cp > 0xA0 && cp <= 0xFF) { one[0] = (char)cp; one[1] = 0; return one; }   // Latin-1: one byte
    switch (cp) {
        case 0x2013: case 0x2014: case 0x2212: return "-";
        case 0x2018: case 0x2019: case 0x201A: return "'";
        case 0x201C: case 0x201D: case 0x201E: return "\"";
        case 0x2022: return "*";
        case 0x2026: return "...";
        case 0x20AC: return "EUR";
        case 0x152: return "OE";
        case 0x153: return "oe";
        case 0x160: return "S";
        case 0x161: return "s";
        case 0x17D: return "Z";
        case 0x17E: return "z";
        case 0x141: return "L";
        case 0x142: return "l";
        default: return "";             // everything else (emoji, symbols) is dropped
    }
}

}  // namespace

size_t foldAppend(char* out, size_t cap, size_t len, const char* in, size_t inLen) {
    if (cap == 0) return 0;
    size_t i = 0;
    while (i < inLen) {
        const unsigned char c = (unsigned char)in[i];
        const char* rep = nullptr;
        char one[2] = {0, 0};
        if (c < 0x80) {
            if (c >= 0x20 && c < 0x7F) { one[0] = (char)c; rep = one; }
            else if (c == '\t' || c == '\n' || c == '\r') rep = " ";
            else rep = "";
            i++;
        } else {
            uint32_t cp = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            else { i++; continue; }      // stray continuation byte
            i++;
            for (int k = 0; k < extra && i < inLen; k++, i++) {
                if (((unsigned char)in[i] & 0xC0) != 0x80) break;
                cp = (cp << 6) | ((unsigned char)in[i] & 0x3F);
            }
            rep = mapCodepoint(cp, one);
        }
        const size_t n = strlen(rep);
        if (len + n >= cap) break;
        memcpy(out + len, rep, n);
        len += n;
    }
    out[len] = 0;
    return len;
}

size_t foldAscii(char* out, size_t cap, const char* in) {
    if (cap) out[0] = 0;
    return foldAppend(out, cap, 0, in, strlen(in));
}

}  // namespace ac
