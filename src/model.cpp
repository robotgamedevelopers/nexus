// The window's state without drawing: the sprint tab, one tab per friend (friends come and go with the server's friend
// list), then the terminal tabs that come and go, the active tab, and each friend's transcript, unsent draft and queue
// of prompts held for later.
// Plain C++: no OS, no clock, no input events. models/Window.tla is its specification, and
// tests/model/trace_walk.cpp checks that this code's behaviours are behaviours of that spec.

#include "model.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <unordered_map>
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
        case TabKind::Extension: return 3;
    }
    return 2;
}

// Window.tla's Init: the sprint tab, if any, at position 0 with the id after the friends', then the friends by their
// place in the list.
Model::Model(const std::vector<FriendInfo>& friends, bool sprint) {
    tabs_.reserve(friends.size() + (sprint ? 1 : 0));
    if (sprint)
        tabs_.push_back(Tab{friends.size(), {}, {}, {}, {}, {}, TabKind::Sprint, {}, home_group(TabKind::Sprint)});
    for (std::size_t f = 0; f < friends.size(); ++f)
        tabs_.push_back(Tab{f, friends[f].name, friends[f].model, {}, {}, {}, TabKind::Friend, {}, home_group(TabKind::Friend)});
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

bool Model::hold_draft(std::size_t i) {
    if (!sendable(i)) return false;
    auto& tab = tabs_[i];
    if (!tab.held.empty()) tab.held += '\n';
    tab.held += tab.draft;
    tab.draft.clear();
    return true;
}

void Model::clear_composer(std::size_t i) {
    if (!friend_tab(i)) return;
    tabs_[i].held.clear();
    tabs_[i].draft.clear();
}

void Model::restore_held(std::size_t i, std::string text) {
    if (!friend_tab(i)) return;
    auto& held = tabs_[i].held;
    held = held.empty() ? std::move(text) : text + "\n" + held;
}

void Model::pop_held(std::size_t i) {
    if (friend_tab(i)) tabs_[i].held.clear();
}

bool Model::take_queued(std::size_t i) {
    if (!friend_tab(i) || tabs_[i].queue.empty()) {
        return false;
    }
    tabs_[i].queue.erase(tabs_[i].queue.begin());
    return true;
}

void Model::clear_transcript(std::size_t i) {
    if (!friend_tab(i)) return;
    tabs_[i].held.clear();
    clear_output(i);
}

void Model::clear_output(std::size_t i) {
    if (!friend_tab(i)) return;
    tabs_[i].transcript.clear();
    ++tabs_[i].transcriptGeneration;
}

bool Model::append(std::size_t i, Entry e) {
    if (!friend_tab(i))
        return false;
    tabs_[i].transcript.push_back(std::move(e));
    return true;
}

void Model::edit_transcript(std::size_t i, std::vector<TranscriptEdit> edits) {
    if (!friend_tab(i) || edits.empty()) return;
    auto& tab = tabs_[i];
    auto& old = tab.transcript;
    std::vector<Entry> updated;
    std::size_t count = old.size();
    for (const auto& edit : edits) {
        if (edit.erase) --count;
        if (edit.replacement) ++count;
    }
    updated.reserve(count);
    auto next = old.begin();
    for (auto& edit : edits) {
        const auto at = old.begin() + static_cast<std::ptrdiff_t>(edit.at);
        std::move(next, at, std::back_inserter(updated));
        if (edit.removed) *edit.removed = std::move(*at);
        if (edit.replacement) updated.push_back(std::move(*edit.replacement));
        next = at + (edit.erase ? 1 : 0);
    }
    std::move(next, old.end(), std::back_inserter(updated));
    old = std::move(updated);
    ++tab.transcriptGeneration;
}

// The fork assigns logical tab types to groups. Older descriptions retain the three built-in zones and
// extension groups in the order they are first opened.
std::string Model::home_group(TabKind kind, const std::string& type, const std::string& declared) const {
    const std::string_view key = kind == TabKind::Extension ? std::string_view(type) :
        kind == TabKind::Friend ? "friend" : kind == TabKind::Sprint ? "sprint" : "terminal";
    for (const TabGroup& g : groups_) {
        if (std::find(g.types.begin(), g.types.end(), key) != g.types.end())
            return g.id;
    }
    return kind == TabKind::Extension ? declared : legacyGroups_[static_cast<std::size_t>(zone(kind))];
}

std::size_t Model::group_rank(const std::string& group) const {
    for (std::size_t i = 0; i < groups_.size(); ++i)
        if (groups_[i].id == group) return i;
    const auto it = std::find(legacyGroups_.begin(), legacyGroups_.end(), group);
    return groups_.size() + static_cast<std::size_t>(it - legacyGroups_.begin());
}

std::size_t Model::group_end(const std::string& group) const {
    const auto rank = group_rank(group);
    auto end = std::find_if(tabs_.rbegin(), tabs_.rend(), [&](const Tab& t) { return group_rank(t.group) <= rank; });
    return static_cast<std::size_t>(end.base() - tabs_.begin());
}

bool Model::accepts(const std::string& from, const std::string& to) const {
    if (from == to) return true;
    for (const TabGroup& g : groups_) {
        if (g.id == to)
            return std::find(g.acceptsFrom.begin(), g.acceptsFrom.end(), from) != g.acceptsFrom.end();
    }
    return false;
}

// A saved instance may have reached its destination in several authorized moves. Validate that the current fork
// still admits a path from its type's home; a single interactive move always checks the actual edge.
bool Model::reachable(const std::string& from, const std::string& to) const {
    if (accepts(from, to)) return true;
    std::unordered_set<std::string> seen{from};
    std::vector<std::string> pending{from};
    for (std::size_t i = 0; i < pending.size(); ++i) {
        for (const TabGroup& g : groups_) {
            if (seen.count(g.id) ||
                std::find(g.acceptsFrom.begin(), g.acceptsFrom.end(), pending[i]) == g.acceptsFrom.end()) continue;
            if (g.id == to) return true;
            seen.insert(g.id);
            pending.push_back(g.id);
        }
    }
    return false;
}

bool Model::set_tab_groups(const std::vector<TabGroup>& groups) {
    if (groups_ == groups) return false;
    // Keep authorized instance overrides, and recalculate every other tab from its type.
    std::vector<bool> overrides;
    overrides.reserve(tabs_.size());
    for (const Tab& t : tabs_)
        overrides.push_back(t.group != home_group(t.kind, t.type, t.declaredGroup));
    ++layoutRevision_;
    groups_ = groups;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        Tab& t = tabs_[i];
        auto home = home_group(t.kind, t.type, t.declaredGroup);
        if (!overrides[i] || !reachable(home, t.group)) t.group = std::move(home);
    }
    sort_layout({});
    return true;
}

