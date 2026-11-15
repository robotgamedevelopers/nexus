// The friend tabs as the window keeps them from the server's state (AGENTS.md "Friend tabs"): the one place the
// window's frame (frame_host.h) and the link's functional test (tests/link/link_e2e.cpp) both bring a model's tabs to a
// state (takeFriends), so that the test drives the code the window runs. models/FriendStrip.tla is its specification, and
// tests/model/strip_walk.cpp checks that this code's behaviours are behaviours of that spec.
//
// What it keeps beside the model: the order the person last left the friends in and the friend whose tab was saved as
// selected (both read at launch, tab_order.h), whether that saved tab still waits for its friend, the drafts of friends
// that have no tab (sync/client.h KeptDrafts), and the friends it last brought the tabs to, so that a state whose
// friends did not change costs nothing. Pure: no OS, no clock.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model.h"
#include "sync/actions.h"
#include "sync/client.h"
#include "sync/mirror.h"
#include "sync/sessions.h"

namespace nexus::sync {

// What one take changed, for the window to act on: the tabs added (a session ensured with each), the tabs removed
// (the mirror forgets each), the names the server sent that are not a friend's name (logged, no tab), and whether the
// kept drafts changed, with the drafts kept and given back (FriendChanges put, taken: the change saved, tab_order.h
// KeptChange). `ran` is false for a state whose friends are the ones last taken: nothing changed.
struct StripTake {
    bool ran = false;
    std::vector<std::string> added;
    std::vector<std::size_t> removed;
    std::vector<std::string> dropped;
    std::size_t past = 0;       // friends past kMaxFriendTabs, which have no tab (logged)
    bool keptChanged = false;
    std::vector<std::pair<std::string, std::string>> put;
    std::vector<std::pair<std::string, std::string>> taken;
};

class FriendStrip {
public:
    FriendStrip() = default;
    // At launch: the saved order, the friend whose tab was saved as selected (empty: the saved tab is no friend's), and
    // the drafts kept from before. With a saved friend, its tab is selected when it arrives, unless the person chose a
    // tab first.
    FriendStrip(std::vector<std::string> savedOrder, std::string savedFriend, KeptDrafts kept);

    // Brings the model's friend tabs to `state` (applyFriends over friendsOf), only when the state's friends differ from
    // the ones last taken: the server's list by its pointer (Sessions::friendsList) and whether it is known; without a
    // list, the sessions' friends by their count and the newest session's id (a session's friend never changes, and ids
    // only grow). Then, while the saved tab waits: its friend's tab, once it has one, is selected, and the wait ends; a
    // list from the server that does not name it ends the wait too, the selection left as it is.
    StripTake take(Model& model, const Sessions& state);

    // The person chose a tab: the saved one no longer waits.
    void personSelected() { pending_ = false; }

    // The saved tab waits for its friend.
    bool pending() const { return pending_; }
    // A state's friends have been taken and the strip holds a friend, or the server sent its list: from then on the
    // friends' order is the person's to save.
    bool taken() const { return taken_; }
    // The strip holds a friend's tab.
    bool hasFriends() const { return friendTabs_ != 0; }
    const KeptDrafts& kept() const { return kept_; }

private:
    std::vector<std::string> savedOrder_;
    std::string savedFriend_;
    bool pending_ = false;
    bool taken_ = false;
    KeptDrafts kept_;
    std::size_t friendTabs_ = 0;
    // What was last taken: nothing yet, a list (held, so that its pointer is never another list's), or the sessions'.
    bool any_ = false;
    bool known_ = false;
    std::shared_ptr<const std::vector<ServerFriend>> list_;
    std::size_t sessionCount_ = 0;
    SessionId newest_ = 0;
};

// The friends of `state` into the window, as the window's frame (FrameHost::takeFriends) and the link's functional
// test both bring them: the strip's take, the mirror's record of each removed tab forgotten, and an ensure for each
// friend whose tab is new handed to `ensure` (unset: none sent; the server makes a session only for a friend that has
// none). The take is returned for the caller's own part: its logs, its kept drafts and its notice.
StripTake takeFriends(FriendStrip& strip, Model& model, Mirror& mirror, const Sessions& state,
    const std::function<void(Action)>& ensure);

// `nexus --server H --tab N`: an N past the tabs at launch may name a friend's tab that the server's friends bring, so
// the window waits for them before it judges N. Whether it waits, at launch:
bool tabWaitsForFriends(bool tabGiven, bool server, std::size_t tab, std::size_t tabs);
// Whether that wait goes on: the friends not taken yet, the link not failed, and time left (milliseconds on the
// caller's clock, so that the rule holds no clock).
bool tabWaitGoesOn(const FriendStrip& strip, bool linkFailed, long long nowMs, long long untilMs);

} // namespace nexus::sync
