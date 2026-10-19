#include "todo.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "civil.h"
#include "fold.h"

namespace ac {

namespace {

struct Marker { const char* bytes; size_t len; int kind; };
enum { M_DUE, M_SCHED, M_START, M_DONE, M_CREATED, M_CANCEL, M_RECUR, M_HIGH, M_LOW };

const Marker kMarkers[] = {
    {"\xF0\x9F\x93\x85", 4, M_DUE},       // 📅
    {"\xE2\x8F\xB3", 3, M_SCHED},         // ⏳
    {"\xF0\x9F\x9B\xAB", 4, M_START},     // 🛫
    {"\xE2\x9C\x85", 3, M_DONE},          // ✅
    {"\xE2\x9E\x95", 3, M_CREATED},       // ➕
    {"\xE2\x9D\x8C", 3, M_CANCEL},        // ❌
    {"\xF0\x9F\x94\x81", 4, M_RECUR},     // 🔁
    {"\xE2\x8F\xAB", 3, M_HIGH},          // ⏫
    {"\xF0\x9F\x94\xBA", 4, M_HIGH},      // 🔺
    {"\xF0\x9F\x94\xBC", 4, M_HIGH},      // 🔼
    {"\xF0\x9F\x94\xBD", 4, M_LOW},       // 🔽
    {"\xE2\x8F\xAC", 3, M_LOW},           // ⏬
};

const Marker* markerAt(const char* p, size_t remaining) {
    for (const Marker& m : kMarkers)
        if (remaining >= m.len && memcmp(p, m.bytes, m.len) == 0) return &m;
    return nullptr;
}

// Skips U+FE0F (variation selector) after an emoji.
size_t skipVariation(const char* p, size_t remaining) {
    return (remaining >= 3 && memcmp(p, "\xEF\xB8\x8F", 3) == 0) ? 3 : 0;
}

bool parseDate(const char* p, size_t remaining, int64_t& out) {
    while (remaining && *p == ' ') { p++; remaining--; }
    if (remaining < 10) return false;
    char d[11];
    memcpy(d, p, 10);
    d[10] = 0;
    Civil c;
    bool hasTime;
    if (!parseIsoDateTime(d, c, hasTime)) return false;
    out = toSec(c);
    return true;
}

bool startsWithNoCase(const char* p, size_t remaining, const char* word) {
    const size_t n = strlen(word);
    return remaining >= n && strncasecmp(p, word, n) == 0;
}

void squeeze(char* s) {
    char* w = s;
    bool space = true;                       // drops leading blanks
    for (char* r = s; *r; r++) {
        if (*r == ' ' || *r == '\t') {
            if (!space) *w++ = ' ';
            space = true;
        } else {
            *w++ = *r;
            space = false;
        }
    }
    if (w > s && w[-1] == ' ') w--;
    *w = 0;
}

}  // namespace

TodoParser::TodoParser(const char* heading, TaskSink sink, void* ctx) : _sink(sink), _ctx(ctx) {
    if (heading) {
        while (*heading == '#' || *heading == ' ') heading++;
        strncpy(_filter, heading, sizeof(_filter) - 1);
    }
    _inSection = _filter[0] == 0;
}

void TodoParser::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\n') line();
        else if (c != '\r' && _len + 1 < kLine) _buf[_len++] = c;
    }
}

void TodoParser::finish() {
    if (_len) line();
}

void TodoParser::heading(const char* text, int level) {
    if (_filter[0] == 0) return;
    char title[64];
    size_t n = 0;
    while (*text == ' ') text++;
    for (; *text && n + 1 < sizeof(title); text++) title[n++] = *text;
    while (n && (title[n - 1] == ' ' || title[n - 1] == '#')) n--;
    title[n] = 0;
    char folded[64];
    foldAscii(folded, sizeof(folded), title);
    char filterFolded[48];
    foldAscii(filterFolded, sizeof(filterFolded), _filter);
    if (strcasecmp(folded, filterFolded) == 0) {
        _inSection = true;
        _sectionLevel = level;
    } else if (_inSection && level <= _sectionLevel) {
        _inSection = false;
    }
}

void TodoParser::line() {
    _buf[_len] = 0;
    _len = 0;
    const char* p = _buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') {
        int level = 0;
        while (*p == '#') { level++; p++; }
        if (*p == ' ') heading(p, level);
        return;
    }
    if ((*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
        p += 2;
        while (*p == ' ') p++;
        const bool ticked = p[1] == 'x' || p[1] == 'X';
        if (p[0] == '[' && p[2] == ']' && (p[1] == ' ' || p[1] == '/' || ticked)) {
            if (_inSection) task(p + 3, ticked);
        }
    }
}

