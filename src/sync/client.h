// The window's rules for its link to nexus-server, apart from the link itself so that they run in unit tests: what the
// status line says, and what a prompt sends.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// The friends whose session a window ensures (ActionKind::Ensure) when its link starts: one per friend tab, in the
// tabs' order. The server makes a session only for a friend that has none.
std::vector<std::string> friendsToEnsure(const Model& model);

// The stop of tab i's friend's work (Command-Escape) as an action: true, with the session the tab shows (the session
// the action names), when i is a friend's tab that shows one; otherwise false. A stop carries no text, changes nothing
// in the window, and is sent whatever the session shows and whether or not the link is up (it waits in the outbox, in
// order): the server decides, against its state when it processes the stop.
bool stopTarget(const Model& model, std::size_t i, const Mirror& mirror, SessionId* session);

} // namespace nexus::sync
