// Server-owned message-bus grants and friend identities from each fork's registry.
// Registry reads run off the serving loop and never reach a driver or client.
#pragma once

#include "command.h"
#include "server/driver_table.h"

#include <cstdint>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nexus::server {

enum class BusPeer { Pending, Friend, Other };

class BusGrants {
public:
    explicit BusGrants(CommandRunner run = runCommand) : run_(std::move(run)) {}
    void refresh(const DriverTable& table, std::int64_t nowMs);
    const std::map<std::string, std::string>& addresses() const { return addresses_; }
    const std::string& problem() const { return problem_; }
    // Each new delivery waits for its own profile's next successful registry query. Friends may have
    // been added or disabled since a previous delivery, including when a refresh is already in flight.
    BusPeer sender(const std::string& recipient, const std::string& id, const std::string& name);
    // Only for projecting history: keep each recipient's most recent complete roster during outages.
    const std::map<std::string, std::set<std::string>>& knownSenders() const { return knownSenders_; }

    const std::set<std::string>& unsandboxed() const { return unsandboxed_; }

private:
    std::set<std::string> unsandboxed_;
    struct Friend {
        std::string harness, address;
        bool noSandbox;
        bool operator==(const Friend&) const = default;
    };
    using Program = std::vector<std::string>;
    using Roster = std::map<std::string, Friend>; // enabled name to harness and optional endpoint
    struct Delivery {
        std::string id, sender;
        std::uint64_t required = 0;
    };
    struct Query {
        std::future<CommandResult> pending;
        std::optional<Roster> roster;
        std::map<std::string, Delivery> deliveries;
        std::uint64_t started = 0, completed = 0;
        std::string problem;
        std::int64_t next = 0;
    };

    static bool read(const std::string& text, Roster& roster);

    CommandRunner run_;
    std::map<Program, Query> queries_;
    std::map<std::string, Program> owners_;
    std::map<std::string, std::string> addresses_;
    std::map<std::string, std::set<std::string>> knownSenders_;
    std::string problem_;
};
} // namespace nexus::server
