#pragma once
// Streaming iCalendar (RFC 5545) and CalDAV multistatus parser (pure, no heap, host-testable).
//
// CalDavStream takes the raw HTTP body of a calendar-query REPORT (XML with the iCalendar text in
// <c:calendar-data>) in arbitrary chunks and reports every VEVENT. Recurrences are not expanded
// here: the server does that (<c:expand> in the request). Times become "local seconds"
// (see civil.h): UTC values are converted to Europe/Berlin, floating/TZID values are taken as is.
// Texts are folded to ASCII (fold.h) because the e-ink fonts have no umlauts.
#include <stddef.h>
#include <stdint.h>

namespace ac {

struct Event {
    int64_t start = 0;          // local seconds
    int64_t end = 0;            // exclusive; all-day events: midnight of the day after the last day
    bool allDay = false;
    char summary[56] = {0};
    char location[32] = {0};
};

using EventSink = void (*)(const Event& ev, void* ctx);

class IcsParser {
public:
    IcsParser(EventSink sink, void* ctx) : _sink(sink), _ctx(ctx) {}
    void feed(const char* data, size_t len);   // iCalendar text, any chunking
    void flush();                              // end of an object / of the stream

private:
    static constexpr size_t kLine = 400;
    enum Comp : uint8_t { C_CAL, C_EVENT, C_ALARM, C_TZ, C_OTHER };

    void endPhysical();
    void process(const char* line);
    void beginEvent();
    void emitEvent();

    EventSink _sink;
    void* _ctx;
    char _cur[kLine];
    size_t _curLen = 0;
    char _pending[kLine];
    size_t _pendingLen = 0;
    bool _hasPending = false;
    Comp _stack[6];
    int _depth = 0;

    Event _ev;
    bool _haveStart = false, _haveEnd = false, _cancelled = false;
    int64_t _durationSec = 0;
    int64_t _start = 0, _end = 0;
    bool _allDay = false;
};

class CalDavStream {
public:
    CalDavStream(EventSink sink, void* ctx) : _ics(sink, ctx) {}
    void feed(const char* data, size_t len);
    void finish() { _ics.flush(); }

private:
    enum State : uint8_t { TEXT, TAG, ENTITY };
    void tag();

    IcsParser _ics;
    State _state = TEXT;
    bool _inData = false;
    char _tag[64];
    size_t _tagLen = 0;
    char _ent[12];
    size_t _entLen = 0;
};

}  // namespace ac
