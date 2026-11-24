#include "server/bus_grants.h"

#include "accounts/registry.h"

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
    return {argv[0], "-I", "-m", "nexus_policy", "bus-grants", argv[5]};
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

bool BusGrants::read(const std::string& text, Grants& grants) {
    // A profile writes one row per enabled friend with a bus grant. Validate before applying any.
    if (text.size() > 64 * 1024) return false;
    Grants parsed;
    std::string_view rest(text);
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
        if (!accounts::isName(name) || !accounts::isProvider(harness) || !endpoint(address) ||
            !parsed.try_emplace(std::string(name), harness, address).second) return false;
    }
    grants = std::move(parsed);
    return true;
}

void BusGrants::refresh(const DriverTable& table, std::int64_t nowMs) {
    using namespace std::chrono_literals;
    std::map<std::vector<std::string>, std::map<std::string, std::string>> wanted;
    for (const auto& [name, entry] : table.friends) {
        const auto driver = table.drivers.find(entry.harness);
        if (driver == table.drivers.end()) continue;
        const auto& argv = driver->second;
        const auto query = queryFor(argv);
        if (!query.empty() && (argv[6] == "{friend}" || argv[6] == name) &&
                              (argv[7] == "{harness}" || argv[7] == entry.harness))
            wanted[query][name] = entry.harness;
    }
    for (const auto& route : wanted) queries_.try_emplace(route.first);

    problem_.clear();
    addresses_.clear();
    for (auto it = queries_.begin(); it != queries_.end();) {
        const auto& command = it->first;
        Query& state = it->second;
        const auto owners = wanted.find(command);
        if (state.pending.valid() && state.pending.wait_for(0ms) == std::future_status::ready) {
            auto result = state.pending.get();
            if (result.end == CommandResult::End::Exited && result.code == 0 && read(result.out, state.grants)) {
                state.problem.clear();
                state.next = nowMs + 30000;
            } else {
                state.grants.clear();
                state.problem = "Cannot read message bus grants. Verify nexus_policy bus-grants for profile " +
                                command.back() + " and its nova-config registry connection.";
                state.next = nowMs + 5000;
            }
        }
        if (owners == wanted.end()) {
            state.grants.clear();
            if (!state.pending.valid()) {
                it = queries_.erase(it);
                continue;
            }
        } else {
            if (!state.pending.valid() && nowMs >= state.next) {
                const Command lookup{command[0], {command.begin() + 1, command.end()}, "nexus_policy bus-grants"};
                try {
                    state.pending = std::async(std::launch::async, run_, lookup, 6);
                } catch (const std::system_error&) {
                    state.grants.clear();
                    state.problem = "Cannot start the registry reader for message bus profile " + command.back() +
                                    ". Check available processes and threads on the server.";
                    state.next = nowMs + 5000;
                }
            }
            if (!state.problem.empty()) {
                if (!problem_.empty()) problem_ += '\n';
                problem_ += state.problem;
            }
            for (const auto& [name, harness] : owners->second) {
                auto grant = state.grants.find(name);
                if (grant != state.grants.end() && grant->second.first == harness)
                    addresses_.emplace(name, grant->second.second);
            }
        }
        ++it;
    }
}
} // namespace nexus::server
