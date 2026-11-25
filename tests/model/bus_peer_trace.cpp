// Trace the server's real asynchronous roster classifier and its two durable admission paths.
// Replies from the registry are released one by one so a lookup already in flight can finish late.
#include "server/bus_grants.h"
#include "server/durable.h"

#include <array>
#include <atomic>
#include <chrono>
#include <ostream>
#include <stdexcept>
#include <thread>

using namespace nexus;
using namespace std::chrono_literals;

void writePeerTrace(std::ostream& out, sync::Hub& hub, const server::DurableStore& store) {
    std::array<std::promise<CommandResult>, 10> replies;
    std::atomic<std::size_t> next = 0;
    server::BusGrants grants([&](const Command&, int) {
        auto reply = replies.at(next++).get_future();
        if (reply.wait_for(3s) != std::future_status::ready) return CommandResult{};
        return reply.get();
    });
    server::DriverTable table;
    table.drivers["codex"] = {"/policy", "-I", "-m", "nexus_policy", "ai-driver", "/profile",
                              "{ai}", "{harness}", "/driver"};
    table.ais["ada"] = {"codex", "/work"};

    constexpr auto header = "nexus-bus-roster-v1\n";
    constexpr auto reader = "ada\tcodex\t127.0.0.1:6379\n";
    constexpr auto address = "127.0.0.1:6379";
    const auto idOf = [](int number) { return std::to_string(number) + "-0"; };
    const auto hasReceipt = [&](const std::string& id) {
        return hub.hasBusReceipt("ada", id) || hub.hasIgnoredBusReceipt("ada", id, address);
    };
    const auto known = [&] {
        const auto owner = grants.knownSenders().find("ada");
        return owner != grants.knownSenders().end() && owner->second.contains("peer");
    };
    int active = 0;
    std::string decision = "wait";
    sync::Sessions previous, client;
    auto writeSet = [&](auto contains) {
        out << "{";
        bool comma = false;
        for (int n : {1, 2}) {
            if (!contains(n)) continue;
            out << (comma ? ", " : "") << n;
            comma = true;
        }
        out << "}";
    };
    auto record = [&](const std::string& op, const std::string& answer = "", bool fresh = false) {
        if (op != "init") out << ",\n";
        out << "[op |-> \"" << op << "\", id |-> " << active << ", answer |-> \"" << answer
            << "\", fresh |-> " << (fresh ? "TRUE" : "FALSE") << ", decision |-> \"" << decision
            << "\", known |-> " << (known() ? "TRUE" : "FALSE") << ", receipts |-> ";
        writeSet([&](int n) { return hasReceipt(idOf(n)); });
        out << ", stored |-> ";
        writeSet([&](int n) {
            for (auto id : store.ids())
                for (const auto& e : store.find(id)->transcript)
                    if (e->kind == EntryKind::Bus && e->text == "peer\n" + idOf(n)) return true;
            return false;
        });
        out << ", shown |-> ";
        client.apply_string(hub.state().diff_from(previous));
        previous = hub.state();
        const auto* session = client.first("ada");
        writeSet([&](int n) {
            if (!session) return false;
            for (const auto& e : session->entries)
                if (e->kind == EntryKind::Bus && e->text == "peer\n" + idOf(n)) return true;
            return false;
        });
        out << "]";
    };
    auto offer = [&](int n) {
        active = n;
        if (!hasReceipt(idOf(n))) grants.sender("ada", idOf(n), "peer");
        record("offer");
    };
    auto decide = [&] {
        const auto id = idOf(active);
        if (hasReceipt(id)) decision = "replay";
        else {
            std::string why;
            switch (grants.sender("ada", id, "peer")) {
            case server::BusPeer::Pending: decision = "wait"; break;
            case server::BusPeer::Other:
                // BusInbox takes this path when private retention has finished. Its functional
                // test exercises the filesystem. Here the receipt is observed on the real store.
                if (!hub.ignoreBus("ada", id, &why, address)) throw std::runtime_error(why);
                decision = "other";
                break;
            case server::BusPeer::AI:
                if (!hub.receiveBus("ada", id, "peer", id, id, &why)) throw std::runtime_error(why);
                decision = "ai";
                break;
            }
        }
        record("decide");
    };
    std::int64_t now = 0;
    std::size_t completed = 0;
    auto lookup = [&](std::string result, const char* answer, bool fresh, auto observed) {
        now += 31000;
        grants.refresh(table, now);
        replies.at(completed++).set_value({CommandResult::End::Exited, 0, std::move(result), {}});
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            grants.refresh(table, now);
            if (observed()) {
                hub.setBusSenders(grants.knownSenders());
                record("lookup", answer, fresh);
                return;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("timed out waiting for the simulated registry");
    };
    out << "---- MODULE BusPeerFilterData ----\nTrace == <<\n";
    record("init");
    lookup(std::string(header) + reader, "other", false, [&] { return grants.addresses().contains("ada"); });
    now += 31000;
    grants.refresh(table, now);                         // old snapshot already started
    offer(1);
    decide();
    lookup(std::string(header) + reader + "peer\tclaude\t\n", "ai", false, known);
    decide();                                          // historical membership cannot authorize this delivery
    lookup("", "unavailable", true, [&] { return !grants.problem().empty(); });
    decide();
    lookup(std::string(header) + reader, "other", true, [&] { return !known() && grants.problem().empty(); });
    decide();                                          // recipient granted, peer absent from complete roster
    offer(2);
    decide();
    lookup(std::string(header) + reader + "peer\tmuse\t\n", "ai", true, known);
    decide();                                          // newly registered peer, new message
    offer(1);
    decide();                                          // same peer, originally filtered ID
    lookup("", "unavailable", false, [&] { return !grants.problem().empty(); });
    lookup(header, "empty", false, [&] { return !known() && grants.problem().empty(); });
    lookup(std::string(header) + reader + "peer\tmuse\t\n", "ai", false, known);
    out << "\n>>\n====\n";
}
