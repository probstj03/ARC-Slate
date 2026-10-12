#include "ics.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "civil.h"
#include "fold.h"

namespace ac {

namespace {

bool nameIs(const char* s, size_t n, const char* name) {
    if (strlen(name) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)s[i]) != name[i]) return false;
    return true;
}

// "TZID=Europe/Berlin;VALUE=DATE" -> value of key (case-insensitive key), empty if absent.
bool paramValue(const char* params, size_t len, const char* key, char* out, size_t cap) {
    const size_t kl = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && params[j] != ';') j++;
        if (j - i > kl && params[i + kl] == '=') {
            bool match = true;
            for (size_t k = 0; k < kl; k++)
                if (toupper((unsigned char)params[i + k]) != key[k]) { match = false; break; }
            if (match) {
                size_t n = j - i - kl - 1;
                if (n >= cap) n = cap - 1;
                memcpy(out, params + i + kl + 1, n);
                out[n] = 0;
                return true;
            }
        }
        i = j + 1;
    }
    return false;
}

// iCalendar TEXT -> plain: \n -> space, \, \; \\ unescaped.
size_t unescape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        if (in[i] == '\\' && in[i + 1]) {
            i++;
            out[o++] = (in[i] == 'n' || in[i] == 'N') ? ' ' : in[i];
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
    return o;
}

// "PT1H30M", "P1D", "P1W", "-PT15M" -> seconds
int64_t parseDuration(const char* s) {
    int64_t sign = 1, total = 0, num = 0;
    bool time = false;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    if (*s != 'P') return 0;
    for (s++; *s; s++) {
        if (isdigit((unsigned char)*s)) { num = num * 10 + (*s - '0'); continue; }
        switch (*s) {
            case 'T': time = true; break;
            case 'W': total += num * 7 * 86400; break;
            case 'D': total += num * 86400; break;
            case 'H': total += num * 3600; break;
            case 'M': total += time ? num * 60 : 0; break;
            case 'S': total += num; break;
            default: break;
        }
        num = 0;
    }
    return sign * total;
}

// Converts an iCalendar date(-time) value to local seconds.
bool toLocal(const char* value, const char* params, size_t paramLen, int64_t& out, bool& allDay) {
    Civil c;
    bool hasTime, utc;
    if (!parseIcalDateTime(value, c, hasTime, utc)) return false;
    char v[16];
    allDay = !hasTime || (paramValue(params, paramLen, "VALUE", v, sizeof(v)) && strcasecmp(v, "DATE") == 0);
    const int64_t sec = toSec(c);
    out = (utc && hasTime) ? utcToLocal(sec) : sec;
    if (allDay) out = dayStart(out);
    return true;
}

}  // namespace

// ── IcsParser ────────────────────────────────────────────────────────────

void IcsParser::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\n') endPhysical();
        else if (c != '\r' && _curLen + 1 < kLine) _cur[_curLen++] = c;
    }
}

void IcsParser::flush() {
    if (_curLen) endPhysical();
    if (_hasPending) {
        _pending[_pendingLen] = 0;
        process(_pending);
        _hasPending = false;
        _pendingLen = 0;
    }
    _depth = 0;
}

void IcsParser::endPhysical() {
    _cur[_curLen] = 0;
    if (_curLen == 0) return;
    if (_cur[0] == ' ' || _cur[0] == '\t') {
        // continuation of the previous logical line
        if (_hasPending) {
            size_t n = _curLen - 1;
            if (_pendingLen + n + 1 > kLine) n = kLine - 1 - _pendingLen;
            memcpy(_pending + _pendingLen, _cur + 1, n);
            _pendingLen += n;
        }
    } else {
        if (_hasPending) {
            _pending[_pendingLen] = 0;
            process(_pending);
        }
        memcpy(_pending, _cur, _curLen + 1);
        _pendingLen = _curLen;
        _hasPending = true;
    }
    _curLen = 0;
}

void IcsParser::beginEvent() {
    _ev = Event();
    _haveStart = _haveEnd = _cancelled = _allDay = false;
    _durationSec = 0;
}

void IcsParser::emitEvent() {
    if (_cancelled || !_haveStart) return;
    _ev.allDay = _allDay;
    _ev.start = _start;
    if (_allDay) {
        _ev.start = dayStart(_start);
        _ev.end = _haveEnd ? dayStart(_end) : _ev.start + 86400;
        if (_ev.end <= _ev.start) _ev.end = _ev.start + 86400;
    } else {
        _ev.end = _haveEnd ? _end : _start + _durationSec;
        if (_ev.end < _ev.start) _ev.end = _ev.start;
    }
    if (_sink) _sink(_ev, _ctx);
}

