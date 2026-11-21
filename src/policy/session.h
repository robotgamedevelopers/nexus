#pragma once

#include "accounts/registry.h"
#include "policy/host.h"
#include "sync/sessions.h"

namespace nexus::policy {
bool newSession(Host& host, const std::string& path, const sync::SessionInfo& defaults, const sync::SessionInfo* source,
    const accounts::Friend* who, bool hasRoster, bool canAutoAccount, sync::SessionInfo& result, std::string& why);
}
