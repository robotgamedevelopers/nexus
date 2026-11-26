// One interpreter per host. All calls are made on its owning thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "policy/application.h"

struct NexusPolicyHost;

namespace nexus::policy {

class Host {
public:
    explicit Host(const char* side);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // The window starts its bundle reader before opening and adopts a ready program at frame boundaries.
    void watch(const std::string& path);
    uint64_t refresh();

    // Returns a JSON value, or an empty string and an actionable error on failure.
    std::string decide(const std::string& bundlePath, const char* event, const std::string& context, std::string& why);

private:
    NexusPolicyHost* host_;
    std::string initError_;
};

// A plan is validated in full before the caller executes its first command.
bool messagePlan(Host& host, const std::string& path, bool client, const char* intent,
    const MessageState& state, MessagePlan& out, std::string& why);

// An applied bundle runs in place of the rules this binary packages. When the bundle at `path` (a host's
// policy.bundle.json) is not what `nexus-policy apply <side> policy/rules` writes from this build's commit, by content,
// this returns one line naming the file, how it differs, and the fix; it returns an empty string when the file is
// absent or the same. It warns; nothing refuses to start over it.
std::string staleBundle(const char* side, const std::string& path);
// The bundle `nexus-policy apply <side> policy/rules` writes from this build's commit, as JSON (for the tests).
std::string packagedBundle(const char* side, std::string& why);

int textZoom(Host& host, const std::string& path, int current, int delta, int low, int high);
// Returns an empty string when the bundle disables the fallback or returns an invalid model.
std::string modelAfterLimit(Host& host, const std::string& path, const std::string& friendName,
    const std::string& account, const std::string& model);
std::string accountCommand(Host& host, const std::string& path, const std::string& current,
    const std::vector<std::string>& linked, const std::string& command, std::string& why);
std::string accountAfterLimit(Host& host, const std::string& path, const std::string& current,
    const std::vector<std::string>& linked, const std::vector<std::string>& eligible, std::string& why);

} // namespace nexus::policy
