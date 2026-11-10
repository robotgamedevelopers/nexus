// The window's state without drawing: tabs, one per friend, each with its transcript and its unsent prompt text.
// Plain C++: no OS, no clock, no input events. The view reads it; the host turns key presses into these calls.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nexus {

enum class TaskState { Done, InProgress, Pending };

struct Task {
    std::string text;
    TaskState state = TaskState::Pending;
};

enum class EntryKind {
    Person,      // the person's message, a bubble on the right
    Ai,          // the friend's text, plain on the left
    ToolSummary, // a collapsed line of what the friend did
    TaskList,    // a card of tasks
    Status,      // elapsed time and tokens under the newest turn
};

struct Entry {
    EntryKind kind = EntryKind::Ai;
    std::string text;
    bool queued = false;       // Person: waits to be sent later
    std::vector<Task> tasks;   // TaskList
};

struct Friend {
    std::string name;
    std::string model;
};

struct Tab {
    Friend who;
    std::vector<Entry> transcript;
    std::string prompt;        // UTF-8
    size_t caret = 0;          // byte offset into prompt, on a code point boundary
};

struct Model {
    std::vector<Tab> tabs;
    size_t active = 0;
};

// Makes one tab per friend, all empty.
Model makeModel(const std::vector<Friend>& friends);

// Selects the tab at `index` (0-based). Out of range does nothing. Returns true if it changed.
bool selectTab(Model& model, size_t index);

Tab& activeTab(Model& model);
const Tab& activeTab(const Model& model);

// Prompt editing on the active tab.
void insertText(Model& model, const std::string& utf8);
void backspace(Model& model);
void caretLeft(Model& model);
void caretRight(Model& model);

// Appends the prompt as a person entry (queued if `queue`) and clears it. Empty prompts do nothing. Returns true if
// something was appended.
bool submit(Model& model, bool queue);

// Fills a tab with sample content that shows every kind of entry.
void seedSample(Tab& tab);

} // namespace nexus
