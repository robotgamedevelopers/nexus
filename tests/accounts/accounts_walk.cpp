// Trace walk: drives the hub's accounts and AIs (src/sync/hub.h over a MemoryStore, src/accounts/registry.h) with
// a seeded random walk and writes the trace as AccountsTraceData.tla, for TLC to check against models/Accounts.tla
// (models/AccountsTrace.tla). No files, no clock: the registry is the hub's, the sign-ins are reported by the walk.
//
//   accounts_walk <seed> <steps> [--faulty-stay | --faulty-harness | --faulty-instance | --faulty-limit] <out.tla>
//
// Each step is one of the model's operations, given to the code the way the server gives it: an account action from a
// window (add, remove, AI, link, unlink, default, set, create) through Hub::apply; a hand edit (dropai,
// droplink, dropaccount, provider, addinst, dropinst, moveinst) as the registry it leaves, through Hub::applyAccounts,
// the way `nexus-server config reload` applies the files; a sign-in reported (in, out) through Hub::reportSignedIn; a
// usage limit (rotate) through Hub::rotate; an AI's limit or an account's billing edited (limit, billing) as the
// registry it leaves; a session's driver started (start), limited or not as accounts::limitedFor decides from the
// hub's registry, the way the server decides it at every driver start (src/server/drivers.cpp), or ended (end). After it the walk records the code's whole state: each account absent,
// signed in or needing sign-in, its provider, the AIs with their links, defaults and instances, each session's
// AI, instance, account and harness, the next id, the sessions the step gave a notice (their transcripts' notices counted before and after), and whether the code took
// the step or refused it.
//
// An account's provider is the registry's while it is set up; while it is not, the walk keeps the one it had (or the
// instance's), as the model does, and sets one up again with it.
//
// The faults show the binding can fail; each is a hub that keeps a session where the model moves it, played by the
// walk writing the session's old settings back into the store after the step:
//   --faulty-stay      a session left on an account no longer linked to its AI
//   --faulty-harness   a session left on its old harness when its account's provider changed
//   --faulty-instance  a session left on an instance its AI no longer has
//   --faulty-limit     a driver started open for a limited AI on an account billed by subscription (a server that
//                      reads only the account's billing)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "sync/hub.h"

namespace {

using namespace nexus;

const std::vector<std::string> kAccts{"a1", "a2", "a3"};
const std::map<std::string, std::string> kProvider{{"a1", "claude"}, {"a2", "claude"}, {"a3", "codex"}};
const std::vector<std::string> kProviders{"claude", "codex"};
const std::vector<std::string> kAIs{"f", "g"};
const std::vector<std::string> kInsts{"i1", "i2"};
constexpr sync::SessionId kMaxSes = 3;
const char* const kStartHarness = "claude";    // the hub's harness for a session of no account (no responder)
const char* const kNone = "none";

std::uint64_t state = 0;
std::uint64_t next() {
    // xorshift64*
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ULL;
}
std::size_t pick(std::size_t n) { return static_cast<std::size_t>(next() % n); }
template <typename T> const T& pickOf(const std::vector<T>& v) { return v[pick(v.size())]; }

std::string q(const std::string& s) { return "\"" + s + "\""; }
std::string orNone(const std::string& s) { return s.empty() ? std::string(kNone) : s; }

struct Walk {
    sync::MemoryStore store;
    sync::Hub hub{store};
    std::map<std::string, std::string> provider = kProvider;   // an account's provider, kept while it is not set up
    std::uint64_t num = 0;
    // Each session's driver, as the walk started it: "off", "limited" or "open", with whether its account was billed at
    // API rate and its AI limited at the start (the model's run).
    struct Run {
        std::string st = "off";
        bool api = false;
        bool flim = false;
    };
    std::map<sync::SessionId, Run> runs;

    // An AI's limit as the model reads it: an AI not in the files reads limited (accounts::limitedFor).
    bool limitOf(const std::string& f) const {
        const accounts::AI* x = reg().aiNamed(f);
        return x == nullptr || x->limit;
    }
    bool apiBilled(const std::string& a) const {
        const accounts::Account* x = reg().account(a);
        return x != nullptr && x->billing == accounts::Billing::Api;
    }

    const accounts::Registry& reg() const { return *hub.accounts(); }

    std::string providerOf(const std::string& a) const {
        const accounts::Account* x = reg().account(a);
        return x != nullptr ? x->provider : provider.at(a);
    }