void TodoParser::task(const char* text, bool done) {
    char raw[kLine];
    size_t o = 0;
    Task t;
    t.done = done;
    int64_t dueDate = 0, schedDate = 0;
    bool haveDue = false, haveSched = false;
    const size_t total = strlen(text);
    size_t i = 0;
    bool wordStart = true;

    while (i < total && o + 4 < sizeof(raw)) {
        const char* p = text + i;
        const size_t rem = total - i;

        if (const Marker* m = markerAt(p, rem)) {
            i += m->len;
            i += skipVariation(text + i, total - i);
            int64_t d;
            switch (m->kind) {
                case M_DUE:
                    if (parseDate(text + i, total - i, d)) { dueDate = d; haveDue = true; }
                    break;
                case M_SCHED:
                    if (parseDate(text + i, total - i, d)) { schedDate = d; haveSched = true; }
                    break;
                case M_HIGH: t.priority = 2; break;
                case M_RECUR:
                    while (i < total && !markerAt(text + i, total - i)) i++;     // "every week ..." up to the next marker
                    continue;
                default: break;
            }
            // skip the date after the emoji (and the spaces before it)
            while (i < total && text[i] == ' ') i++;
            if (m->kind == M_DUE || m->kind == M_SCHED || m->kind == M_START || m->kind == M_DONE || m->kind == M_CREATED) {
                if (total - i >= 10 && isdigit((unsigned char)text[i]) && text[i + 4] == '-') i += 10;
            }
            wordStart = true;
            continue;
        }

        const char c = *p;

        // Dataview inline field: [due:: 2026-10-15] / (due:: ...) / due:: ...
        if ((c == '[' || c == '(') && (startsWithNoCase(p + 1, rem - 1, "due::") || startsWithNoCase(p + 1, rem - 1, "scheduled::") ||
                                      startsWithNoCase(p + 1, rem - 1, "start::") || startsWithNoCase(p + 1, rem - 1, "created::") ||
                                      startsWithNoCase(p + 1, rem - 1, "completion::") || startsWithNoCase(p + 1, rem - 1, "priority::"))) {
            const bool isDue = startsWithNoCase(p + 1, rem - 1, "due::");
            const bool isSched = startsWithNoCase(p + 1, rem - 1, "scheduled::");
            const char close = c == '[' ? ']' : ')';
            size_t k = i + 1;
            while (k < total && text[k] != ':') k++;
            k += 2;
            int64_t d;
            if (k < total && parseDate(text + k, total - k, d)) {
                if (isDue) { dueDate = d; haveDue = true; }
                if (isSched) { schedDate = d; haveSched = true; }
            }
            while (k < total && text[k] != close) k++;
            i = k < total ? k + 1 : total;
            continue;
        }

        // Wikilink [[Page|Alias]] -> Alias / Page
        if (c == '[' && rem > 1 && p[1] == '[') {
            size_t k = i + 2, aliasStart = i + 2;
            while (k + 1 < total && !(text[k] == ']' && text[k + 1] == ']')) {
                if (text[k] == '|') aliasStart = k + 1;
                k++;
            }
            for (size_t j = aliasStart; j < k && o + 4 < sizeof(raw); j++) raw[o++] = text[j];
            i = k + 2 <= total ? k + 2 : total;
            wordStart = false;
            continue;
        }

        // Markdown link [text](url) -> text
        if (c == '[') {
            size_t k = i + 1;
            while (k < total && text[k] != ']') k++;
            if (k + 1 < total && text[k + 1] == '(') {
                size_t e = k + 2;
                while (e < total && text[e] != ')') e++;
                for (size_t j = i + 1; j < k && o + 4 < sizeof(raw); j++) raw[o++] = text[j];
                i = e < total ? e + 1 : total;
                wordStart = false;
                continue;
            }
        }

        // #tag (only at the start of a word)
        if (c == '#' && wordStart && rem > 1 && (isalnum((unsigned char)p[1]) || p[1] == '_' || p[1] == '/' || p[1] == '-' || (unsigned char)p[1] >= 0x80)) {
            while (i < total && text[i] != ' ') i++;
            continue;
        }

        // block id "^abc123" at the end
        if (c == '^' && wordStart) {
            size_t k = i + 1;
            while (k < total && (isalnum((unsigned char)text[k]) || text[k] == '-')) k++;
            if (k == total && k > i + 1) { i = total; continue; }
        }

        // emphasis markers
        if ((c == '*' && rem > 1 && p[1] == '*') || (c == '_' && rem > 1 && p[1] == '_') || c == '`' || (c == '=' && rem > 1 && p[1] == '=') ||
            (c == '~' && rem > 1 && p[1] == '~')) {
            i += (c == '`') ? 1 : 2;
            continue;
        }

        raw[o++] = c;
        wordStart = (c == ' ');
        i++;
    }
    raw[o] = 0;
    squeeze(raw);
    foldAscii(t.text, sizeof(t.text), raw);
    squeeze(t.text);
    if (t.text[0] == 0) return;
    if (haveDue) { t.date = dueDate; t.hasDate = true; }
    else if (haveSched) { t.date = schedDate; t.hasDate = true; }
    t.order = _order++;
    if (_sink) _sink(t, _ctx);
}

int taskGroup(const Task& t, int64_t today) {
    if (!t.hasDate) return 4;
    if (t.date < today) return 0;
    if (t.date == today) return 1;
    if (t.date <= today + 7 * 86400) return 2;
    return 3;
}

void sortTasks(Task* tasks, int n, int64_t today) {
    for (int i = 0; i < n; i++) tasks[i].overdueFlag = !tasks[i].done && tasks[i].hasDate && tasks[i].date < today;
    auto before = [today](const Task& a, const Task& b) {
        const int ga = taskGroup(a, today), gb = taskGroup(b, today);
        if (ga != gb) return ga < gb;
        if (a.hasDate && b.hasDate && a.date != b.date) return a.date < b.date;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.order < b.order;
    };
    // Ticked tasks keep their place in the list (as in the file); the open ones are sorted on the other places.
    int slot[64];
    int m = 0;
    for (int i = 0; i < n && m < 64; i++)
        if (!tasks[i].done) slot[m++] = i;
    for (int a = 1; a < m; a++) {                 // insertion sort over the open slots: small n, stable
        Task key = tasks[slot[a]];
        int b = a - 1;
        while (b >= 0 && before(key, tasks[slot[b]])) { tasks[slot[b + 1]] = tasks[slot[b]]; b--; }
        tasks[slot[b + 1]] = key;
    }
}

}  // namespace ac
