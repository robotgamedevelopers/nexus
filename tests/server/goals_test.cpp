#include "sync/hub.h"
#include "sync/codec.h"
#include "server/durable.h"
#include "store/fs.h"
#include "policy/host.h"
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace nexus;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); ++failures; } } while (0)
struct Store : sync::MemoryStore {
    bool reject = false;
    bool commit(sync::SessionId id, const std::vector<sync::SessionChange>& changes, const std::string& client,
                std::uint64_t num, std::string* why) override {
        return !reject && MemoryStore::commit(id, changes, client, num, why);
    }
};
struct Responder : sync::Responder {
    Store& store;
    int effects = 0;
    explicit Responder(Store& value) : store(value) {}
    sync::SessionInfo defaults(const std::string& ai) const override { return {ai, "test", {}, {}, {}}; }
    std::vector<sync::SessionChange> send(sync::SessionId, const std::string& text) override {
        return {{std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::Person, text, true}), {}}};
    }
    std::vector<sync::SessionChange> queue(sync::SessionId, const std::string& text) override { return {{nullptr, text}}; }
    std::vector<sync::SessionChange> stop(sync::SessionId) override { return {}; }
    void configured(sync::SessionId, const sync::SessionInfo&) override {}
    void sent(sync::SessionId id, bool committed) override {
        if (!committed) return;
        const auto* session = store.find(id);
        CHECK(!session->info.goal.empty() && session->info.goalNextMs > 0);
        CHECK(session->transcript.back()->text == session->info.goal);
        ++effects;
    }
};
struct Fixture {
    Store store;
    std::unique_ptr<sync::Hub> hub;
    std::uint64_t now = 1000;
    Fixture(const std::string& harness = "claude") {
        sync::SessionInfo info{"test", harness, {}, {}, {}};
        CHECK(store.commit(1, {sync::infoChange(info)}, {}, 0, nullptr));
        restart();
    }
    void restart() {
        hub = std::make_unique<sync::Hub>(store);
        hub->goalsWith([this] { return now; });
    }
    bool command(const std::string& text) {
        sync::Action a;
        a.kind = sync::ActionKind::Send; a.session = 1; a.text = text;
        std::string why;
        const bool ok = hub->apply(a, &why);
        if (!ok && !store.reject) std::fprintf(stderr, "%s\n", why.c_str());
        return ok;
    }
    const sync::SessionInfo& info() const { return store.find(1)->info; }
    std::size_t sends() const {
        std::size_t n = 0;
        for (const auto& e : store.find(1)->transcript) if (e->kind == EntryKind::Person) ++n;
        return n;
    }
};
void checks() {
    for (const auto* harness : {"claude", "codex", "antigravity", "grok", "muse"}) {
        Fixture f(harness);
        CHECK(f.command("/goal keep working"));
        CHECK(f.info().goal == "keep working" && f.info().goalNextMs == 301000 && f.sends() == 1);
        auto wire = f.hub->state().init_diff();
        sync::Sessions decoded; decoded.apply_string(wire);
        CHECK(decoded.find(1)->info.goal == "keep working" && decoded.find(1)->info.goalNextMs == 301000);
        f.now = 300999; CHECK(!f.hub->serveGoals() && f.sends() == 1);
        f.now = 301000; CHECK(f.hub->serveGoals() && f.sends() == 2);
        CHECK(!f.hub->serveGoals() && f.sends() == 2);
        f.now += 20 * sync::Hub::kGoalIntervalMs;
        f.restart(); CHECK(f.hub->serveGoals() && f.sends() == 3);
        CHECK(!f.hub->serveGoals());
        CHECK(f.command("/goal") && f.sends() == 3);
        CHECK(f.command("/goal /clear") && f.info().goal == "/clear" && f.sends() == 4);
        f.now += sync::Hub::kGoalIntervalMs;
        CHECK(f.hub->serveGoals() && f.sends() == 5); // literal text, never reparsed as a Nexus command
        CHECK(f.command("/goal off") && f.info().goal.empty() && f.info().goalNextMs == 0);
        f.now += sync::Hub::kGoalIntervalMs; CHECK(!f.hub->serveGoals() && f.sends() == 5);
    }
    Fixture f;
    CHECK(f.command("/goal original"));
    f.store.reject = true;
    CHECK(!f.command("/goal replacement"));
    CHECK(f.info().goal == "original" && f.sends() == 1);
    f.now = f.info().goalNextMs;
    f.hub->serveGoals(); CHECK(f.sends() == 1 && f.info().goalNextMs == f.now);
    f.store.reject = false;
    CHECK(f.hub->serveGoals() && f.sends() == 2);
    sync::DriverState waiting; waiting.waiting = {"original"};
    CHECK(f.store.commit(1, {{nullptr, {}, waiting}}, {}, 0, nullptr));
    f.now = f.info().goalNextMs;
    CHECK(f.hub->serveGoals() && f.sends() == 2 && f.info().goalNextMs == f.now + sync::Hub::kGoalIntervalMs);
    sync::Action create; create.kind = sync::ActionKind::Create; create.aiName = "test"; create.session = 1;
    CHECK(f.hub->apply(create) && f.store.find(2)->info.goal.empty());

    // Exercise the driver boundary: dispatch observes the durable goal and prompt together, and failed writes
    // dispatch nothing.
    Fixture driven;
    Responder responder(driven.store);
    driven.hub = std::make_unique<sync::Hub>(driven.store, sync::Limits{}, &responder);
    driven.hub->goalsWith([&] { return driven.now; });
    sync::Action set;
    set.kind = sync::ActionKind::Send; set.session = 1; set.text = "/goal durable work"; set.num = 1;
    CHECK(driven.hub->apply(set, nullptr, "client") && responder.effects == 1);
    driven.now = driven.info().goalNextMs;
    driven.store.reject = true;
    driven.hub->serveGoals();
    CHECK(responder.effects == 1 && driven.info().goalNextMs == driven.now);
    driven.store.reject = false;
    CHECK(driven.hub->serveGoals() && responder.effects == 2);
    driven.store.reject = true;
    CHECK(!driven.command("/goal off") && !driven.info().goal.empty());

    store::MemFs fs;
    for (const bool compact : {false, true}) {
        const auto dir = compact ? "compact" : "journal";
        store::StoreOptions options; if (compact) options.compact_bytes = 1;
        {
            server::DurableStore disk; std::string why;
            CHECK(disk.open(fs, dir, &why, options));
            auto info = f.info(); info.goal = "persisted"; info.goalNextMs = 123456789;
            CHECK(disk.commit(1, {sync::infoChange(info)}, {}, 0, &why));
        }
        {
            server::DurableStore disk; std::string why;
            CHECK(disk.open(fs, dir, &why));
            CHECK(disk.find(1)->info.goal == "persisted" && disk.find(1)->info.goalNextMs == 123456789);
            auto info = disk.find(1)->info; info.goal.clear(); info.goalNextMs = 0;
            CHECK(disk.commit(1, {sync::infoChange(info)}, {}, 0, &why));
        }
        server::DurableStore disk; std::string why; CHECK(disk.open(fs, dir, &why));
        CHECK(disk.find(1)->info.goal.empty() && disk.find(1)->info.goalNextMs == 0);
    }

}
void trace(const std::string& path, const std::string& fault) {
    Fixture f;
    std::ofstream out(path);
    out << "---- MODULE SessionGoalTraceData ----\nTrace == <<\n";
    bool first = true;
    auto step = [&](const std::string& event, std::uint64_t now, bool pending = false) {
        f.now = now;
        if (event == "set") f.command("/goal fixture");
        if (event == "off" && fault != "ignore_stop") f.command("/goal off");
        if (event == "restart") f.restart();
        if (event == "tick") {
            sync::DriverState driver;
            if (pending && fault != "pending") driver.waiting = {"fixture"};
            f.store.commit(1, {{nullptr, {}, driver}}, {}, 0, nullptr);
            if (fault == "early") ++f.now;
            f.hub->serveGoals();
        }
        out << (first ? "" : ",\n") << "[event |-> \"" << event << "\", now |-> " << now
            << ", pending |-> " << (pending ? "TRUE" : "FALSE") << ", active |-> " << (f.info().goal.empty() ? "FALSE" : "TRUE")
            << ", due |-> " << f.info().goalNextMs << ", sends |-> " << f.sends() << "]";
        first = false;
    };
    step("init", 1000); step("set", 1000); step("tick", 300999); step("tick", 301000);
    step("tick", 301000); step("restart", 900000); step("tick", 900000); step("tick", 1200000, true);
    step("set", 1200001); step("off", 1200002); step("tick", 1800000); step("restart", 2400000);
    step("tick", 2400000); step("set", 2400000); step("tick", 9000000);
    out << "\n>>\n====\n";
    CHECK(bool(out));
}
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--state") {
        Fixture f; f.command("/goal Keep improving the client and verify each change.");
        std::ofstream out(argv[2], std::ios::binary); out << sync::stateFile(f.hub->state());
    } else if (argc == 3) trace(argv[2], argv[1]); else checks();
    return failures ? 1 : 0;
}