    std::map<sync::SessionId, std::size_t> notices() const {
        std::map<sync::SessionId, std::size_t> n;
        for (sync::SessionId id : store.ids()) {
            for (const sync::EntryRef& e : store.find(id)->transcript) {
                n[id] += e->kind == EntryKind::Notice ? 1 : 0;
            }
        }
        return n;
    }

    std::map<sync::SessionId, sync::SessionInfo> infos() const {
        std::map<sync::SessionId, sync::SessionInfo> m;
        for (sync::SessionId id : store.ids()) {
            m[id] = store.find(id)->info;
        }
        return m;
    }

    bool act(sync::ActionKind kind, sync::SessionId session, const std::string& aiName, const std::string& text,
             const std::string& account) {
        std::string why;
        return hub.apply(sync::Action{++num, kind, session, aiName, text, account}, &why, "walk");
    }

    bool edit(const accounts::Registry& next) {
        std::vector<std::string> report;
        return hub.applyAccounts(next, "a hand edit (walk)", &report);
    }

    // The code's whole state, as the model's variables.
    std::string record(const std::set<sync::SessionId>& told) const {
        std::ostringstream o;
        o << "acct |-> (";
        for (std::size_t i = 0; i < kAccts.size(); ++i) {
            const std::string& a = kAccts[i];
            const char* st = reg().account(a) == nullptr ? "absent" : hub.signedIn().count(a) != 0 ? "in" : "out";
            o << (i == 0 ? "" : " @@ ") << q(a) << " :> " << q(st);
        }
        o << "), prov |-> (";
        for (std::size_t i = 0; i < kAccts.size(); ++i) {
            o << (i == 0 ? "" : " @@ ") << q(kAccts[i]) << " :> " << q(providerOf(kAccts[i]));
        }
        o << "), known |-> {";
        bool first = true;
        for (const accounts::AI& f : reg().ais) {
            o << (first ? "" : ", ") << q(f.name);
            first = false;
        }
        o << "}, links |-> (";
        for (std::size_t i = 0; i < kAIs.size(); ++i) {
            const accounts::AI* f = reg().aiNamed(kAIs[i]);
            o << (i == 0 ? "" : " @@ ") << q(kAIs[i]) << " :> <<";
            if (f != nullptr) {
                for (std::size_t j = 0; j < f->accounts.size(); ++j) {
                    o << (j == 0 ? "" : ", ") << q(f->accounts[j]);
                }
            }
            o << ">>";
        }
        o << "), dflt |-> (";
        for (std::size_t i = 0; i < kAIs.size(); ++i) {
            const accounts::AI* f = reg().aiNamed(kAIs[i]);
            o << (i == 0 ? "" : " @@ ") << q(kAIs[i]) << " :> " << q(orNone(f == nullptr ? "" : f->preferred));
        }
        o << "), insts |-> (";
        for (std::size_t i = 0; i < kAIs.size(); ++i) {
            const accounts::AI* f = reg().aiNamed(kAIs[i]);
            o << (i == 0 ? "" : " @@ ") << q(kAIs[i]) << " :> <<";
            if (f != nullptr) {
                for (std::size_t j = 0; j < f->instances.size(); ++j) {
                    o << (j == 0 ? "" : ", ") << q(f->instances[j].name);
                }
            }
            o << ">>";
        }
        o << "), ses |-> <<";
        for (sync::SessionId id = 1; id <= kMaxSes; ++id) {
            const sync::StoredSession* s = store.find(id);
            o << (id == 1 ? "" : ", ");
            if (s == nullptr) {
                o << "[made |-> FALSE, ai |-> \"none\", inst |-> \"none\", acct |-> \"none\", harness |-> \"none\", automatic |-> FALSE]";
            } else {
                o << "[made |-> TRUE, ai |-> " << q(s->info.aiName) << ", inst |-> " <<
                    q(orNone(s->info.instance)) << ", acct |-> " << q(orNone(s->info.account)) << ", harness |-> " <<
                    q(s->info.harness) << ", automatic |-> " << (s->info.autoAccount ? "TRUE" : "FALSE") << "]";
            }
        }
        o << ">>, nextSes |-> " << store.nextId() << ", lim |-> (";
        for (std::size_t i = 0; i < kAIs.size(); ++i) {
            o << (i == 0 ? "" : " @@ ") << q(kAIs[i]) << " :> " << (limitOf(kAIs[i]) ? "TRUE" : "FALSE");
        }
        o << "), bill |-> (";
        for (std::size_t i = 0; i < kAccts.size(); ++i) {
            o << (i == 0 ? "" : " @@ ") << q(kAccts[i]) << " :> " << q(apiBilled(kAccts[i]) ? "api" : "sub");
        }
        o << "), run |-> <<";
        for (sync::SessionId id = 1; id <= kMaxSes; ++id) {
            const auto it = runs.find(id);
            const Run r = it == runs.end() ? Run{} : it->second;
            o << (id == 1 ? "" : ", ") << "[st |-> " << q(r.st) << ", api |-> " << (r.api ? "TRUE" : "FALSE") <<
                ", flim |-> " << (r.flim ? "TRUE" : "FALSE") << "]";
        }
        o << ">>, told |-> {";
        first = true;
        for (sync::SessionId id : told) {
            o << (first ? "" : ", ") << id;
            first = false;
        }
        o << "}";
        return o.str();
    }
};

} // namespace

