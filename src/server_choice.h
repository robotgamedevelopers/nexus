// The server a device's window connects to when nothing names one: on iOS and iPadOS there is no shell to pass
// `--server` and no file the person edits, so on first launch the window asks for it (view.h ServerChoice) and keeps
// the answer in its own home, ~/.nexus/client/server.txt (client_home.h), as it keeps the text scale. The person's
// input, never config put on the device by hand.
//
// server.txt:
//
//   nexus server 1
//   user@host
//
// An address is one line of at most kServerAddressMax bytes with no spaces or control bytes: `host`, or `user@host`
// where the device needs the user (iOS: no ssh config supplies one). The user is letters, digits, `.`, `_` and `-`; the
// host is letters, digits, `.`, `-` and `:`, or a bracketed IPv6 address, and starts with neither `-` nor `.`, so it
// can never read as an ssh option.

#pragma once

#include <string>
#include <string_view>

#include "tab_order.h"

namespace nexus {

constexpr std::size_t kServerAddressMax = 255;

// The address in `text` (spaces at either end ignored), into `address`. False, with `why` one line saying what is
// wrong, when it is not one; `needUser` refuses a bare host. Pure.
bool parseServerAddress(std::string_view text, bool needUser, std::string* address, std::string* why);

// server.txt's text for `address`, and back. parseServerFile is false with `why` for anything else. Pure.
std::string formatServerFile(const std::string& address);
bool parseServerFile(std::string_view text, bool needUser, std::string* address, std::string* why);

// The store: server.txt in the client home `dir`, made at the first write.
TabOrderStore fileServerStore(const std::string& dir);

// The kept address, or empty with `warning` empty when none is kept, or empty with `warning` naming the file when
// what is kept is not an address (the window asks again).
std::string loadServerAddress(const TabOrderStore& store, bool needUser, std::string* warning);

} // namespace nexus
