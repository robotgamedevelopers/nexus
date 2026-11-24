// The registry refresh runs off the serving loop. A fake policy chooses when its answer can complete.
#include "server/bus_grants.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

using namespace std::chrono_literals;
using namespace nexus;
using namespace nexus::server;

int main() {
    DriverTable table;
    table.drivers["codex"] = {"/policy", "-I", "-m", "nexus_policy", "friend-driver", "/profile",
                               "{friend}", "{harness}", "/driver"};
    table.friends["review"] = {"codex", "/working"};
    table.friends["author"] = {"codex", "/work"};
    std::atomic<int> runs{0};
    std::promise<void> ready;
    auto release = ready.get_future().share();
    BusGrants subscriptions([&](const Command& command, int timeout) {
        const std::vector<std::string> wanted{"-I", "-m", "nexus_policy", "bus-grants", "/profile"};
        if (command.program != "/policy" || command.arguments != wanted || timeout <= 0)
            return CommandResult{};
        const int run = ++runs;
        release.wait();
        return CommandResult{CommandResult::End::Exited, 0, run == 1 ?
            "review\tcodex\t127.0.0.1:6379\nauthor\tclaude\t127.0.0.1:6379\n" :
            "review\tcodex\t127.0.0.1:6390\nintruder\tclaude\t192.0.2.1:6379\n", {}};
    });
    const auto until = [&](std::int64_t now, auto done) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            subscriptions.refresh(table, now);
            if (done()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    };
    subscriptions.refresh(table, 0);
    ready.set_value();
    if (!until(1, [&] { return !subscriptions.addresses().empty(); })) return 1;
    if (subscriptions.addresses() != std::map<std::string, std::string>{{"review", "127.0.0.1:6379"}}) return 1;
    if (runs.load() != 1 || !subscriptions.problem().empty()) return 1;
    // A fork cannot inherit another fork's grant by changing a driver line's fixed arguments.
    table.drivers.at("codex")[6] = "author";
    subscriptions.refresh(table, 2);
    if (!subscriptions.addresses().empty()) return 1;
    table.drivers.at("codex")[6] = "{friend}";
    // A malformed profile response invalidates prior authority; no prefix of its rows is adopted.
    if (!until(30002, [&] { return !subscriptions.problem().empty(); })) return 1;
    if (!subscriptions.addresses().empty() || runs.load() != 2) return 1;
    table.drivers.clear();
    subscriptions.refresh(table, 30003);
    if (!subscriptions.problem().empty()) return 1;
    std::puts("server bus profile scope, ownership change, and invalid refresh passed");
}
