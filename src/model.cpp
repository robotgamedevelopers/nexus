// The window's stand-in for the model: the simplest code that keeps the frozen interface in model.h, so the window
// builds. The model worker's model.cpp replaces this file.
//
// The frozen header declares no data members, so the state of each Model lives in a table keyed by its address. A Model
// is therefore never copied or moved, and its entry stays for the life of the process (the window makes one).

#include "model.h"

#include <unordered_map>

namespace nexus {

namespace {

struct State {
    std::vector<Tab> tabs;
    std::size_t active = 0;
};

std::unordered_map<const Model*, State>& states() {
    static std::unordered_map<const Model*, State> table;
    return table;
}

State& of(const Model* model) {
    return states()[model];
}

bool blank(const std::string& text) {
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v')
            return false;
    }
    return true;
}

bool post(const Model* model, std::size_t i, EntryKind kind) {
    State& s = of(model);
    if (i >= s.tabs.size() || blank(s.tabs[i].draft))
        return false;
    s.tabs[i].transcript.push_back(Entry{kind, std::move(s.tabs[i].draft)});
    s.tabs[i].draft.clear();
    return true;
}

} // namespace

Model::Model(const std::vector<FriendInfo>& friends) {
    State& s = of(this);
    s = State{};
    for (const FriendInfo& f : friends)
        s.tabs.push_back(Tab{f.name, f.model, {}, {}});
}

std::size_t Model::tab_count() const {
    return of(this).tabs.size();
}

std::size_t Model::active() const {
    return of(this).active;
}

const Tab& Model::tab(std::size_t i) const {
    return of(this).tabs[i];
}

bool Model::select_tab(std::size_t i) {
    State& s = of(this);
    if (i >= s.tabs.size())
        return false;
    s.active = i;
    return true;
}

bool Model::set_draft(std::size_t i, std::string text) {
    State& s = of(this);
    if (i >= s.tabs.size())
        return false;
    s.tabs[i].draft = std::move(text);
    return true;
}

bool Model::send(std::size_t i) {
    return post(this, i, EntryKind::Person);
}

bool Model::queue(std::size_t i) {
    return post(this, i, EntryKind::Queued);
}

bool Model::append(std::size_t i, Entry e) {
    State& s = of(this);
    if (i >= s.tabs.size())
        return false;
    s.tabs[i].transcript.push_back(std::move(e));
    return true;
}

} // namespace nexus
