// Record the real Hub's bus admissions on a batched DurableStore. The disk projection
// reopens a copy of MemFs after dropping every unsynced write.
#include "server/durable.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

void writePeerTrace(std::ostream&, nexus::sync::Hub&, const nexus::server::DurableStore&);

namespace {
using namespace nexus;
using namespace nexus::sync;
using server::DurableStore;

constexpr auto messageId = "1000-0";
constexpr const char* inboxes[] = {"ada", "bo"};

class Driver final : public Responder {
public:
    SessionInfo defaults(const std::string& name) const override { return {name, "driver", {}, {}, {}}; }
    std::vector<SessionChange> prepareBus(SessionId, const std::string&, std::string&) override { return {}; }
    std::vector<SessionChange> send(SessionId, const std::string&) override { throw std::logic_error("human send"); }
    std::vector<SessionChange> queue(SessionId, const std::string&) override { throw std::logic_error("human queue"); }
    std::vector<SessionChange> stop(SessionId) override { return {}; }
    void sent(SessionId, bool) override {}
    void configured(SessionId, const SessionInfo&) override {}
};

std::string projection(const DurableStore& store) {
    std::string result = "<<";
    for (const auto* name : inboxes) {
        std::size_t visible = 0;
        for (const auto id : store.ids()) {
            const auto* session = store.find(id);
            if (session->info.aiName == name)
                visible += std::count_if(session->transcript.begin(), session->transcript.end(),
                    [](const EntryRef& entry) { return entry->kind == EntryKind::Bus; });
        }
        if (result.size() > 2) result += ", ";
        const auto receipt = store.applied(std::string("nexus:bus:in:") + name + ":" + messageId);
        result += "<<" + std::to_string(receipt) + ", " + std::to_string(visible) + ">>";
    }
    return result + ">>";
}

void writeTrace(std::ostream& out) {
    store::MemFs fs;
    Driver driver;
    std::unique_ptr<DurableStore> store;
    std::unique_ptr<Hub> hub;
    std::string why;
    auto open = [&] {
        store = std::make_unique<DurableStore>();
        if (!store->open(fs, "store", &why)) throw std::runtime_error(why);
        hub = std::make_unique<Hub>(*store, Limits{}, &driver);
        accounts::Registry registry;
        for (const auto* name : inboxes) registry.ais.push_back({name, {}, {}, {}});
        hub->useAccounts(registry, nullptr);
    };
    open();
    out << "---- MODULE BusDeliveryData ----\nTrace == <<\n";
    auto record = [&](const char* op, const std::string& inbox = {}) {
        store::MemFs crashed(fs);
        crashed.crash(crashed.plan_all(false));
        DurableStore disk;
        if (!disk.open(crashed, "store", &why)) throw std::runtime_error(why);
        if (std::string_view(op) != "init") out << ",\n";
        out << "[op |-> \"" << op << "\", inbox |-> \"" << inbox << "\", mem |-> " << projection(*store)
            << ", disk |-> " << projection(disk) << "]";
    };
    auto receive = [&](const std::string& inbox) {
        store->beginBatch();
        if (!hub->receiveBus(inbox, messageId, "sender", "hello", "hello", &why)) throw std::runtime_error(why);
        record("receive", inbox);
    };
    auto flush = [&] {
        if (!store->flush(&why)) throw std::runtime_error(why);
        record("flush");
    };
    auto restart = [&] {
        hub.reset();
        store.reset();
        fs.crash(fs.plan_all(false));
        open();
        record("restart");
    };

    record("init");
    receive("ada");
    restart();                  // admission did not reach disk
    receive("ada");
    flush();
    restart();                  // admission reached disk, reply lost
    receive("ada");
    flush();
    restart();
    store->beginBatch();
    SessionChange clear{};
    clear.clearTranscript = true;
    const auto* session = hub->state().first("ada");
    if (!session || !store->commit(session->id, {clear}, {}, 0, &why)) throw std::runtime_error(why);
    record("clear", "ada");
    flush();
    receive("ada");             // the bubble was cleared, its receipt was retained
    receive("bo");              // identical stream ID in another inbox
    flush();
    restart();
    out << "\n>>\n====\n";
}
} // namespace

int main(int argc, char** argv) {
    const bool peer = argc == 3 && std::string_view(argv[1]) == "--peer-roster";
    if (!peer && argc != 2) return 2;
    try {
        std::ofstream out(argv[argc - 1]);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        if (peer) {
            store::MemFs fs;
            DurableStore store;
            std::string why;
            if (!store.open(fs, "store", &why)) throw std::runtime_error(why);
            Driver driver;
            Hub hub(store, Limits{}, &driver);
            accounts::Registry roster;
            roster.ais.push_back({"ada", {}, {}, {}});
            hub.useAccounts(roster, nullptr);
            writePeerTrace(out, hub, store);
        } else {
            writeTrace(out);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