void IcsParser::process(const char* line) {
    // name[;params]:value  (the first ':' outside double quotes ends the name/params part)
    size_t colon = 0;
    bool inQuote = false, found = false;
    for (size_t i = 0; line[i]; i++) {
        if (line[i] == '"') inQuote = !inQuote;
        else if (line[i] == ':' && !inQuote) { colon = i; found = true; break; }
    }
    if (!found) return;
    size_t nameLen = 0;
    while (nameLen < colon && line[nameLen] != ';') nameLen++;
    const char* params = line + nameLen + (nameLen < colon ? 1 : 0);
    const size_t paramLen = colon > nameLen ? colon - nameLen - 1 : 0;
    const char* value = line + colon + 1;

    if (nameIs(line, nameLen, "BEGIN")) {
        Comp c = C_OTHER;
        if (strcasecmp(value, "VEVENT") == 0) c = C_EVENT;
        else if (strcasecmp(value, "VALARM") == 0) c = C_ALARM;
        else if (strcasecmp(value, "VTIMEZONE") == 0) c = C_TZ;
        else if (strcasecmp(value, "VCALENDAR") == 0) c = C_CAL;
        if (_depth < (int)(sizeof(_stack) / sizeof(_stack[0]))) _stack[_depth++] = c;
        if (c == C_EVENT) beginEvent();
        return;
    }
    if (nameIs(line, nameLen, "END")) {
        if (_depth > 0) {
            const Comp c = _stack[--_depth];
            if (c == C_EVENT) emitEvent();
        }
        return;
    }
    if (_depth == 0 || _stack[_depth - 1] != C_EVENT) return;   // properties of other components

    if (nameIs(line, nameLen, "DTSTART")) {
        _haveStart = toLocal(value, params, paramLen, _start, _allDay);
    } else if (nameIs(line, nameLen, "DTEND")) {
        bool dummy;
        _haveEnd = toLocal(value, params, paramLen, _end, dummy);
    } else if (nameIs(line, nameLen, "DURATION")) {
        _durationSec = parseDuration(value);
    } else if (nameIs(line, nameLen, "STATUS")) {
        if (strncasecmp(value, "CANCELLED", 9) == 0) _cancelled = true;
    } else if (nameIs(line, nameLen, "SUMMARY") || nameIs(line, nameLen, "LOCATION")) {
        char plain[kLine];
        unescape(value, plain, sizeof(plain));
        if (nameIs(line, nameLen, "SUMMARY")) foldAscii(_ev.summary, sizeof(_ev.summary), plain);
        else foldAscii(_ev.location, sizeof(_ev.location), plain);
    }
}

// ── CalDavStream ─────────────────────────────────────────────────────────

void CalDavStream::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        switch (_state) {
            case TEXT:
                if (c == '<') { _state = TAG; _tagLen = 0; }
                else if (_inData) {
                    if (c == '&') { _state = ENTITY; _entLen = 0; }
                    else _ics.feed(&c, 1);
                }
                break;
            case TAG:
                if (c == '>') { tag(); _state = TEXT; }
                else if (_tagLen + 1 < sizeof(_tag)) _tag[_tagLen++] = c;
                break;
            case ENTITY:
                if (c == ';' || _entLen + 1 >= sizeof(_ent)) {
                    _ent[_entLen] = 0;
                    char out = 0;
                    if (strcmp(_ent, "amp") == 0) out = '&';
                    else if (strcmp(_ent, "lt") == 0) out = '<';
                    else if (strcmp(_ent, "gt") == 0) out = '>';
                    else if (strcmp(_ent, "quot") == 0) out = '"';
                    else if (strcmp(_ent, "apos") == 0) out = '\'';
                    else if (_ent[0] == '#') {
                        const long v = (_ent[1] == 'x' || _ent[1] == 'X') ? strtol(_ent + 2, nullptr, 16) : strtol(_ent + 1, nullptr, 10);
                        if (v > 0 && v < 128) out = (char)v;
                    }
                    if (out) {
                        _ics.feed(&out, 1);
                    } else if (c != ';') {          // not an entity after all: pass the text through
                        const char amp = '&';
                        _ics.feed(&amp, 1);
                        _ics.feed(_ent, _entLen);
                        _ics.feed(&c, 1);
                    }
                    _state = TEXT;
                } else {
                    _ent[_entLen++] = c;
                }
                break;
        }
    }
}

void CalDavStream::tag() {
    _tag[_tagLen] = 0;
    const char* t = _tag;
    const bool closing = t[0] == '/';
    if (closing) t++;
    // element name = up to whitespace or '/', without the namespace prefix
    size_t nameEnd = 0;
    while (t[nameEnd] && t[nameEnd] != ' ' && t[nameEnd] != '\t' && t[nameEnd] != '/') nameEnd++;
    size_t nameStart = 0;
    for (size_t k = 0; k < nameEnd; k++)
        if (t[k] == ':') nameStart = k + 1;
    if (nameEnd - nameStart != 13 || strncasecmp(t + nameStart, "calendar-data", 13) != 0) return;
    const bool selfClosing = _tagLen > 0 && _tag[_tagLen - 1] == '/';
    if (closing) {
        _ics.flush();
        _inData = false;
    } else if (!selfClosing) {
        _ics.flush();
        _inData = true;
    }
}

}  // namespace ac