void Model::restore_tab_layout(const std::vector<std::pair<std::size_t, std::string>>& ordered) {
    std::vector<std::pair<std::size_t, std::string>> before;
    for (const Tab& t : tabs_) before.emplace_back(t.id, t.group);
    std::unordered_map<std::size_t, std::string> requested;
    for (const auto& item : ordered) requested.insert(item);
    for (Tab& t : tabs_) {
        const auto it = requested.find(t.id);
        if (it == requested.end()) continue;
        const auto home = home_group(t.kind, t.type, t.declaredGroup);
        t.group = reachable(home, it->second) ? it->second : home;
    }
    sort_layout(ordered);
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (before[i].first != tabs_[i].id || before[i].second != tabs_[i].group) {
            ++layoutRevision_;
            break;
        }
    }
}

void Model::sort_layout(const std::vector<std::pair<std::size_t, std::string>>& ordered) {
    if (tabs_.empty()) return;
    const auto activeId = tabs_[active_].id;
    std::unordered_map<std::size_t, std::size_t> saved;
    for (std::size_t i = 0; i < ordered.size(); ++i) saved.try_emplace(ordered[i].first, i);
    const auto position = [&](const Tab& t) {
        const auto it = saved.find(t.id);
        return std::pair{group_rank(t.group), it == saved.end() ? ordered.size() : it->second};
    };
    std::stable_sort(tabs_.begin(), tabs_.end(), [&](const Tab& a, const Tab& b) { return position(a) < position(b); });
    const auto active = std::find_if(tabs_.begin(), tabs_.end(), [activeId](const Tab& t) { return t.id == activeId; });
    active_ = static_cast<std::size_t>(active - tabs_.begin());
}

