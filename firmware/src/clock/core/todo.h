#pragma once
// To-do list from an Obsidian markdown file (pure, streaming, host-testable).
//
// Reads open tasks ("- [ ] text", "* [/] text"), understands the Obsidian Tasks plugin
// (due/scheduled dates, priority emojis) and Dataview inline fields ("[due:: 2026-10-15]"),
// and strips tags, links and markup from the text. Optionally only the section below one heading.
#include <stddef.h>
#include <stdint.h>

namespace ac {

struct Task {
    char text[72] = {0};       // ASCII-folded, metadata removed
    int64_t date = 0;          // due date (or scheduled date), local midnight; 0 = none
    bool hasDate = false;
    bool overdueFlag = false;  // filled by sortTasks()
    uint8_t priority = 0;      // 2 = high / highest
    uint16_t order = 0;        // position in the file
};

using TaskSink = void (*)(const Task& task, void* ctx);

class TodoParser {
public:
    // `heading`: only tasks below this heading ("Heute", "## Heute"); nullptr/"" = whole file.
    TodoParser(const char* heading, TaskSink sink, void* ctx);
    void feed(const char* data, size_t len);
    void finish();

private:
    static constexpr size_t kLine = 320;
    void line();
    void heading(const char* text, int level);
    void task(const char* text);

    TaskSink _sink;
    void* _ctx;
    char _filter[48] = {0};
    char _buf[kLine];
    size_t _len = 0;
    bool _inSection = true;
    int _sectionLevel = 0;
    uint16_t _order = 0;
};

// Display order: overdue, today, next 7 days, later, undated; inside a group by date, priority, file order.
int taskGroup(const Task& t, int64_t today);
void sortTasks(Task* tasks, int n, int64_t today);

}  // namespace ac
