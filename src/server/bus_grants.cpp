#include "server/bus_grants.h"

#include "sync/bus_peer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string_view>
#include <system_error>

namespace nexus::server {

namespace {

// Only the bundled identity profile's documented launcher opts its friends into this integration.
// The executable/profile pair belongs to the fork. No registry program or profile comes from a message.
std::vector<std::string> queryFor(const std::vector<std::string>& argv) {
    if (argv.size() < 9 || argv[0].empty() || argv[0].front() != '/' || argv[1] != "-I" ||
        argv[2] != "-m" || argv[3] != "nexus_policy" || argv[4] != "friend-driver" ||
        argv[5].empty() || argv[5].front() != '/') return {};
    return {argv[0], "-I", "-m", "nexus_policy", "bus-roster", argv[5]};
}

bool endpoint(std::string_view text) {
    constexpr std::string_view loopback = "127.0.0.1:";
    if (!text.starts_with(loopback)) return false;
    const auto port = text.substr(loopback.size());
    if (port.empty() || port.size() > 5 || port.front() == '0') return false;
    unsigned value = 0;
    const auto end = port.data() + port.size();
    const auto [after, error] = std::from_chars(port.data(), end, value);
    return error == std::errc{} && after == end && value > 0 && value <= 65535;
}
} // namespace

bool BusGrants::read(const std::string& text, Roster& roster) {
    // The old grant-only output cannot establish that a sender is absent. Require the complete roster.
    constexpr std::string_view header = "nexus-bus-roster-v1\n";
    if (text.size() > 1024 * 1024 || !text.starts_with(header)) return false;
    Roster parsed;
    std::string_view rest(text);
    rest.remove_prefix(header.size());
    while (!rest.empty()) {
        const auto eol = rest.find('\n');
        if (eol == rest.npos) return false;
        const auto line = rest.substr(0, eol);
        rest.remove_prefix(eol + 1);
        const auto first = line.find('\t');
        if (first == line.npos) return false;
        const auto second = line.find('\t', first + 1);
        if (second == line.npos) return false;
        const auto name = line.substr(0, first);
        const auto harness = line.substr(first + 1, second - first - 1);
        const auto address = line.substr(second + 1);
        // External peers can use a harness with no local driver. It only has to be a registry identifier.
        constexpr std::string_view identifiers = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:/-";
        if (!sync::busPeerName(name) || harness.empty() || harness.size() > 128 ||
            harness.find_first_not_of(identifiers) != harness.npos || (!address.empty() && !endpoint(address)) ||
            !parsed.try_emplace(std::string(name), harness, address).second) return false;
    }
    roster = std::move(parsed);
    return true;
}

BusPeer BusGrants::sender(const std::string& recipient, const std::string& id, const std::string& name) {
    const auto owner = owners_.find(recipient);
    if (owner == owners_.end() || !addresses_.contains(recipient)) return BusPeer::Pending;
    auto& query = queries_.at(owner->second);
    auto& delivery = query.deliveries[recipient];
    if (delivery.id != id || delivery.sender != name) delivery = {id, name, query.started + 1};
    if (!query.roster || !query.problem.empty() || query.completed < delivery.required) return BusPeer::Pending;
    return query.roster->contains(name) ? BusPeer::Friend : BusPeer::Other;
}

void BusGrants::refresh(const DriverTable& table, std::int64_t nowMs) {
    using namespace std::chrono_literals;
    std::map<Program, std::map<std::string, std::string>> wanted;
    std::map<std::string, Program> routes;
    for (const auto& [name, entry] : table.friends) {
        const auto driver = table.drivers.find(entry.harness);
        if (driver == table.drivers.end()) continue;
        const auto& argv = driver->second;
        const auto query = queryFor(argv);
        if (!query.empty() && (argv[6] == "{friend}" || argv[6] == name) &&
                              (argv[7] == "{harness}" || argv[7] == entry.harness)) {
            wanted[query][name] = entry.harness;
            routes.emplace(name, query);
        }
    }
    bool rosterChanged = owners_ != routes;
    owners_ = std::move(routes);
    for (const auto& route : wanted) queries_.try_emplace(route.first);

    problem_.clear();
    addresses_.clear();
    for (auto it = queries_.begin(); it != queries_.end();) {
        const auto& command = it->first;
        Query& state = it->second;
        const auto owners = wanted.find(command);
        if (state.pending.valid() && state.pending.wait_for(0ms) == std::future_status::ready) {
            auto result = state.pending.get();
            Roster roster;
            if (result.end == CommandResult::End::Exited && result.code == 0 && read(result.out, roster)) {
                if (!state.roster || *state.roster != roster) {
                    state.roster = std::move(roster);
                    rosterChanged = true;
                }
                state.completed = state.started;
                state.problem.clear();
                state.next = nowMs + 30000;
            } else {
                state.problem = "Cannot read the message bus roster. Verify nexus_policy bus-roster for profile " +
                                command.back() + " and its nova-config registry connection.";
                state.next = nowMs + 5000;
            }
        }
        if (owners == wanted.end()) {
            state.roster.reset();
            state.deliveries.clear();
            if (!state.pending.valid()) {
                it = queries_.erase(it);
                continue;
            }
        } else {
            std::erase_if(state.deliveries, [&](const auto& pending) { return !owners->second.contains(pending.first); });
            const bool requested = std::any_of(state.deliveries.begin(), state.deliveries.end(),
                [&](const auto& pending) { return pending.second.required > state.started; });
            if (!state.pending.valid() && (nowMs >= state.next || (requested && state.problem.empty()))) {
                const Command lookup{command[0], {command.begin() + 1, command.end()}, "nexus_policy bus-roster"};
                try {
                    state.pending = std::async(std::launch::async, run_, lookup, 6);
                    ++state.started;
                } catch (const std::system_error&) {
                    state.problem = "Cannot start the registry reader for message bus profile " + command.back() +
                                    ". Check available processes and threads on the server.";
                    state.next = nowMs + 5000;
                }
            }
            if (!state.problem.empty()) {
                if (!problem_.empty()) problem_ += '\n';
                problem_ += state.problem;
            }
            if (state.problem.empty() && state.roster) {
                for (const auto& [name, harness] : owners->second) {
                    auto grant = state.roster->find(name);
                    if (grant != state.roster->end() && grant->second.first == harness && !grant->second.second.empty())
                        addresses_.emplace(name, grant->second.second);
                }
            }
        }
        ++it;
    }
    if (rosterChanged) {
        knownSenders_.clear();
        for (const auto& [recipient, program] : owners_) {
            const auto& roster = queries_.at(program).roster;
            if (!roster) continue;
            auto& names = knownSenders_[recipient];
            for (const auto& row : *roster) names.insert(row.first);
        }
    }
}
} // namespace nexus::server
