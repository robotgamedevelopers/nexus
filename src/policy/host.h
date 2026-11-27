// One interpreter per host. All calls are made on its owning thread.
#pragma once

#include <cstdint>
#include <map>
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

std::string inputAction(Host& host, const std::string& target, const std::string& intent, std::string& why);

// A plan is validated in full before the caller executes its first command.
bool messagePlan(Host& host, const std::string& path, bool client, const char* intent,
    const MessageState& state, MessagePlan& out, std::string& why);

// Human-facing application replies are formatted by the fork, never by the transport.
std::string noticeText(Host& host, const std::string& path, const std::string& kind,
    const std::map<std::string, std::string>& fields);

int textZoom(Host& host, const std::string& path, int current, int delta, int low, int high);
// Returns an empty string when the bundle disables the fallback or returns an invalid model.
std::string modelAfterLimit(Host& host, const std::string& path, const std::string& aiName,
    const std::string& account, const std::string& model);
std::string accountCommand(Host& host, const std::string& path, const std::string& current,
    const std::vector<std::string>& linked, const std::string& command, std::string& why);
std::string accountAfterLimit(Host& host, const std::string& path, const std::string& current,
    const std::vector<std::string>& linked, const std::vector<std::string>& eligible, std::string& why);

} // namespace nexus::policy
