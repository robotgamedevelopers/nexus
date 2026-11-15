// The window's state without drawing: the sprint tab, one tab per friend (friends come and go with the server's friend
// list), then the terminal tabs that come and go, the active tab, and each friend's transcript, unsent draft and queue
// of prompts held for later.
// Plain C++: no OS, no clock, no input events. models/Window.tla is its specification, and
// tests/model/trace_walk.cpp checks that this code's behaviours are behaviours of that spec.

#include "model.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <unordered_set>
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

bool Model::take_queued(std::size_t i) {
    if (!friend_tab(i) || tabs_[i].queue.empty()) {
        return false;
    }
    tabs_[i].queue.erase(tabs_[i].queue.begin());
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
    remove_at(i);
    return true;
}

// Window.tla's AddFriend: the tab goes at FriendEnd, the first position past the sprint tab and the friends; active_
// moves with the tab it names when that stood at or past it.
bool Model::add_friend(FriendInfo f) {
    if (f.name.empty()) {
        return false;
    }
    for (const Tab& t : tabs_) {
        if (t.kind == TabKind::Friend && t.name == f.name) {
            return false;
        }
    }
    std::size_t at = 0;
    while (at < tabs_.size() && zone(tabs_[at].kind) <= zone(TabKind::Friend)) {
        ++at;
    }
    const std::size_t id = next_id_++;
    const bool empty = tabs_.empty();
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at),
                 Tab{id, std::move(f.name), std::move(f.model), {}, {}, {}, TabKind::Friend});
    if (empty) {
        history_.push_back(id);
    } else if (active_ >= at) {
        ++active_;
    }
    return true;
}

bool Model::remove_friend(std::size_t i) {
    if (!friend_tab(i)) {
        return false;
    }
    remove_at(i);
    return true;
}

// add_friend of each in turn: every tab goes at the friends' zone's end, so the new tabs stand together there in the
// order given, and the active tab moves past them when it stood at or past that end. A model with no tabs makes the first
// of them active and its id the history, and add_friend of the rest leaves both as they are.
std::vector<std::string> Model::add_friends(std::vector<FriendInfo> friends, std::size_t* atOut) {
    std::unordered_set<std::string> named;
    for (const Tab& t : tabs_) {
        if (t.kind == TabKind::Friend) {
            named.insert(t.name);
        }
    }
    std::size_t at = tabs_.size();
    while (at > 0 && zone(tabs_[at - 1].kind) > zone(TabKind::Friend)) {
        --at;
    }
    if (atOut != nullptr) {
        *atOut = at;
    }
    std::vector<Tab> fresh;
    std::vector<std::string> added;
    for (FriendInfo& f : friends) {
        if (f.name.empty() || !named.insert(f.name).second) {
            continue;
        }
        added.push_back(f.name);
        fresh.push_back(Tab{next_id_++, std::move(f.name), std::move(f.model), {}, {}, {}, TabKind::Friend});
    }
    if (fresh.empty()) {
        return added;
    }
    const bool empty = tabs_.empty();
    const std::size_t count = fresh.size();
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(fresh.begin()),
                 std::make_move_iterator(fresh.end()));
    if (empty) {
        history_.push_back(tabs_[0].id);
    } else if (active_ >= at) {
        active_ += count;
    }
    return added;
}

// remove_friend of each, last first. A tab that is not active leaves only its place and its id in the history. When
// the active tab goes, remove_friend makes the tab of the history's last id active, and when that one goes too the
// next: so, while any id of the history stays, the tab of the last id that stays is active at the end. When every id of
// the history goes, remove_friend's fallback (the tab at the removed one's place) decides, step by step, and so the
// loop itself is run.
std::vector<std::size_t> Model::remove_friends(const std::function<bool(const Tab&)>& gone) {
    std::vector<std::size_t> removed;
    std::vector<char> drop(tabs_.size(), 0);
    std::unordered_set<std::size_t> ids;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].kind == TabKind::Friend && gone(tabs_[i])) {
            drop[i] = 1;
            ids.insert(tabs_[i].id);
            removed.push_back(tabs_[i].id);
        }
    }
    if (removed.empty()) {
        return removed;
    }
    const bool historyStays = std::any_of(history_.begin(), history_.end(),
        [&ids](std::size_t id) { return ids.count(id) == 0; });
    if (!historyStays) {
        for (std::size_t i = tabs_.size(); i-- > 0;) {
            if (drop[i] != 0) {
                remove_at(i);
            }
        }
        return removed;
    }
    const std::size_t activeId = tabs_[active_].id;
    history_.erase(std::remove_if(history_.begin(), history_.end(),
        [&ids](std::size_t id) { return ids.count(id) != 0; }), history_.end());
    std::size_t kept = 0;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (drop[i] == 0) {
            if (kept != i) {
                tabs_[kept] = std::move(tabs_[i]);
            }
            ++kept;
        }
    }
    tabs_.resize(kept);
    const std::size_t shows = ids.count(activeId) != 0 ? history_.back() : activeId;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].id == shows) {
            active_ = i;
            break;
        }
    }
    return removed;
}

void Model::remove_at(std::size_t i) {
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
    // A history left empty with tabs open (the closed tab was the only one ever selected, and the rest arrived without
    // a selection, as a friend's tab does) holds the tab that now shows, so that it still ends at the active tab
    // (Window.tla, ClosedHistory).
    if (history_.empty() && !tabs_.empty()) {
        history_.push_back(tabs_[active_].id);
    }
}

} // namespace nexus
