// One interpreter per host. All calls are made on its owning thread.
#pragma once

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

    // Returns a JSON value, or an empty string and an actionable error on failure.
    std::string decide(const std::string& bundlePath, const char* event, const std::string& context, std::string& why);

private:
    NexusPolicyHost* host_;
    std::string initError_;
};

// A plan is validated in full before the caller executes its first command.
bool messagePlan(Host& host, const std::string& path, bool client, const char* intent,
    const MessageState& state, MessagePlan& out, std::string& why);

int textZoom(Host& host, const std::string& path, int current, int delta, int low, int high);
std::string accountAfterLimit(Host& host, const std::string& path, const std::string& current,
    const std::vector<std::string>& linked, const std::vector<std::string>& eligible, std::string& why);

} // namespace nexus::policy
