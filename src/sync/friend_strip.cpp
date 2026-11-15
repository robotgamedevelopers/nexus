#include "sync/friend_strip.h"

#include <utility>

namespace nexus::sync {

FriendStrip::FriendStrip(std::vector<std::string> savedOrder, std::string savedFriend, KeptDrafts kept)
    : savedOrder_(std::move(savedOrder)), savedFriend_(std::move(savedFriend)), pending_(!savedFriend_.empty()),
      kept_(std::move(kept)) {}

StripTake FriendStrip::take(Model& model, const Sessions& state) {
    StripTake out;
    const bool known = state.friendsKnown();
    const std::vector<SessionRef>& sessions = state.sessions();
    const SessionId newest = sessions.empty() ? 0 : sessions.back()->id;
    if (any_ && known == known_) {
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

    FriendChanges changes = applyFriends(model, friendsOf(state, &out.dropped, &out.past), known, savedOrder_, &kept_);
    out.added = std::move(changes.added);
    out.removed = std::move(changes.removed);
    out.keptChanged = changes.keptChanged;
    out.put = std::move(changes.put);
    out.taken = std::move(changes.taken);

    friendTabs_ = 0;
    std::size_t saved = model.tab_count();
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::Friend) {
            ++friendTabs_;
            if (pending_ && model.tab(i).name == savedFriend_) {
                saved = i;
            }
        }
    }
    if (pending_ && saved < model.tab_count()) {
        model.select_tab(saved);
        pending_ = false;
    } else if (pending_ && known) {
        pending_ = false;
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
