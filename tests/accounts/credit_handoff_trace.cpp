// Emits observations from a real Hub. The same person entry and partial reply
// must survive every rotation; a pin and the last exhausted payer both stop it.
#include "accounts/registry.h"
#include "sync/hub.h"

#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

int creditHandoffTrace(const std::string& mode, const std::string& path) {
    using namespace nexus;
    sync::MemoryStore store;
    sync::Hub hub(store);
    accounts::Registry roster;
    roster.accounts = {{"a", "claude"}, {"api", "claude", accounts::Billing::Api},
                       {"b", "claude"}, {"out", "claude"}, {"c", "claude"}};
    roster.friends = {{"friend", {"a", "api", "b", "out", "c"}, "a"}};
    hub.useAccounts(roster, nullptr);
    for (const auto* account : {"a", "api", "b", "c"})
        hub.reportSignedIn(account, true);
    if (!hub.apply({1, sync::ActionKind::Create, 0, "friend", "", ""}))
        return 1;
    const auto id = store.ids().front();
    if (mode == "pin" && !hub.apply({2, sync::ActionKind::SetAccount, id, "", "a", ""}))
        return 1;
    auto person = std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::Person, "request"});
    auto partial = std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::Friend, "partial"});
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
        for (const auto& name : hub.accounts()->friendNamed("friend")->accounts) {
            if (hub.accounts()->account(name)->billing == accounts::Billing::Subscription && hub.signedIn().contains(name)) {
                if (!first) out << ", ";
                out << std::quoted(name);
                first = false;
            }
        }
        out << "}, stopped |-> " << (stopped ? "TRUE" : "FALSE") << ", content |-> <<";
        first = true;
        for (const auto& e : session.transcript) {
            if (e->kind == EntryKind::Person || e->kind == EntryKind::Friend) {
                if (!first) out << ", ";
                out << "<<" << std::quoted(e->kind == EntryKind::Person ? "person" : "friend")
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
