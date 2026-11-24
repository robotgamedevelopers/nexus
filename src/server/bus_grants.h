// Server-owned message-bus grants from the registry used by each fork's friend-driver policy.
// Refresh from the server loop. Registry reads run off that loop and never reach a driver or client.
#pragma once

#include "command.h"
#include "server/driver_table.h"

#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nexus::server {

class BusGrants {
public:
    explicit BusGrants(CommandRunner run = runCommand) : run_(std::move(run)) {}
    void refresh(const DriverTable& table, std::int64_t nowMs);
    const std::map<std::string, std::string>& addresses() const { return addresses_; }
    const std::string& problem() const { return problem_; }

private:
    using Grants = std::map<std::string, std::pair<std::string, std::string>>; // name to harness and endpoint
    struct Query {
        std::future<CommandResult> pending;
        Grants grants;
        std::string problem;
        std::int64_t next = 0;
    };

    static bool read(const std::string& text, Grants& grants);

    CommandRunner run_;
    std::map<std::vector<std::string>, Query> queries_;
    std::map<std::string, std::string> addresses_;
    std::string problem_;
};
} // namespace nexus::server
