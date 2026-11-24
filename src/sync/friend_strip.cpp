#include "sync/friend_strip.h"

#include <algorithm>
#include <utility>

namespace nexus::sync {

FriendStrip::FriendStrip(std::vector<std::string> savedOrder, std::string savedFriend, KeptDrafts kept)
    : savedOrder_(std::move(savedOrder)), savedFriend_(std::move(savedFriend)), pending_(!savedFriend_.empty()),
      kept_(std::move(kept)) {}

void FriendStrip::dismiss(const Tab& tab) {
    if (tab.primaryChat()) return;
    dismissed_.insert(tab.name);
    if (friendTabs_) --friendTabs_;
    if (!tab.draft.empty()) kept_[tab.name] = tab.draft;
}

StripTake FriendStrip::take(Model& model, const Sessions& state) {
    StripTake out;
    const bool known = state.friendsKnown();
    const std::vector<SessionRef>& sessions = state.sessions();
    const SessionId newest = sessions.empty() ? 0 : sessions.back()->id;
    const SessionId primary = state.primaryChat();
    if (any_ && known == known_ && primary == primary_) {
        if (known && state.friendsList() == list_) {
            return out;
        }
        if (!known && sessions.size() == sessionCount_ && newest == newest_) {
            return out;
        }
    }
    any_ = true;
    known_ = known;
    list_ = known ? state.friendsList() : nullptr;
    sessionCount_ = sessions.size();
    newest_ = newest;
    out.ran = true;

    // A different server binding gets its own transcript, unsent drafts and mirror cursor.
    bool replacedActive = false;
    if (primary != primary_) {
        if (primary_) {
            replacedActive = model.tab_count() && model.tab(model.active()).primaryChat();
            out.removed = model.remove_friends([](const Tab& tab) { return tab.primaryChat(); });
        }
        if (primary) model.add_friend({kPrimaryChat, state.find(primary)->info.model});
        primary_ = primary;
    }
    auto visible = friendsOf(state, &out.dropped, &out.past);
    visible.erase(std::remove_if(visible.begin(), visible.end(),
        [&](const FriendInfo& f) { return dismissed_.count(f.name) != 0; }), visible.end());
    FriendChanges changes = applyFriends(model, visible, known, savedOrder_, &kept_);
    out.added = std::move(changes.added);
    out.removed.insert(out.removed.end(), changes.removed.begin(), changes.removed.end());
    out.keptChanged = changes.keptChanged;
    out.put = std::move(changes.put);
    out.taken = std::move(changes.taken);

    friendTabs_ = 0;
    std::size_t saved = model.tab_count();
    std::size_t chat = model.tab_count();
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::Friend) {
            ++friendTabs_;
            if (model.tab(i).primaryChat()) chat = i;
            if (pending_ && model.tab(i).name == savedFriend_) {
                saved = i;
            }
        }
    }
    if (pending_ && saved < model.tab_count()) {
        model.select_tab(saved);
        pending_ = false;
        preferPrimary_ = false;
    } else if (pending_ && known && savedFriend_ != kPrimaryChat) {
        pending_ = false;
    }
    if (chat < model.tab_count() && !pending_ && (preferPrimary_ || replacedActive)) {
        model.select_tab(chat);
        preferPrimary_ = false;
    }
    taken_ = taken_ || friendTabs_ != 0 || known;
    return out;
}

bool tabWaitsForFriends(bool tabGiven, bool server, std::size_t tab, std::size_t tabs) {
    return tabGiven && server && tab > tabs;
}

bool tabWaitGoesOn(const FriendStrip& strip, bool linkFailed, long long nowMs, long long untilMs) {
    return !strip.taken() && !linkFailed && nowMs < untilMs;
}

StripTake takeFriends(FriendStrip& strip, Model& model, Mirror& mirror, const Sessions& state,
        const std::function<void(Action)>& ensure) {
    StripTake changes = strip.take(model, state);
    for (std::size_t id : changes.removed) {
        mirror.forget(id);
    }
    if (ensure) {
        for (const std::string& name : changes.added) {
            Action a;
            a.kind = ActionKind::Ensure;
            a.friendName = name;
            ensure(std::move(a));
        }
    }
    return changes;
}

} // namespace nexus::sync
