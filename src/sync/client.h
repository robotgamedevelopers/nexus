// The window's rules for its link to nexus-server, apart from the link itself so that they run in unit tests: what the
// status line says, and what a prompt sends.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accounts/registry.h"
#include "cursor.h"
#include "model.h"
#include "sync/actions.h"
#include "sync/mirror.h"

namespace nexus::sync {

enum class LinkPhase {
    Connecting,     // resolving the host and running nexus-server new over ssh, until the server is first heard
    Connected,      // the server has been heard
    Failed,         // the bootstrap failed; `problem` says why
};

// After this long without hearing the server, the status line says so (mosh's heartbeat is every 3 s).
constexpr std::uint64_t kQuietMs = 6000;

// The status line over the prompt's place: plain, and empty while all is well. Connecting: "Connecting to <host>".
// Failed: "<host>: <problem>". Connected and heard within kQuietMs: empty. Connected and not heard for longer:
// "No contact with <host> for <seconds> s".
std::string linkStatus(LinkPhase phase, const std::string& host, std::uint64_t heardMsAgo, const std::string& problem);

// The window has lost the server: not yet connected, the bootstrap failed, or not heard for longer than kQuietMs. What
// the window shows of the server's sessions is then stale.
bool linkLost(LinkPhase phase, std::uint64_t heardMsAgo);

// The state cursor of a friend's tab (AGENTS.md "The state cursor"), from the server's state only: Lost while the link
// is lost (never Busy or Idle then: what the window holds is stale), and while no state has held the session the tab
// shows; otherwise the session's activity in the newest state the window applied (Mirror::activity).
CursorState cursorOf(LinkPhase phase, std::uint64_t heardMsAgo, const Mirror& mirror, std::size_t tabId);

// Every byte an ASCII space, tab, newline, vertical tab, form feed or carriage return (the empty text too): the rule
// Model::send uses (model.cpp blank).
bool blankText(const std::string& text);

// The prompt of tab i as an action of `kind`: true, with its text, and the tab's draft cleared, when i is a friend's tab
// whose draft is not all whitespace and fits `limit` bytes; otherwise false, nothing changed, and `why` says so when the
// draft is too long (an empty or blank draft sends nothing, as Model::send).
bool takePrompt(Model& model, std::size_t i, std::size_t limit, std::string* text, std::string* why);

// The session tab i shows, which an action from it names: 0 when i is not a friend's tab, or when no state the mirror
// applied has held a session of its friend yet (the window then sends nothing from it; its draft stays).
SessionId sessionOf(const Model& model, std::size_t i, const Mirror& mirror);

// The friends whose session a window ensures (ActionKind::Ensure): one per friend tab, in the tabs' order. The server
// makes a session only for a friend that has none. The window ensures each friend's as its tab is added
// (applyFriends).
std::vector<std::string> friendsToEnsure(const Model& model);

// A name the window shows as a friend's tab and saves in its tab order: the server's own rule for a friend's name
// (accounts::isName: 1 to 64 letters, digits, '-', '_' or '.', not starting with '.' or '-'), so no control character,
// no newline and nothing a label or a line of a saved file cannot hold.
bool friendNameShown(std::string_view name);

// The most friend tabs a window shows: the most friends a server's friends.json holds (accounts::kMaxFriends). A list
// longer than that comes only from a server of another build; its first kMaxFriendTabs friends get tabs. Measured
// (tests/window/test_frames.cpp, many_friends_while_streaming): 2,000 new tabs cost one frame 24 ms of layout, every
// new label set at once; 256 keep the budget.
constexpr std::size_t kMaxFriendTabs = accounts::kMaxFriends;

// The friends a window shows for `state`: the server's own list when the state carries one (Sessions::friendsKnown),
// in its order; otherwise, from a server before the list, the friends of the sessions it holds, each once, in the
// order of its first session. The server's list names no model: a friend's model is the server's, per session. A name
// that is not friendNameShown is left out, and named once in `dropped` when given; past kMaxFriendTabs friends the rest
// are left out, and counted in `past` when given. Time linear in the list or the sessions.
std::vector<FriendInfo> friendsOf(const Sessions& state, std::vector<std::string>* dropped = nullptr,
    std::size_t* past = nullptr);

// The drafts the window keeps for friends of its server that have no tab, by friend name: what the person typed to a
// friend the server's list then left out, given back when a friend of that name has a tab again (applyFriends). The
// window keeps them across launches by server and name (tab_order.h KeptChange) and never deletes one itself.
using KeptDrafts = std::map<std::string, std::string>;

// What applyFriends changed: the names of the friends whose tabs it added, in the order it added them, the ids
// (Tab::id) of the friends' tabs it removed, whether the kept drafts changed, and how: the drafts it kept (`put`, a
// removed tab's friend and draft) and those it gave back to a new tab (`taken`, the friend and the draft given).
struct FriendChanges {
    std::vector<std::string> added;
    std::vector<std::size_t> removed;
    bool keptChanged = false;
    std::vector<std::pair<std::string, std::string>> put;
    std::vector<std::pair<std::string, std::string>> taken;
};

// Brings the model's friend tabs to `friends` (models/Window.tla AddFriend, RemoveFriend). With `exact` (the server's
// own list), each friend's tab whose friend the list does not name is removed (Model::remove_friends: its transcript
// and queue, which are the server's, go with it, and if it was active the tab selected before it shows); without (the
// friends of an older server's sessions, which name only the friends that have one), none is. A removed tab's draft,
// when it is not empty, goes into `kept` under its friend's name. Each friend the list names that has no tab gets one
// (Model::add_friends), at the end of the friends' zone, in the order `savedOrder` (tab_order.h) names them, then in the
// list's order, with the draft `kept` holds for its name, which then leaves `kept`. The tabs already standing keep
// their order. Time linear in the tabs, the list and the saved order. Pure: no OS.
FriendChanges applyFriends(Model& model, const std::vector<FriendInfo>& friends, bool exact,
    const std::vector<std::string>& savedOrder, KeptDrafts* kept = nullptr);

// The stop of tab i's friend's work (Command-Escape) as an action: true, with the session the tab shows (the session
// the action names), when i is a friend's tab that shows one; otherwise false. A stop carries no text, changes nothing
// in the window, and is sent whatever the session shows and whether or not the link is up (it waits in the outbox, in
// order): the server decides, against its state when it processes the stop.
bool stopTarget(const Model& model, std::size_t i, const Mirror& mirror, SessionId* session);

} // namespace nexus::sync
