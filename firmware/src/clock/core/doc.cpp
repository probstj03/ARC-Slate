#include "doc.h"
#include <ctype.h>
#include <string.h>
#include "fold.h"

namespace ac {

namespace {

// Removes inline markup from in[0..n) and writes the folded (Latin-1) result.
void cleanInline(const char* in, size_t n, char* out, size_t cap) {
    char tmp[200];
    size_t o = 0;
    auto put = [&](char c) { if (o + 1 < sizeof(tmp)) tmp[o++] = c; };
    for (size_t i = 0; i < n;) {
        const char c = in[i];
        // wiki link / embed: [[Page|Alias]] -> Alias, ![[Page]] -> Page
        const bool embed = c == '!' && i + 2 < n && in[i + 1] == '[' && in[i + 2] == '[';
        if (embed || (c == '[' && i + 1 < n && in[i + 1] == '[')) {
            size_t k = i + (embed ? 3 : 2), alias = k;
            while (k + 1 < n && !(in[k] == ']' && in[k + 1] == ']')) {
                if (in[k] == '|') alias = k + 1;
                k++;
            }
            for (size_t j = alias; j < k; j++) put(in[j]);
            i = k + 1 < n ? k + 2 : n;
            continue;
        }
        // link / image: [text](url) -> text
        if (c == '[' || (c == '!' && i + 1 < n && in[i + 1] == '[')) {
            const size_t b = i + (c == '!' ? 2 : 1);
            size_t k = b;
            while (k < n && in[k] != ']') k++;
            if (k + 1 < n && in[k + 1] == '(') {
                size_t p = k + 2;
                while (p < n && in[p] != ')') p++;
                if (p < n) {
                    for (size_t j = b; j < k; j++) put(in[j]);
                    i = p + 1;
                    continue;
                }
            }
        }
        // html tag
        if (c == '<' && i + 1 < n && (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '/')) {
            size_t k = i + 1;
            while (k < n && in[k] != '>') k++;
            if (k < n) { i = k + 1; continue; }
        }
        // bold / underline / strike / highlight markers and code ticks
        if (i + 1 < n && in[i + 1] == c && (c == '*' || c == '_' || c == '~' || c == '=')) { i += 2; continue; }
        if (c == '`') { i++; continue; }
        put(c);
        i++;
    }
    // trailing block id " ^abc123"
    while (o > 0 && tmp[o - 1] == ' ') o--;
    size_t k = o;
    while (k > 0 && (isalnum((unsigned char)tmp[k - 1]) || tmp[k - 1] == '-')) k--;
    if (k >= 2 && k < o && tmp[k - 1] == '^' && tmp[k - 2] == ' ') o = k - 2;
    tmp[o] = 0;
    if (cap) out[0] = 0;
    foldAppend(out, cap, 0, tmp, o);
}

}  // namespace

void DocParser::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\n') line();
        else if (_len + 1 < sizeof(_buf)) _buf[_len++] = c;        // longer lines are cut
    }
}

void DocParser::finish() {
    if (_len) line();
}

void DocParser::emit(DocLine& l) {
    for (; _started && _blanks > 0; _blanks--) {
        DocLine b;
        b.kind = DocKind::Blank;
        if (_sink) _sink(b, _ctx);
    }
    _blanks = 0;
    _started = true;
    if (_sink) _sink(l, _ctx);
}

void DocParser::line() {
    size_t n = _len;
    _len = 0;
    while (n && (_buf[n - 1] == ' ' || _buf[n - 1] == '\t' || _buf[n - 1] == '\r')) n--;
    _buf[n] = 0;
    const char* p = _buf;
    const int no = _lineNo++;

    // YAML front matter at the very top
    if (no == 0 && strcmp(p, "---") == 0) { _front = true; return; }
    if (_front) {
        if (strcmp(p, "---") == 0 || strcmp(p, "...") == 0) _front = false;
        return;
    }
    if (n == 0) {                                   // blank: kept, but not before the first line and not at the end
        if (_started && _blanks < 4) _blanks++;
        return;
    }

    int spaces = 0;
    while (*p == ' ' || *p == '\t') { spaces += *p == '\t' ? 4 : 1; p++; }
    DocLine l;
    l.indent = (uint8_t)(spaces / 2 > 3 ? 3 : spaces / 2);
    const char* text = p;

    if (*p == '#') {
        int level = 0;
        const char* q = p;
        while (*q == '#') { level++; q++; }
        if (level <= 6 && *q == ' ') {
            if (level == 1 && !_started && !_titleDone) { _titleDone = true; return; }   // the title
            l.kind = level == 1 ? DocKind::H1 : level == 2 ? DocKind::H2 : DocKind::H3;
            l.indent = 0;
            text = q + 1;
            cleanInline(text, strlen(text), l.text, sizeof(l.text));
            emit(l);
            return;
        }
    }
    if (strcmp(p, "---") == 0 || strcmp(p, "***") == 0 || strcmp(p, "___") == 0) {
        l.kind = DocKind::Rule;
        emit(l);
        return;
    }
    if ((*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
        const char* q = p + 2;
        while (*q == ' ') q++;
        if (q[0] == '[' && q[2] == ']' && (q[3] == ' ' || q[3] == 0)) {
            const char m = q[1];
            if (m == ' ' || m == '/') l.kind = DocKind::TaskOpen;
            else if (m == 'x' || m == 'X' || m == '-') l.kind = DocKind::TaskDone;
            else l.kind = DocKind::Bullet;
            text = l.kind == DocKind::Bullet ? q : q + 3;
        } else {
            l.kind = DocKind::Bullet;
            text = q;
        }
    } else {
        l.kind = DocKind::Text;
    }
    while (*text == ' ') text++;
    cleanInline(text, strlen(text), l.text, sizeof(l.text));
    emit(l);
}

}  // namespace ac