bool Model::move_tab(std::size_t from, std::size_t to) {
    if (from >= tabs_.size() || to >= tabs_.size() || !accepts(tabs_[from].group, tabs_[to].group))
        return false;
    if (from != to) ++layoutRevision_;
    tabs_[from].group = tabs_[to].group;
    return reorder(from, to);
}

// A Prax drop names an insertion slot in the destination strip, including the slot past its last tab.
bool Model::move_to_group(std::size_t from, const std::string& group, std::size_t slot) {
    if (from >= tabs_.size() || tabs_[from].group == group || !accepts(tabs_[from].group, group))
        return false;
    const auto first = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& t) { return t.group == group; });
    if (first == tabs_.end()) return false;
    const auto start = static_cast<std::size_t>(first - tabs_.begin());
    const auto dest = start + std::min(slot, group_end(group) - start);
    ++layoutRevision_;
    tabs_[from].group = group;
    return reorder(from, from < dest ? dest - 1 : dest);
}

bool Model::reorder(std::size_t from, std::size_t to) {
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
    ++layoutRevision_;
    const std::size_t id = next_id_++;
    const auto group = home_group(TabKind::Terminal);
    auto pos = tabs_.begin() + static_cast<std::ptrdiff_t>(group_end(group));
    active_ = static_cast<std::size_t>(pos - tabs_.begin());
    tabs_.insert(pos, Tab{id, {}, {}, {}, {}, {}, TabKind::Terminal, {}, group});
    touch(id);
    return id;
}

std::size_t Model::open_extension(std::string type, std::string group, std::string title) {
    if (std::find(legacyGroups_.begin(), legacyGroups_.end(), group) == legacyGroups_.end())
        legacyGroups_.push_back(group);
    auto actualGroup = home_group(TabKind::Extension, type, group);
    active_ = group_end(actualGroup);
    ++layoutRevision_;
    const auto id = next_id_++;
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(active_),
                 Tab{id, std::move(title), {}, {}, {}, {}, TabKind::Extension, std::move(type),
                     std::move(actualGroup), std::move(group)});
    touch(id);
    return id;
}

bool Model::edit_extension(std::size_t i, std::string text) {
    if (i >= tabs_.size() || tabs_[i].kind != TabKind::Extension)
        return false;
    tabs_[i].draft = std::move(text);
    return true;
}

// Window.tla's CloseTab: the order is Removed, the history Forget, and active_ ClosedActive: the tab of the last id left
// in the history if the closed tab was active, else (and with no id left) ClosedIndex.
bool Model::close_tab(std::size_t i) {
    if (i >= tabs_.size() || (tabs_[i].kind != TabKind::Terminal && tabs_[i].kind != TabKind::Extension))
        return false;
    return dismiss_tab(i);
}

bool Model::dismiss_tab(std::size_t i) {
    if (i >= tabs_.size())
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
    const auto group = home_group(TabKind::Friend);
    const std::size_t at = group_end(group);
    ++layoutRevision_;
    const std::size_t id = next_id_++;
    const bool empty = tabs_.empty();
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at),
                 Tab{id, std::move(f.name), std::move(f.model), {}, {}, {}, TabKind::Friend, {}, group});
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
    const auto group = home_group(TabKind::Friend);
    const auto at = group_end(group);
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
        fresh.push_back(Tab{next_id_++, std::move(f.name), std::move(f.model), {}, {}, {}, TabKind::Friend, {}, group});
    }
    if (fresh.empty()) {
        return added;
    }
    const bool empty = tabs_.empty();
    ++layoutRevision_;
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
    ++layoutRevision_;
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
    ++layoutRevision_;
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
