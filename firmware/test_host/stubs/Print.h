#pragma once
#include "Arduino.h"

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t* buf, size_t n) {
        size_t c = 0;
        for (size_t i = 0; i < n; i++) c += write(buf[i]);
        return c;
    }
    size_t write(const char* s) { return write((const uint8_t*)s, strlen(s)); }
    size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
    size_t print(const char* s) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v) { return print(String(v)); }
    size_t print(int v, int) { return print(String(v)); }
    size_t println() { return write('\n'); }
    size_t println(const String& s) { return print(s) + write('\n'); }
    size_t println(const char* s) { return print(s) + write('\n'); }
};
