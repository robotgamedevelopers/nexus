// A window drawn from a server's state file alone, as `nexus --screenshot --state FILE` draws it (main.cpp): no home, no
// config, no saved tab order and no sprint board, and the AI tabs the state's AIs (sync/client.h aisOf).
// The iOS app and its headless frame (tests/ios/state_shot.cpp) take a state file this way.

#pragma once

#include <string>

#include "frame_host.h"
#include "model.h"
#include "sync/sessions.h"
#include "view.h"

namespace nexus {

// Reads the state file at `path` into `held`. False, with why (naming the file), if it cannot be read or is no state.
bool readStateAt(const std::string& path, sync::Sessions* held, std::string* why);

// Puts `held` into the window: its AIs as tabs of `model` (the first AI's selected unless `tab`, 1-based, is
// given), and its sessions shown through `host` as the window shows them once the server is heard, or once it has lost
// the server with `lost`. False, with why, if `tab` names no tab.
bool showHeldState(const sync::Sessions& held, bool lost, std::size_t tab, Model& model, View& view, FrameHost& host,
    std::string* why);

} // namespace nexus
