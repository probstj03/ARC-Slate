#pragma once
// A markdown document shown 1:1 on the e-ink (pure, streaming, host-testable): blank lines, headings, tasks (open and
// ticked), bullets and text lines in the order of the file. Left out: the YAML front matter (--- ... ---) and the first
// heading (the title; the e-ink has its own heading). Inline markup is removed (wiki links, links, bold, html tags,
// code ticks); the texts are Latin-1 (fold.h), long lines are cut.
#include <stddef.h>
#include <stdint.h>

namespace ac {

enum class DocKind : uint8_t { Blank, H1, H2, H3, TaskOpen, TaskDone, Bullet, Text, Rule };

struct DocLine {
    DocKind kind = DocKind::Blank;
    uint8_t indent = 0;        // nesting level 0..3 (two spaces each)
    char text[76] = {0};
};

constexpr int kMaxDocLines = 64;

using DocSink = void (*)(const DocLine& line, void* ctx);

class DocParser {
public:
    DocParser(DocSink sink, void* ctx) : _sink(sink), _ctx(ctx) {}
    void feed(const char* data, size_t len);
    void finish();             // the last line without a line break

private:
    void line();
    void emit(DocLine& l);

    DocSink _sink;
    void* _ctx;
    char _buf[200];
    size_t _len = 0;
    int _lineNo = 0;
    bool _front = false;       // inside the front matter
    bool _titleDone = false;   // the first heading was left out
    bool _started = false;     // a content line was emitted: blank lines before the first one are dropped
    int _blanks = 0;           // blank lines waiting for the next content line (none at the end of the document)
};

}  // namespace ac
