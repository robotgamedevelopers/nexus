// The window's state without drawing: one tab per friend, the active tab, each tab's transcript, unsent draft and
// queue of prompts held for later.
// Plain C++: no OS, no clock, no input events. models/Window.tla is its specification, and
// tests/model/trace_walk.cpp checks that this code's behaviours are behaviours of that spec.

#include "model.h"

#include <utility>

namespace nexus {

namespace {

// All-whitespace means every byte is an ASCII space, tab, newline, vertical tab, form feed or carriage return.
// The empty string is all-whitespace.
bool blank(const std::string& text) {
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' && c != '\r')
            return false;
    }
    return true;
}

} // namespace

Model::Model(const std::vector<FriendInfo>& friends) {
    tabs_.reserve(friends.size());
    for (const FriendInfo& f : friends)
        tabs_.push_back(Tab{f.name, f.model, {}, {}, {}});
}

std::size_t Model::tab_count() const {
    return tabs_.size();
}

std::size_t Model::active() const {
    return active_;
}

const Tab& Model::tab(std::size_t i) const {
    return tabs_.at(i);
}

bool Model::select_tab(std::size_t i) {
    if (i >= tabs_.size())
        return false;
    active_ = i;
    return true;
}

bool Model::set_draft(std::size_t i, std::string text) {
    if (i >= tabs_.size())
        return false;
    tabs_[i].draft = std::move(text);
    return true;
}

bool Model::sendable(std::size_t i) const {
    return i < tabs_.size() && !blank(tabs_[i].draft);
}

bool Model::send(std::size_t i) {
    if (!sendable(i))
        return false;
    Tab& t = tabs_[i];
    t.transcript.push_back(Entry{EntryKind::Person, std::move(t.draft)});
    t.draft.clear();
    return true;
}

bool Model::queue(std::size_t i) {
    if (!sendable(i))
        return false;
    Tab& t = tabs_[i];
    t.queue.push_back(std::move(t.draft));
    t.draft.clear();
    return true;
}

bool Model::append(std::size_t i, Entry e) {
    if (i >= tabs_.size())
        return false;
    tabs_[i].transcript.push_back(std::move(e));
    return true;
}

} // namespace nexus
