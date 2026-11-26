// Emits observations from a real Hub. The same person entry and partial reply
// must survive every rotation; a pin and the last exhausted payer both stop it.
#include "accounts/registry.h"
#include "sync/hub.h"

#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

namespace {

int modelLimitTrace(bool switchAccount, const std::string& path) {
    using namespace nexus;
    sync::MemoryStore store;
    sync::DriverState pause;
    pause.modelLimitPending = true;
    pause.nextTurn = 4;
    pause.continuationPrompt = "unfinished";
    pause.exhaustedAccounts = {"first"};
    if (!store.commit(1, {sync::infoChange({"ai", "claude", "first", "fable", ""}),
                          sync::SessionChange{nullptr, {}, pause}}, {}, 0, nullptr))
        return 1;
    sync::Hub hub(store);
    accounts::Registry roster;
    roster.accounts = {{"first", "claude"}, {"second", "claude"}};
    roster.ais = {{"ai", {"first", "second"}, "first"}};
    hub.useAccounts(roster, nullptr);
    hub.reportSignedIn("first", true);
    hub.reportSignedIn("second", true);

    const auto offer = hub.state().find(1)->info;
    const std::string choice = switchAccount ? "account" : "model";
    sync::Action select{1, sync::ActionKind::ResolveLimit, 1,
                        switchAccount ? offer.limitAccount : offer.limitModel, choice, offer.account};
    select.request = offer.limitRequest;

    std::ofstream out(path);
    out << "---- MODULE CreditHandoffData ----\nTrace == <<\n";
    auto record = [&](const sync::Hub& observer, bool fresh) {
        const auto& session = *store.find(1);
        out << "[choice |-> " << std::quoted(choice) << ", fresh |-> " << (fresh ? "TRUE" : "FALSE")
            << ", account |-> " << std::quoted(session.info.account) << ", model |-> " << std::quoted(session.info.model)
            << ", work |-> " << std::quoted(session.driver.continuationPrompt)
            << ", pending |-> " << (session.driver.modelLimitPending ? "TRUE" : "FALSE")
            << ", notices |-> " << session.transcript.size() << ", request |-> " << observer.state().find(1)->info.limitRequest << "]";
    };
    record(hub, false);
    auto stale = select;
    --stale.request;
    hub.apply(stale);
    out << ",\n";
    record(hub, false);
    hub.apply(select);
    out << ",\n";
    record(hub, true);
    // The second window reconnects after the store committed the first window's choice.
    sync::Hub restarted(store);
    restarted.apply(select);
    out << ",\n";
    record(restarted, true);
    out << "\n>>\n====\n";
    return out ? 0 : 1;
}

} // namespace

int creditHandoffTrace(const std::string& mode, const std::string& path) {
    if (mode == "fable-account" || mode == "fable-model")
        return modelLimitTrace(mode == "fable-account", path);
    using namespace nexus;
    sync::MemoryStore store;
    sync::Hub hub(store);
    accounts::Registry roster;
    roster.accounts = {{"a", "claude"}, {"api", "claude", accounts::Billing::Api},
                       {"b", "claude"}, {"out", "claude"}, {"c", "claude"}};
    roster.ais = {{"ai", {"a", "api", "b", "out", "c"}, "a"}};
    hub.useAccounts(roster, nullptr);
    for (const auto* account : {"a", "api", "b", "c"})
        hub.reportSignedIn(account, true);
    if (!hub.apply({1, sync::ActionKind::Create, 0, "ai", "", ""}))
        return 1;
    const auto id = store.ids().front();
    if (mode == "pin" && !hub.apply({2, sync::ActionKind::SetAccount, id, "", "a", ""}))
        return 1;
    auto person = std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::Person, "request"});
    auto partial = std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::AI, "partial"});
    if (!hub.commit(id, {{person, {}}, {partial, {}}}))
        return 1;
    std::ofstream out(path);
    out << "---- MODULE CreditHandoffData ----\nExpectedPin == " << (mode == "pin" ? "TRUE" : "FALSE")
        << "\nTrace == <<\n";
    std::vector<std::string> exhausted;
    bool stopped = false;
    // Bound the emitter independently of the implementation. A faulty infinite
    // loop yields a finite unfinished trace that the model rejects.
    for (std::size_t step = 0; step <= roster.accounts.size(); ++step) {
        const auto& session = *store.find(id);
        if (step > 0)
            out << ",\n";
        out << "[session |-> " << id << ", account |-> " << std::quoted(session.info.account)
            << ", pinned |-> " << (session.info.autoAccount ? "FALSE" : "TRUE") << ", eligible |-> {";
        bool first = true;
        for (const auto& name : hub.accounts()->aiNamed("ai")->accounts) {
            if (hub.accounts()->account(name)->billing == accounts::Billing::Subscription && hub.signedIn().contains(name)) {
                if (!first) out << ", ";
                out << std::quoted(name);
                first = false;
            }
        }
        out << "}, stopped |-> " << (stopped ? "TRUE" : "FALSE") << ", content |-> <<";
        first = true;
        for (const auto& e : session.transcript) {
            if (e->kind == EntryKind::Person || e->kind == EntryKind::AI) {
                if (!first) out << ", ";
                out << "<<" << std::quoted(e->kind == EntryKind::Person ? "person" : "ai")
                    << ", " << std::quoted(e->text) << ">>";
                first = false;
            }
        }
        out << ">>]";
        if (stopped)
            break;
        exhausted.push_back(session.info.account);
        stopped = !hub.rotate(id, nullptr, exhausted);
    }
    out << "\n>>\n====\n";
    return out ? 0 : 1;
}
