// Trace walk: drives the hub's accounts and friends (src/sync/hub.h over a MemoryStore, src/accounts/registry.h) with
// a seeded random walk and writes the trace as AccountsTraceData.tla, for TLC to check against models/Accounts.tla
// (models/AccountsTrace.tla). No files, no clock: the registry is the hub's, the sign-ins are reported by the walk.
//
//   accounts_walk <seed> <steps> [--faulty-stay | --faulty-harness] <out.tla>
//
// Each step is one of the model's operations, given to the code the way the server gives it: an account action from a
// window (add, remove, friend, link, unlink, default, set, create) through Hub::apply; a hand edit (dropfriend,
// droplink, dropaccount, provider) as the registry it leaves, through Hub::applyAccounts, the way `nexus-server config
// reload` applies the files; a sign-in reported (in, out) through Hub::reportSignedIn; a usage limit (rotate) through
// Hub::rotate. After it the walk records the code's whole state: each account absent, signed in or needing sign-in,
// its provider, the friends with their links and defaults, each session's friend, account and harness, the next id,
// the sessions the step gave a notice (their transcripts' notices counted before and after), and whether the code took
// the step or refused it.
//
// An account's provider is the registry's while it is set up; while it is not, the walk keeps the one it had (or the
// instance's), as the model does, and sets one up again with it.
//
// The faults show the binding can fail; each is a hub that keeps a session where the model moves it, played by the
// walk writing the session's old settings back into the store after the step:
//   --faulty-stay      a session left on an account no longer linked to its friend
//   --faulty-harness   a session left on its old harness when its account's provider changed

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
const std::vector<std::string> kFriends{"f", "g"};
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

    bool act(sync::ActionKind kind, sync::SessionId session, const std::string& friendName, const std::string& text,
             const std::string& account) {
        std::string why;
        return hub.apply(sync::Action{++num, kind, session, friendName, text, account}, &why, "walk");
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
        for (const accounts::Friend& f : reg().friends) {
            o << (first ? "" : ", ") << q(f.name);
            first = false;
        }
        o << "}, links |-> (";
        for (std::size_t i = 0; i < kFriends.size(); ++i) {
            const accounts::Friend* f = reg().friendNamed(kFriends[i]);
            o << (i == 0 ? "" : " @@ ") << q(kFriends[i]) << " :> <<";
            if (f != nullptr) {
                for (std::size_t j = 0; j < f->accounts.size(); ++j) {
                    o << (j == 0 ? "" : ", ") << q(f->accounts[j]);
                }
            }
            o << ">>";
        }
        o << "), dflt |-> (";
        for (std::size_t i = 0; i < kFriends.size(); ++i) {
            const accounts::Friend* f = reg().friendNamed(kFriends[i]);
            o << (i == 0 ? "" : " @@ ") << q(kFriends[i]) << " :> " << q(orNone(f == nullptr ? "" : f->preferred));
        }
        o << "), ses |-> <<";
        for (sync::SessionId id = 1; id <= kMaxSes; ++id) {
            const sync::StoredSession* s = store.find(id);
            o << (id == 1 ? "" : ", ");
            if (s == nullptr) {
                o << "[made |-> FALSE, friend |-> \"none\", acct |-> \"none\", harness |-> \"none\"]";
            } else {
                o << "[made |-> TRUE, friend |-> " << q(s->info.friendName) << ", acct |-> " << q(orNone(s->info.account))
                  << ", harness |-> " << q(s->info.harness) << "]";
            }
        }
        o << ">>, nextSes |-> " << store.nextId() << ", told |-> {";
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

int main(int argc, char** argv) {
    std::string faulty;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-stay") == 0) {
            faulty = "stay";
        } else if (std::strcmp(argv[i], "--faulty-harness") == 0) {
            faulty = "harness";
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: accounts_walk <seed> <steps> [--faulty-stay | --faulty-harness] <out.tla>\n");
        return 2;
    }
    state = std::strtoull(args[0].c_str(), nullptr, 10) * 0x9E3779B97F4A7C15ULL + 1;
    const long steps = std::strtol(args[1].c_str(), nullptr, 10);

    Walk w;
    w.hub.useAccounts(accounts::Registry{}, nullptr);
    std::vector<std::string> records;
    records.push_back("[op |-> \"init\", f |-> \"none\", a |-> \"none\", s |-> 0, p |-> \"none\", ok |-> TRUE, " +
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
        bool ok = false;
        const std::size_t choice = pick(30);
        if (choice < 3) {
            op = "add";
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::AddAccount, 0, "", w.providerOf(a), a);
        } else if (choice < 4) {
            op = "remove";
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::RemoveAccount, 0, "", "", a);
        } else if (choice < 7) {
            op = pick(3) == 0 ? "out" : "in";
            a = pickOf(kAccts);
            ok = w.reg().account(a) != nullptr;
            w.hub.reportSignedIn(a, op == "in");
        } else if (choice < 9) {
            op = "friend";
            f = pickOf(kFriends);
            ok = w.act(sync::ActionKind::AddFriend, 0, f, "", "");
        } else if (choice < 12) {
            op = "link";
            f = pickOf(kFriends);
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::LinkAccount, 0, f, "", a);
        } else if (choice < 13) {
            op = "unlink";
            f = pickOf(kFriends);
            a = pickOf(kAccts);
            ok = w.act(sync::ActionKind::UnlinkAccount, 0, f, "", a);
        } else if (choice < 15) {
            op = "default";
            f = pickOf(kFriends);
            a = pickOf(acctsOrNone);
            ok = w.act(sync::ActionKind::SetDefault, 0, f, "", a == kNone ? "" : a);
        } else if (choice < 16) {
            op = "dropfriend";
            f = pickOf(kFriends);
            accounts::Registry next = w.reg();
            std::erase_if(next.friends, [&](const accounts::Friend& x) { return x.name == f; });
            ok = w.edit(next);
        } else if (choice < 17) {
            op = "droplink";
            f = pickOf(kFriends);
            a = pickOf(kAccts);
            accounts::Registry next = w.reg();
            if (accounts::Friend* x = next.friendNamed(f)) {
                std::erase(x->accounts, a);
            }
            ok = w.edit(next);
        } else if (choice < 18) {
            op = "dropaccount";
            a = pickOf(kAccts);
            accounts::Registry next = w.reg();
            std::erase_if(next.accounts, [&](const accounts::Account& x) { return x.name == a; });
            ok = w.edit(next);
        } else if (choice < 20) {
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
        } else if (choice < 25) {
            op = "set";
            s = static_cast<sync::SessionId>(1 + pick(kMaxSes));
            a = pickOf(acctsOrNone);
            ok = w.act(sync::ActionKind::SetAccount, s, "", a == kNone ? "" : a, "");
        } else if (choice < 27) {
            op = "rotate";
            s = static_cast<sync::SessionId>(1 + pick(kMaxSes));
            ok = w.hub.rotate(s);
        } else {
            if (w.store.nextId() > kMaxSes) {
                --step;     // the model's refusal past its limit is not the code's (Limits::sessions): not walked
                continue;
            }
            op = "create";
            f = pickOf(kFriends);
            s = static_cast<sync::SessionId>(pick(kMaxSes + 1));
            ok = w.act(sync::ActionKind::Create, s, f, "", "");
        }
        // The faulty hubs: a session the step moved is put back where it was.
        if (ok && !faulty.empty()) {
            const bool unlinking = op == "remove" || op == "unlink" || op == "dropfriend" || op == "droplink";
            for (const auto& [id, before] : infosBefore) {
                const sync::SessionInfo now = w.store.find(id)->info;
                const bool stray = faulty == "stay" && unlinking && now.account != before.account;
                const bool harness = faulty == "harness" && op == "provider" && now.harness != before.harness;
                if (stray || harness) {
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
                          std::to_string(s) + ", p |-> " + q(p) + ", ok |-> " + (ok ? "TRUE" : "FALSE") + ", " +
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
        << "TraceFriends == {\"f\", \"g\"}\n"
        << "TraceMaxSes == " << kMaxSes << "\n"
        << "TraceStartHarness == \"" << kStartHarness << "\"\n"
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
