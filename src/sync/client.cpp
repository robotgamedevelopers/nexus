#include "sync/client.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

#include "accounts/registry.h"

namespace nexus::sync {

bool blankText(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
        [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; });
}

std::string linkStatus(LinkPhase phase, const std::string& host, std::uint64_t heardMsAgo, const std::string& problem) {
    switch (phase) {
        case LinkPhase::Connecting:
            return "Connecting to " + host;
        case LinkPhase::Failed:
            return host + ": " + problem;
        case LinkPhase::Connected:
            if (heardMsAgo < kQuietMs)
                return std::string();
            return "No contact with " + host + " for " + std::to_string(heardMsAgo / 1000) + " s";
    }
    return std::string();
}

bool linkLost(LinkPhase phase, std::uint64_t heardMsAgo) {
    return phase != LinkPhase::Connected || heardMsAgo >= kQuietMs;
}

CursorState cursorOf(LinkPhase phase, std::uint64_t heardMsAgo, const Mirror& mirror, std::size_t tabId) {
    if (linkLost(phase, heardMsAgo)) {
        return CursorState::Lost;
    }
    const Activity* activity = mirror.activity(tabId);
    if (activity == nullptr) {
        return CursorState::Lost;
    }
    switch (*activity) {
        case Activity::Working: return CursorState::Working;
        case Activity::Busy:
            return CursorState::Busy;
        case Activity::Down:
            return CursorState::Down;
        case Activity::Idle:
            break;
    }
    return CursorState::Idle;
}

bool takePrompt(Model& model, std::size_t i, std::size_t limit, std::string* text, std::string* why) {
    if (i >= model.tab_count() || model.tab(i).kind != TabKind::Friend)
        return false;
    const std::string& draft = model.tab(i).draft;
    if (blankText(draft))
        return false;
    if (draft.size() > limit) {
        if (why != nullptr)
            *why = "This prompt is " + std::to_string((draft.size() + 1023) / 1024) + " KiB; the limit is " +
                std::to_string(limit / 1024) + " KiB.";
        return false;
    }
    *text = draft;
    model.set_draft(i, std::string());
    return true;
}

SessionId sessionOf(const Model& model, std::size_t i, const Mirror& mirror) {
    if (i >= model.tab_count() || model.tab(i).kind != TabKind::Friend) {
        return 0;
    }
    return mirror.session(model.tab(i).id);
}

std::vector<std::string> friendsToEnsure(const Model& model) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::Friend) {
            out.push_back(model.tab(i).name);
        }
    }
    return out;
}

bool friendNameShown(std::string_view name) {
    return accounts::isName(name);
}

std::vector<FriendInfo> friendsOf(const Sessions& state, std::vector<std::string>* dropped, std::size_t* past) {
    std::vector<FriendInfo> out;
    std::unordered_set<std::string_view> named;
    std::unordered_set<std::string_view> refused;
    std::size_t over = 0;
    const auto take = [&](const std::string& name, const std::string& model) {
        if (named.count(name) != 0 || refused.count(name) != 0) {
            return;
        }
        if (!friendNameShown(name)) {
            refused.insert(name);
            if (dropped != nullptr) {
                dropped->push_back(name);
            }
            return;
        }
        named.insert(name);
        if (out.size() == kMaxFriendTabs) {
            ++over;
            return;
        }
        out.push_back(FriendInfo{name, model});
    };
    const auto done = [&] {
        if (past != nullptr) {
            *past = over;
        }
        return out;
    };
    if (state.friendsKnown()) {
        for (const ServerFriend& f : state.friends()) {
            take(f.name, std::string());
        }
        return done();
    }
    for (const SessionRef& s : state.sessions()) {
        take(s->info.friendName, s->info.model);
    }
    return done();
}

FriendChanges applyFriends(Model& model, const std::vector<FriendInfo>& friends, bool exact,
        const std::vector<std::string>& savedOrder, KeptDrafts* kept) {
    FriendChanges changes;
    std::unordered_set<std::string_view> listed;
    for (const FriendInfo& f : friends) {
        listed.insert(f.name);
    }
    if (exact) {
        changes.removed = model.remove_friends([&](const Tab& tab) {
            if (listed.count(tab.name) != 0) {
                return false;
            }
            // What the person typed to the friend stays, for when the friend is back.
            if (kept != nullptr && !tab.draft.empty()) {
                (*kept)[tab.name] = tab.draft;
                changes.put.emplace_back(tab.name, tab.draft);
                changes.keptChanged = true;
            }
            return true;
        });
    }
    // The friends without a tab: those the saved order names, in its order, then the rest in the list's.
    std::unordered_set<std::string_view> standing;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::Friend) {
            standing.insert(model.tab(i).name);
        }
    }
    std::unordered_map<std::string_view, const FriendInfo*> fresh;
    for (const FriendInfo& f : friends) {
        if (standing.count(f.name) == 0) {
            fresh.emplace(f.name, &f);
        }
    }
    std::vector<FriendInfo> ordered;
    ordered.reserve(fresh.size());
    std::unordered_set<std::string_view> placed;
    for (const std::string& name : savedOrder) {
        const auto at = fresh.find(name);
        if (at != fresh.end() && placed.insert(at->first).second) {
            ordered.push_back(*at->second);
        }
    }
    for (const FriendInfo& f : friends) {
        if (fresh.count(f.name) != 0 && placed.insert(f.name).second) {
            ordered.push_back(f);
        }
    }
    std::size_t at = 0;
    changes.added = model.add_friends(std::move(ordered), &at);
    if (kept != nullptr && !kept->empty()) {
        for (std::size_t i = at; i < at + changes.added.size(); ++i) {
            const auto draft = kept->find(model.tab(i).name);
            if (draft != kept->end()) {
                changes.taken.emplace_back(draft->first, draft->second);
                model.set_draft(i, std::move(draft->second));
                kept->erase(draft);
                changes.keptChanged = true;
            }
        }
    }
    return changes;
}

bool stopTarget(const Model& model, std::size_t i, const Mirror& mirror, SessionId* session) {
    const SessionId id = sessionOf(model, i, mirror);
    if (id == 0) {
        return false;
    }
    *session = id;
    return true;
}

} // namespace nexus::sync
