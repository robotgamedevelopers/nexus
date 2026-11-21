// The commands an application can return for one input. A message's notices and
// driver operation share the same durable commit and acknowledgement.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace nexus::policy {
enum class Delivery { None, Send, Queue, Native, SignOut };
struct MessagePlan {
    Delivery delivery = Delivery::None;
    std::optional<std::string> text;
    std::string service;
    std::vector<std::string> notices;
};
struct MessageState {
    const std::string& text;
    const std::string& friendName;
    const std::string& model;
    std::size_t queued;
    bool busy;
};
} // namespace nexus::policy
