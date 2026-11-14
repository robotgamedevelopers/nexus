// The versions of what the window and nexus-server send each other, and how a window and a server of different
// versions agree on one (docs/formats.md says the whole scheme). A window and a server never have to be the same
// version: each reads every older format, a format from 2 on frames each session and entry by its length so that a
// reader skips the fields it does not know, and a link uses the newer format both ends read.

#pragma once

#include <cstdint>
#include <string>

namespace nexus::sync {

// The sessions state (sync/sessions.h), down from the server to the window.
//   1  the first: no activity, entry kinds Person, Friend and Notice; nothing framed, so a reader of 1 reads exactly 1.
//      A server that states no format (one before formats were stated) sends 1, and a window that states none reads 1.
//   2  each changed session and each entry framed by its length, the session's activity after its settings, and entry
//      kind Tool. A reader of 2 reads any later format, skipping what it does not know.
constexpr std::uint32_t kStateFormat = 2;

// The oldest format of each end this build still works with: a window stating an older one is refused by the server,
// and a server answering with an older one is refused by the window, each with one plain line. Today every format
// is.
constexpr std::uint32_t kOldestWindowFormat = 1;    // the server's floor for a window
constexpr std::uint32_t kOldestServerFormat = 1;    // the window's floor for a server

// How the window states its format to `nexus-server new` over ssh: an environment variable, which a nexus-server
// before formats ignores (an argument it would refuse).
constexpr const char* kStateFormatVariable = "NEXUS_STATE_FORMAT";

// The format a link uses: the newer of the two that both ends read, so the older of the two stated.
inline std::uint32_t linkFormat(std::uint32_t window, std::uint32_t server) {
    return window < server ? window : server;
}

// Empty when a window stating `window` can be served by a server whose floor is `oldest`; otherwise the one plain
// line the window shows (the server also logs it).
std::string windowRefusal(std::uint32_t window, std::uint32_t oldest = kOldestWindowFormat);
// Empty when a window can read a server answering with `server`, given the window's floor `oldest`; otherwise the one
// plain line the window shows.
std::string serverRefusal(std::uint32_t server, std::uint32_t oldest = kOldestServerFormat);

} // namespace nexus::sync
