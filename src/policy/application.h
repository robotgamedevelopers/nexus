// The commands an application can return for one input. A message's notices and
// driver operation share the same durable commit and acknowledgement.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nexus::policy {
enum class Delivery { None, Quiet, Loud, Hold, Release, Send, Queue, Native, SignOut, ClearLocal, Clear, Compact, SetModel, SetEffort, Goal };
struct MessagePlan {
    Delivery delivery = Delivery::None;
    std::optional<std::string> text;
    std::optional<std::string> effort;
    std::string service;
    std::vector<std::string> notices;
};
struct UsageRow {
    std::string harness, account;
    int minutes, percent;
    std::uint64_t ageMinutes;
};
struct MessageState {
    const std::string& text;
    const std::string& aiName;
    const std::string& model;
    std::size_t queued;
    bool busy;
    std::string effort{}, harness{}, catalog{};
    bool signedOut = false;
    std::string accountEmail{}, loginState{}, aiInfo{};
    std::vector<UsageRow> usage{};
};
} // namespace nexus::policy
