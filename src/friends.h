// The friends to show as tabs, read from `nova-config friend list --json`.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "model.h"

namespace nexus {

// The enabled friends, in the order the command lists them, or why there are none.
struct FriendList {
    std::vector<FriendInfo> friends;
    std::string error;          // empty when `friends` came from the command; otherwise one line for the window
};

// Reads the command's JSON: {"result": {"status": "ok"}, "items": [{"fields": {"name", "model", "enabled"}}]}. Keeps
// the friends whose `enabled` is true. Pure: no OS.
FriendList parseFriendList(std::string_view json);

// Runs `nova-config friend list --json` with this process's environment and parses what it prints. On a failure the
// error carries the command's own text from its standard error.
FriendList loadFriends();

} // namespace nexus
