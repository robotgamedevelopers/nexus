// The window's rules for its link to nexus-server, apart from the link itself so that they run in unit tests: what the
// status line says, and what a prompt sends.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "model.h"
#include "sync/actions.h"

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

// Every byte an ASCII space, tab, newline, vertical tab, form feed or carriage return (the empty text too): the rule
// Model::send uses (model.cpp blank).
bool blankText(const std::string& text);

// The prompt of tab i as an action of `kind`: true, with its text, and the tab's draft cleared, when i is a friend's tab
// whose draft is not all whitespace and fits `limit` bytes; otherwise false, nothing changed, and `why` says so when the
// draft is too long (an empty or blank draft sends nothing, as Model::send).
bool takePrompt(Model& model, std::size_t i, std::size_t limit, std::string* text, std::string* why);

// The stop of tab i's friend's work (Command-Escape) as an action: true, with the friend's name (the tab the action
// names), when i is a friend's tab; otherwise false. A stop carries no text, changes nothing in the window, and is
// sent whether or not the friend is working: the server stops the turn open, if any, and ignores it otherwise.
bool stopTarget(const Model& model, std::size_t i, std::string* tab);

} // namespace nexus::sync
