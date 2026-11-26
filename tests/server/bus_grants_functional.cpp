// Exercise sender decisions and per-profile refreshes without starting a registry, hook or provider.
#include "server/bus_grants.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace nexus;
using namespace nexus::server;

namespace {
#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (false)

const std::string header = "nexus-bus-roster-v1\n";
const std::string owners = "review\tcodex\t127.0.0.1:6379\nauthor\tclaude\t127.0.0.1:6379\n";

CommandResult success(const std::string& output) {
    return CommandResult{CommandResult::End::Exited, 0, output, {}};
}
} // namespace

int main() {
    DriverTable table;
    table.drivers["codex"] = {"/policy", "-I", "-m", "nexus_policy", "ai-driver", "/profile",
                              "{ai}", "{harness}", "/driver"};
    table.ais["review"] = {"codex", "/working"};
    table.ais["author"] = {"codex", "/work"};  // its registry harness does not match this driver
    table.drivers["claude"] = table.drivers["codex"];
    table.ais["admin"] = {"claude", "/admin"};
    table.drivers["muse"] = table.drivers["codex"];
    table.drivers["muse"][5] = "/other";
    table.ais["writer"] = {"muse", "/other-work"};

    enum Revision { Initial, HeldOld, Updated, Legacy, Invalid };
    std::atomic<Revision> revision{Initial};
    std::atomic<bool> oldQueryCaptured{false};
    std::promise<void> releaseOld;
    const auto gate = releaseOld.get_future().share();
    BusGrants subscriptions([&](const Command& command, int timeout) {
        const auto& args = command.arguments;
        if (command.program != "/policy" || args.size() != 6 || args[5] != "--startup-policy" || args[0] != "-I" || args[1] != "-m" ||
                args[2] != "nexus_policy" || args[3] != "bus-roster" || timeout <= 0)
            return CommandResult{};
        if (args[4] == "/other") return success(header + "writer\tmuse\t127.0.0.1:6380\nlocal-only\tmuse\t\n");
        if (args[4] != "/profile") return CommandResult{};
        switch (revision.load()) {
            case HeldOld:
                oldQueryCaptured = true;
                if (gate.wait_for(3s) != std::future_status::ready) return CommandResult{};
                [[fallthrough]];
            case Initial: return success(header + owners + "former\tmuse\t\n");
            case Updated: return success(header + owners + "admin\tclaude\t\tno-sandbox\nnew-peer\tclaude\t\n");
            case Legacy: return success(owners);
            case Invalid: return success(header + owners + "extra\tclaude\t192.0.2.1:6379\n");
        }
        return CommandResult{};
    });
    const auto until = [&](std::int64_t now, auto done) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            subscriptions.refresh(table, now);
            if (done()) return true;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };

    const auto decision = [&](std::int64_t now, const char* inbox, const char* id, const char* peer, BusPeer expected) {
        auto answer = BusPeer::Pending;
        const bool settled = until(now, [&] {
            answer = subscriptions.sender(inbox, id, peer);
            return answer != BusPeer::Pending;
        });
        return settled && answer == expected;
    };

    CHECK(subscriptions.knownSenders().empty());
    CHECK(until(1, [&] { return subscriptions.addresses().size() == 2; }));
    CHECK(subscriptions.addresses().at("review") == "127.0.0.1:6379");
    CHECK(subscriptions.addresses().at("writer") == "127.0.0.1:6380");
    CHECK(!subscriptions.addresses().contains("author"));
    CHECK(subscriptions.knownSenders().at("review").contains("former"));
    CHECK(!subscriptions.knownSenders().at("review").contains("local-only"));
    CHECK(subscriptions.knownSenders().at("writer").contains("local-only"));
    CHECK(!subscriptions.knownSenders().at("writer").contains("former"));

    // A sender registered after the periodic lookup started must survive both the cached negative
    // and the in-flight negative. Only a successful lookup started for this delivery can classify it.
    revision = HeldOld;
    CHECK(until(31000, [&] { return oldQueryCaptured.load(); }));
    CHECK(subscriptions.sender("review", "200-0", "new-peer") == BusPeer::Pending);
    revision = Updated;
    releaseOld.set_value();
    CHECK(decision(31001, "review", "200-0", "new-peer", BusPeer::AI));
    CHECK(subscriptions.unsandboxed().contains("admin"));
    CHECK(!subscriptions.addresses().contains("new-peer"));  // enabled in nova-config, no local inbox or driver
    CHECK(subscriptions.knownSenders().at("review").contains("new-peer"));
    CHECK(!subscriptions.knownSenders().at("review").contains("former"));

    CHECK(subscriptions.sender("review", "201-0", "local-only") == BusPeer::Pending);
    CHECK(decision(31002, "review", "201-0", "local-only", BusPeer::Other));
    CHECK(subscriptions.sender("writer", "201-0", "local-only") == BusPeer::Pending);
    CHECK(decision(31003, "writer", "201-0", "local-only", BusPeer::AI));

    // An outdated policy returning grants alone cannot establish that a sender is unknown. Ignore
    // the entire response on parse failure and keep the last confirmed roster for history.
    revision = Legacy;
    CHECK(subscriptions.sender("review", "202-0", "new-peer") == BusPeer::Pending);
    CHECK(until(31004, [&] { return !subscriptions.problem().empty(); }));
    CHECK(!subscriptions.addresses().contains("review"));
    CHECK(subscriptions.knownSenders().at("review").contains("new-peer"));
    CHECK(subscriptions.sender("review", "202-0", "new-peer") == BusPeer::Pending);
    revision = Updated;
    CHECK(decision(37000, "review", "202-0", "new-peer", BusPeer::AI));
    CHECK(subscriptions.problem().empty());

    revision = Invalid;
    CHECK(subscriptions.sender("review", "203-0", "extra") == BusPeer::Pending);
    CHECK(until(37001, [&] { return !subscriptions.problem().empty(); }));
    CHECK(!subscriptions.addresses().contains("review"));
    CHECK(!subscriptions.knownSenders().at("review").contains("extra"));
    CHECK(subscriptions.sender("review", "203-0", "extra") == BusPeer::Pending);
    revision = Updated;
    CHECK(decision(43000, "review", "203-0", "extra", BusPeer::Other));

    // A peer removed since the last successful snapshot must not pass on its old membership.
    revision = Initial;
    CHECK(subscriptions.sender("review", "204-0", "new-peer") == BusPeer::Pending);
    CHECK(decision(43001, "review", "204-0", "new-peer", BusPeer::Other));

    CHECK(subscriptions.unsandboxed().empty());

    // Changing a fork's launcher must release inboxes that the fixed launcher arguments no longer cover.
    table.drivers.at("codex")[6] = "author";
    subscriptions.refresh(table, 43001);
    CHECK(!subscriptions.addresses().contains("review"));
    table.drivers.clear();
    subscriptions.refresh(table, 43002);
    CHECK(subscriptions.addresses().empty());
    CHECK(subscriptions.knownSenders().empty());
    std::puts("server bus recipient grants, sender rosters, freshness and isolation passed");
    return 0;
}
