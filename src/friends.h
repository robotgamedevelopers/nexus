// The friends to show as tabs, read from `nova-config friend list --json`.

#pragma once

#include <string>
#include <vector>

#include "model.h"

namespace nexus {

// Runs the command and returns the enabled friends it lists. On failure or none, returns four placeholders and writes
// why to `note`.
std::vector<Friend> loadFriends(std::string& note);

} // namespace nexus
