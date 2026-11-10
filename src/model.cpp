// The window's state without drawing: the sprint tab, one tab per friend, then the terminal tabs that come and go, the
// active tab, and
// each friend's transcript, unsent draft and queue of prompts held for later.
// Plain C++: no OS, no clock, no input events. models/Window.tla is its specification, and
// tests/model/trace_walk.cpp checks that this code's behaviours are behaviours of that spec.

#include "model.h"

#include <algorithm>
#include <cstddef>
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

int zone(TabKind kind) {
    switch (kind) {
        case TabKind::Sprint: return 0;
        case TabKind::Friend: return 1;
        case TabKind::Terminal: return 2;
    }
    return 2;
}

// Window.tla's Init: the sprint tab, if any, at position 0 with the id after the friends', then the friends by their
// place in the list.
Model::Model(const std::vector<FriendInfo>& friends, bool sprint) {
    tabs_.reserve(friends.size() + (sprint ? 1 : 0));
    if (sprint)
        tabs_.push_back(Tab{friends.size(), {}, {}, {}, {}, {}, TabKind::Sprint});
    for (std::size_t f = 0; f < friends.size(); ++f)
        tabs_.push_back(Tab{f, friends[f].name, friends[f].model, {}, {}, {}, TabKind::Friend});
    next_id_ = friends.size() + (sprint ? 1 : 0);
    if (!tabs_.empty())
        history_.push_back(tabs_[0].id);
}

void Model::touch(std::size_t id) {
    history_.erase(std::remove(history_.begin(), history_.end(), id), history_.end());
    history_.push_back(id);
}

const std::vector<std::size_t>& Model::selection_history() const {
    return history_;
}

bool Model::friend_tab(std::size_t i) const {
    return i < tabs_.size() && tabs_[i].kind == TabKind::Friend;
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
    touch(tabs_[i].id);
    return true;
}

bool Model::set_draft(std::size_t i, std::string text) {
    if (!friend_tab(i))
        return false;
    tabs_[i].draft = std::move(text);
    return true;
}

bool Model::sendable(std::size_t i) const {
    return friend_tab(i) && !blank(tabs_[i].draft);
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
    if (!friend_tab(i))
        return false;
    tabs_[i].transcript.push_back(std::move(e));
    return true;
}

// Window.tla's MoveTab: the order is Moved (a rotation of the tabs from `from` to `to`), and active_ is MovedIndex. A
// zone's tabs stand together, so a move whose two ends are in one zone shifts only tabs of that zone.
bool Model::move_tab(std::size_t from, std::size_t to) {
    if (from >= tabs_.size() || to >= tabs_.size() || zone(tabs_[from].kind) != zone(tabs_[to].kind))
        return false;
    auto at = [this](std::size_t i) { return tabs_.begin() + static_cast<std::ptrdiff_t>(i); };
    if (from < to)
        std::rotate(at(from), at(from + 1), at(to + 1));
    else if (to < from)
        std::rotate(at(to), at(from), at(from + 1));
    if (active_ == from)
        active_ = to;
    else if (from < active_ && active_ <= to)
        --active_;
    else if (to <= active_ && active_ < from)
        ++active_;
    return true;
}

// Window.tla's AddTerminal: the terminal zone is last, so the end of the strip is in it.
std::size_t Model::add_terminal() {
    const std::size_t id = next_id_++;
    tabs_.push_back(Tab{id, {}, {}, {}, {}, {}, TabKind::Terminal});
    active_ = tabs_.size() - 1;
    touch(id);
    return id;
}

// Window.tla's CloseTab: the order is Removed, the history Forget, and active_ ClosedActive: the tab of the last id left
// in the history if the closed tab was active, else (and with no id left) ClosedIndex.
bool Model::close_tab(std::size_t i) {
    if (i >= tabs_.size() || tabs_[i].kind != TabKind::Terminal)
        return false;
    const std::size_t id = tabs_[i].id;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
    history_.erase(std::remove(history_.begin(), history_.end(), id), history_.end());
    if (active_ == i && !history_.empty()) {
        const std::size_t back = history_.back();
        const auto at = std::find_if(tabs_.begin(), tabs_.end(), [back](const Tab& t) { return t.id == back; });
        active_ = static_cast<std::size_t>(at - tabs_.begin());
    } else if (active_ > i) {
        --active_;
    } else if (active_ == i && active_ == tabs_.size()) {
        active_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    }
    return true;
}

} // namespace nexus