int creditHandoffTrace(const std::string& mode, const std::string& path);

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--credit-handoff") == 0)
        return creditHandoffTrace(argv[2], argv[3]);
    std::string faulty;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-stay") == 0) {
            faulty = "stay";
        } else if (std::strcmp(argv[i], "--faulty-harness") == 0) {
            faulty = "harness";
        } else if (std::strcmp(argv[i], "--faulty-instance") == 0) {
            faulty = "instance";
        } else if (std::strcmp(argv[i], "--faulty-limit") == 0) {
            faulty = "limit";
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: accounts_walk <seed> <steps> [--faulty-stay | --faulty-harness | --faulty-instance | --faulty-limit] "
                     "<out.tla>\n");
        return 2;
    }
    state = std::strtoull(args[0].c_str(), nullptr, 10) * 0x9E3779B97F4A7C15ULL + 1;
    const long steps = std::strtol(args[1].c_str(), nullptr, 10);

    Walk w;
    w.hub.useAccounts(accounts::Registry{}, nullptr);
    std::vector<std::string> records;
    records.push_back("[op |-> \"init\", f |-> \"none\", a |-> \"none\", s |-> 0, p |-> \"none\", v |-> TRUE, "
                      "b |-> \"sub\", ok |-> TRUE, " +
                      w.record({}) + "]");
    std::vector<std::string> acctsOrNone = kAccts;
    acctsOrNone.emplace_back(kNone);
    std::size_t faults = 0;
    std::map<std::string, std::size_t> taken;  // each operation's steps the code took
    std::size_t moved = 0;                      // sessions the server moved (rotations and settles)
    std::size_t harnessChanged = 0;             // sessions whose harness changed

    for (long step = 0; step < steps; ++step) {
        const auto noticesBefore = w.notices();
        const auto infosBefore = w.infos();
        std::string op;
        std::string f = kNone;
        std::string a = kNone;
        std::string p = kNone;
        sync::SessionId s = 0;
        bool v = true;
        std::string b = "sub";
        bool ok = false;
        const std::size_t choice = pick(66);   // weighted toward the steps that move sessions
        if (choice < 3) {
            op = "add";
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::AddAccount, 0, "", w.providerOf(a), a);
        } else if (choice < 4) {
            op = "remove";
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::RemoveAccount, 0, "", "", a);
        } else if (choice < 10) {
            op = pick(3) == 0 ? "out" : "in";
            a = pickOf(kAccts);
            ok = w.reg().account(a) != nullptr;
            w.hub.reportSignedIn(a, op == "in");
        } else if (choice < 12) {
            op = "ai";
            f = pickOf(kAIs);
            ok = w.act(sync::ActionKind::AddAI, 0, f, "", "");
        } else if (choice < 19) {
            op = "link";
            f = pickOf(kAIs);
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::LinkAccount, 0, f, "", a);
        } else if (choice < 21) {
            op = "unlink";
            f = pickOf(kAIs);
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::UnlinkAccount, 0, f, "", a);
        } else if (choice < 24) {
            op = "default";
            f = pickOf(kAIs);
            a = pickOf(acctsOrNone);
            ok = w.act(sync::ActionKind::SetDefault, 0, f, "", a == kNone ? "" : a);
        } else if (choice < 25) {
            op = "dropai";
            f = pickOf(kAIs);
            accounts::Registry next = w.reg();
            std::erase_if(next.ais, [&](const accounts::AI& x) { return x.name == f; });
            ok = w.edit(next);
        } else if (choice < 26) {
            op = "droplink";
            f = pickOf(kAIs);
            a = pickOf(kAccts);
            accounts::Registry next = w.reg();
            if (accounts::AI* x = next.aiNamed(f)) {
                std::erase(x->accounts, a);
            }
            ok = w.edit(next);
        } else if (choice < 27) {
            op = "dropaccount";
            a = pickOf(kAccts);
            accounts::Registry next = w.reg();
            std::erase_if(next.accounts, [&](const accounts::Account& x) { return x.name == a; });
            ok = w.edit(next);
        } else if (choice < 29) {
            op = "provider";
            a = pickOf(kAccts);
            p = pickOf(kProviders);
            accounts::Registry next = w.reg();
            for (accounts::Account& x : next.accounts) {
                if (x.name == a) {
                    x.provider = p;
                }
            }
            ok = w.edit(next);
            if (ok) {
                w.provider[a] = p;
            }
        } else if (choice < 38) {
            op = "set";
            s = static_cast<sync::SessionId>(1 + pick(kMaxSes));
            a = pickOf(acctsOrNone);
            const sync::StoredSession* on = w.store.find(s);
            const accounts::AI* fr = on == nullptr ? nullptr : w.reg().aiNamed(on->info.aiName);
            if (fr != nullptr && !fr->accounts.empty() && pick(4) != 0) {
                a = pickOf(fr->accounts);   // mostly one of its AI's accounts, so that sessions run on some
            }
            ok = w.act(sync::ActionKind::SetAccount, s, "", a == kNone ? "" : a, "");
        } else if (choice < 44) {
            op = "rotate";
            s = static_cast<sync::SessionId>(1 + pick(kMaxSes));
            ok = w.hub.rotate(s);
        } else if (choice >= 58 && choice < 60) {
            // An AI's limit edited; an AI not in the files has no "limit" to edit (the model refuses it too).
            op = "limit";
            f = pickOf(kAIs);
            v = pick(2) == 0;
            accounts::Registry next = w.reg();
            if (accounts::AI* x = next.aiNamed(f)) {
                x->limit = v;
                ok = w.edit(next);
            }
        } else if (choice >= 60 && choice < 62) {
            // An account's billing edited; an account not in the files has none (the model refuses it too).
            op = "billing";
            a = pickOf(kAccts);
            b = pick(2) == 0 ? "api" : "sub";
            accounts::Registry next = w.reg();
            for (accounts::Account& x : next.accounts) {
                if (x.name == a) {
                    x.billing = b == "api" ? accounts::Billing::Api : accounts::Billing::Subscription;
                    ok = w.edit(next);
                }
            }
        } else if (choice >= 62 && choice < 65) {
            // A session's driver started: limited as the server decides at the start, from the files as they are.
            op = "start";
            s = static_cast<sync::SessionId>(1 + pick(kMaxSes));
            const sync::StoredSession* on = w.store.find(s);
            Walk::Run& r = w.runs[s];
            if (on != nullptr && r.st == "off") {
                const std::string& acc = on->info.account;
                bool limited = accounts::limitedFor(w.hub.accounts(), on->info.aiName, acc);
                if (faulty == "limit" && limited && !w.apiBilled(acc)) {
                    limited = false;
                    ++faults;
                }
                r = Walk::Run{limited ? "limited" : "open", !acc.empty() && w.apiBilled(acc), w.limitOf(on->info.aiName)};
                ok = true;
            }
        } else if (choice == 65) {
            op = "end";
            s = static_cast<sync::SessionId>(1 + pick(kMaxSes));
            Walk::Run& r = w.runs[s];
            if (r.st != "off") {
                r = Walk::Run{};
                ok = true;
            }
        } else if (choice >= 50) {
            // An edit of an AI's instances: one added at the end, one taken out, or one moved in place (its
            // directory changed, its name kept).
            op = choice < 53 ? "addinst" : choice < 55 ? "dropinst" : "moveinst";
            f = pickOf(kAIs);
            a = pickOf(kInsts);
            accounts::Registry next = w.reg();
            accounts::AI* x = next.aiNamed(f);
            if (op == "moveinst") {
                // The model's refusal of an instance the AI does not have is the code's too: not edited.
                auto in = x == nullptr ? std::vector<accounts::Instance>::iterator() :
                    std::find_if(x->instances.begin(), x->instances.end(),
                                 [&](const accounts::Instance& i) { return i.name == a; });
                if (x != nullptr && in != x->instances.end()) {
                    in->dir = (in->dir.rfind("/work/", 0) == 0 ? "/moved/" : "/work/") + a;
                    ok = w.edit(next);
                }
            } else if (x != nullptr) {
                if (op == "addinst") {
                    x->instances.push_back(accounts::Instance{a, "studio", "/work/" + a});
                } else {
                    std::erase_if(x->instances, [&](const accounts::Instance& i) { return i.name == a; });
                }
                ok = w.edit(next);
            } else {
                // No such AI in the files: an instance of it makes them inconsistent (refused); none to drop is
                // the files as they are.
                ok = op == "dropinst" && w.edit(next);
            }
        } else {
            if (w.store.nextId() > kMaxSes) {
                --step;     // the model's refusal past its limit is not the code's (Limits::sessions): not walked
                continue;
            }
            op = "create";
            f = pickOf(kAIs);
            s = static_cast<sync::SessionId>(pick(kMaxSes + 1));
            ok = w.act(sync::ActionKind::Create, s, f, "", "");
        }
        // The faulty hubs: a session the step moved is put back where it was.
        if (ok && !faulty.empty()) {
            const bool unlinking = op == "remove" || op == "unlink" || op == "dropai" || op == "droplink";
            for (const auto& [id, before] : infosBefore) {
                const sync::SessionInfo now = w.store.find(id)->info;
                const bool stray = faulty == "stay" && unlinking && now.account != before.account;
                const bool harness = faulty == "harness" && op == "provider" && now.harness != before.harness;
                const bool instance = faulty == "instance" && (op == "dropinst" || op == "dropai") &&
                    now.instance != before.instance;
                if (stray || harness || instance) {
                    std::string why;
                    w.store.commit(id, {sync::infoChange(before)}, std::string(), 0, &why);
                    ++faults;
                }
            }
        }
        if (ok) {
            ++taken[op];
        }
        for (const auto& [id, before] : infosBefore) {
            const sync::SessionInfo now = w.store.find(id)->info;
            if (op != "set" && now.account != before.account) {
                ++moved;
            }
            if (now.harness != before.harness) {
                ++harnessChanged;
            }
        }
        const auto noticesAfter = w.notices();
        std::set<sync::SessionId> told;
        for (const auto& [id, n] : noticesAfter) {
            auto it = noticesBefore.find(id);
            if (n > (it == noticesBefore.end() ? 0 : it->second)) {
                told.insert(id);
            }
        }
        records.push_back("[op |-> " + q(op) + ", f |-> " + q(f) + ", a |-> " + q(a) + ", s |-> " +
                          std::to_string(s) + ", p |-> " + q(p) + ", v |-> " + (v ? "TRUE" : "FALSE") + ", b |-> " + q(b) +
                          ", ok |-> " + (ok ? "TRUE" : "FALSE") + ", " +
                          w.record(told) + "]");
    }

    std::ofstream out(args[2]);
    out << "---- MODULE AccountsTraceData ----\n"
           "\\* Written by tests/accounts/accounts_walk: seed " << args[0] << ", " << steps << " steps"
        << (faulty.empty() ? std::string() : ", faulty " + faulty) << ".\n"
        << "EXTENDS TLC\n"
        << "TraceAccts == {\"a1\", \"a2\", \"a3\"}\n"
        << "TraceProvider == (\"a1\" :> \"claude\" @@ \"a2\" :> \"claude\" @@ \"a3\" :> \"codex\")\n"
        << "TraceProviders == {\"claude\", \"codex\"}\n"
        << "TraceAIs == {\"f\", \"g\"}\n"
        << "TraceInsts == {\"i1\", \"i2\"}\n"
        << "TraceMaxSes == " << kMaxSes << "\n"
        << "TraceStartHarness == \"" << kStartHarness << "\"\n"
        << "TraceBills == {\"sub\", \"api\"}\n"
        << "TraceLimits == {TRUE, FALSE}\n"
        << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    if (!out) {
        std::fprintf(stderr, "accounts_walk: could not write %s\n", args[2].c_str());
        return 1;
    }
    std::string coverage;
    for (const auto& [name, n] : taken) {
        coverage += " " + name + "=" + std::to_string(n);
    }
    std::printf("accounts_walk: %zu records; taken:%s; %zu moves by the server, %zu harness changes%s\n",
                records.size(), coverage.c_str(), moved, harnessChanged,
                faulty.empty() ? "" : (", " + std::to_string(faults) + " sessions put back").c_str());
    if (!faulty.empty() && faults == 0) {
        std::fprintf(stderr, "accounts_walk: the faulty walk never hit its fault; try more steps\n");
        return 1;
    }
    return 0;
}
