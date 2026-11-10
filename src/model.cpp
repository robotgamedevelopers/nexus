#include "model.h"

namespace nexus {

namespace {

bool isContinuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

} // namespace

Model makeModel(const std::vector<Friend>& friends) {
    Model model;
    for (const Friend& f : friends)
        model.tabs.push_back(Tab{.who = f, .transcript = {}, .prompt = {}, .caret = 0});
    return model;
}

bool selectTab(Model& model, size_t index) {
    if (index >= model.tabs.size() || index == model.active)
        return false;
    model.active = index;
    return true;
}

Tab& activeTab(Model& model) {
    return model.tabs[model.active];
}

const Tab& activeTab(const Model& model) {
    return model.tabs[model.active];
}

void insertText(Model& model, const std::string& utf8) {
    Tab& tab = activeTab(model);
    tab.prompt.insert(tab.caret, utf8);
    tab.caret += utf8.size();
}

void backspace(Model& model) {
    Tab& tab = activeTab(model);
    if (tab.caret == 0)
        return;
    size_t start = tab.caret - 1;
    while (start > 0 && isContinuation(static_cast<unsigned char>(tab.prompt[start])))
        --start;
    tab.prompt.erase(start, tab.caret - start);
    tab.caret = start;
}

void caretLeft(Model& model) {
    Tab& tab = activeTab(model);
    if (tab.caret == 0)
        return;
    --tab.caret;
    while (tab.caret > 0 && isContinuation(static_cast<unsigned char>(tab.prompt[tab.caret])))
        --tab.caret;
}

void caretRight(Model& model) {
    Tab& tab = activeTab(model);
    if (tab.caret >= tab.prompt.size())
        return;
    ++tab.caret;
    while (tab.caret < tab.prompt.size() && isContinuation(static_cast<unsigned char>(tab.prompt[tab.caret])))
        ++tab.caret;
}

bool submit(Model& model, bool queue) {
    Tab& tab = activeTab(model);
    if (tab.prompt.empty())
        return false;
    Entry entry;
    entry.kind = EntryKind::Person;
    entry.text = tab.prompt;
    entry.queued = queue;
    tab.transcript.push_back(entry);
    tab.prompt.clear();
    tab.caret = 0;
    return true;
}

void seedSample(Tab& tab) {
    auto add = [&](EntryKind kind, const char* text) {
        Entry e;
        e.kind = kind;
        e.text = text;
        tab.transcript.push_back(e);
    };
    add(EntryKind::Person, "Can you set up the repo for nexus?");
    add(EntryKind::Ai, "Sure. I'll check who can create repos in the org first, then make it private and report back.");
    add(EntryKind::ToolSummary, "Read org-settings.md, ran 1 command");
    add(EntryKind::Ai, "Only org owners can create repos there. I'm not an owner, so I'll need you to create it.");
    tab.transcript.back().lead = "What I found.";
    add(EntryKind::Person, "Done, it's created. Go ahead.");
    add(EntryKind::ToolSummary, "Read not-org-admin-by-design.md, ran 2 commands, 1 failed");
    add(EntryKind::Ai, "Thanks. The first push was refused because the repo was still empty on my side; retrying on main now.");
    Entry card;
    card.kind = EntryKind::TaskList;
    card.tasks = {
        Task{"Check who can create the repo", TaskState::Done},
        Task{"Create private nexus repo", TaskState::InProgress},
        Task{"Report", TaskState::Pending},
    };
    tab.transcript.push_back(card);
    add(EntryKind::Status, "29s \xC2\xB7 225 tokens");
}

} // namespace nexus
