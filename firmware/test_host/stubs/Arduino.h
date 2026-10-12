// Minimal Arduino stand-in so that the real Adafruit_GFX and the clock layout compile on the host.
#pragma once
#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define PROGMEM
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))
#define pgm_read_ptr(addr) (*(void *const *)(addr))
#define F(x) x
class __FlashStringHelper;
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define radians(deg) ((deg)*DEG_TO_RAD)
typedef bool boolean;
typedef uint8_t byte;
#define HEX 16
#define DEC 10

class String {
public:
    String() {}
    String(const char* s) : _s(s ? s : "") {}
    String(char c) : _s(1, c) {}
    String(int v) : _s(std::to_string(v)) {}
    String(long v) : _s(std::to_string(v)) {}
    String(unsigned v) : _s(std::to_string(v)) {}
    String(const std::string& s) : _s(s) {}

    size_t length() const { return _s.size(); }
    const char* c_str() const { return _s.c_str(); }
    bool isEmpty() const { return _s.empty(); }
    char operator[](size_t i) const { return _s[i]; }
    String& operator+=(const String& o) { _s += o._s; return *this; }
    String& operator+=(const char* o) { _s += o; return *this; }
    String& operator+=(char c) { _s += c; return *this; }
    String& concat(const char* s, size_t n) { _s.append(s, n); return *this; }
    String operator+(const String& o) const { return String(_s + o._s); }
    String operator+(const char* o) const { return String(_s + o); }
    bool operator==(const String& o) const { return _s == o._s; }
    bool startsWith(const String& p) const { return _s.compare(0, p._s.size(), p._s) == 0; }
    bool endsWith(const String& p) const { return _s.size() >= p._s.size() && _s.compare(_s.size() - p._s.size(), p._s.size(), p._s) == 0; }
    void remove(size_t index) { if (index < _s.size()) _s.erase(index); }
    void remove(size_t index, size_t count) { if (index < _s.size()) _s.erase(index, count); }
    String substring(size_t from) const { return String(from < _s.size() ? _s.substr(from) : std::string()); }
    String substring(size_t from, size_t to) const { return String(from < _s.size() ? _s.substr(from, to - from) : std::string()); }
    int indexOf(char c, size_t from = 0) const { const size_t p = _s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    void trim() {
        const size_t a = _s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) { _s.clear(); return; }
        _s = _s.substr(a, _s.find_last_not_of(" \t\r\n") - a + 1);
    }
    long toInt() const { return atol(_s.c_str()); }
    void reserve(size_t n) { _s.reserve(n); }

private:
    std::string _s;
};
inline String operator+(const char* a, const String& b) { return String(a) + b; }

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
