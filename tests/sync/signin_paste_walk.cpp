// Trace walk: drives sign-in by a pasted code (src/sync/signin.h SignInDesk: need, claim, url, opened, codeWanted,
// code, printed, exited, expire, windowGone, drop, and offersFor as each window receives the flows) with a seeded random
// walk and writes the trace as SignInPasteTraceData.tla, for TLC to check against models/SignInPaste.tla
// (models/SignInPasteTrace.tla). No real files, no clock, no sockets, no processes: the time is the walk's counter,
// and the enrollment process, the vendor's page, the person and the windows are the walk's.
//
//   signin_paste_walk <seed> <steps> [--faulty-code] <out.tla>
//
// The desk is the real one, called as nexus-server calls it (src/server/server.cpp, through src/sync/hub.cpp): a
// window's claim, opened and code actions by its claim token, the process's URL and "code asked" by account and id,
// its enrollment saved (printed) or its end (exited), the deadline (expire), a window's link ending (windowGone) and a
// restart (drop, then a new desk). Each window's view is the desk's offersFor its token, taken when the window syncs.
//
// The walk plays the rest, as the model's environment: the one Antigravity account's enrollment process (started by a
// claim's effect, it hands over a made-up Google URL, asks for the code, and once given one, enrolls or exits; or exits
// at any time), the windows (two, c2 on another machine than the server: nothing here differs for it, which is the
// point), the person (claiming an offer no window holds, opening the URL of an offer it holds, pasting the code into the
// window whose offer asks for one, once), and stray or hostile code actions from any window for any flow. After each
// step it records the code's state as the model's variables (all but `log`: every log line and notice is searched for
// every made-up code, the URL and its parts instead, and one found fails the walk).
//
// The fault shows the binding can fail:
//   --faulty-code   the desk takes a pasted code from any window, not only the one holding the flow
//                   (SignInDesk::injectCodeFault): TLC must reject the trace by CodeCodeOnlyFromHolder

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "sync/secrets.h"
#include "sync/signin.h"

namespace {

using namespace nexus;
using namespace nexus::sync;

const std::string kAccount = "g1";
const std::string kProvider = "antigravity";
const std::vector<std::string> kClients{"c1", "c2"};
// Each window's claim token (isClaimToken: letters and digits).
const std::map<std::string, std::string> kTokens{{"c1", "walkTokenOneQx7Lm2"}, {"c2", "walkTokenTwoWp4Hn8"}};
const std::vector<std::string> kCodes{"k1", "k2"};
// The made-up codes' bytes, of letters found in nothing else the walk writes.
const std::map<std::string, std::string> kCodeBytes{{"k1", "4/0FAKEwalkCodeOneQx7Lm2Vb9Nc4Zr8Tw3"},
                                                    {"k2", "4/0FAKEwalkCodeTwoWp4Hn8Rk2Bv6Mx9Cq3"}};
const std::string kUrl = "https://accounts.google.com/o/oauth2/v2/auth?client_id=walkClientId&state=fakeWalkStQzXv";
constexpr std::uint64_t kMaxFlows = 12;
constexpr std::uint64_t kMaxStrays = 16;
constexpr std::uint64_t kMaxRestarts = 2;
const char* const kNone = "none";

std::uint64_t rngState = 0;
std::uint64_t rnd() {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return rngState * 2685821657736338717ULL;
}
std::size_t pick(std::size_t n) { return static_cast<std::size_t>(rnd() % n); }

std::string q(const std::string& s) { return "\"" + s + "\""; }
std::string b(bool x) { return x ? "TRUE" : "FALSE"; }
// A code is a model value (k1, k2); "none" is a string, as in the model.
std::string val(const std::string& v) { return v == kNone ? q(v) : v; }

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "signin_paste_walk: %s\n", what.c_str());
    std::exit(1);
}

// No log line or notice may hold a code, the URL or a part of either.
void clean(const std::string& text) {
    for (const auto& [name, bytes] : kCodeBytes) {
        if (text.find(bytes) != std::string::npos) {
            fail("a log line or notice holds code " + name + ": " + text);
        }
    }
    if (text.find("FAKEwalk") != std::string::npos || text.find("fakeWalk") != std::string::npos ||
            text.find("walkClientId") != std::string::npos || text.find("accounts.google.com") != std::string::npos ||
            text.find("walkToken") != std::string::npos) {
        fail("a log line or notice gives a value away: " + text);
    }
}

struct Msg {
    std::string kind;
    std::uint64_t id = 0;
    std::string c;
    std::string v;
    bool operator<(const Msg& o) const { return std::tie(kind, id, c, v) < std::tie(o.kind, o.id, o.c, o.v); }
    std::string tla() const {
        return "[kind |-> " + q(kind) + ", id |-> " + std::to_string(id) + ", c |-> " + q(c) + ", v |-> " + val(v) + "]";
    }
};

struct Offer {
    std::uint64_t id = 0;
    std::string opener;
    bool url = false;
    bool ask = false;
    bool operator<(const Offer& o) const { return std::tie(id, opener, url, ask) < std::tie(o.id, o.opener, o.url, o.ask); }
    bool operator==(const Offer& o) const = default;
    std::string tla() const {
        return "[id |-> " + std::to_string(id) + ", opener |-> " + q(opener) + ", url |-> " + b(url) + ", ask |-> " +
               b(ask) + "]";
    }
};

struct FlowRec {
    std::string st = "unused";
    std::string opener = kNone;
    bool asked = false;
    bool sent = false;
};

struct Proc {
    std::string st = kNone;     // none, running, url, asking, got-code
    std::uint64_t id = 0;
    std::string code = kNone;
};

std::string clientOf(const std::string& token) {
    for (const auto& [c, t] : kTokens) {
        if (t == token) {
            return c;
        }
    }
    fail("a token no window has: the desk made one up");
}

struct Walk {
    std::unique_ptr<SignInDesk> desk;
    bool faulty = false;
    std::uint64_t next = 1;
    std::map<std::uint64_t, FlowRec> flow;     // as last read; ended flows are kept as they ended
    std::set<std::uint64_t> ended;
    Proc proc;
    std::map<std::string, bool> conn;
    std::map<std::string, std::set<Offer>> view;
    std::map<std::string, std::set<std::uint64_t>> claimed, opened, pasted;
    std::set<Msg> up;
    std::uint64_t strays = 0;
    bool signedin = false;
    std::uint64_t restarts = 0;
    std::map<std::uint64_t, std::uint64_t> sent;
    std::set<std::uint64_t> badfrom, unasked;
    std::uint64_t clock = 1000;
    std::uint64_t faults = 0;

    void fresh() {
        desk = std::make_unique<SignInDesk>();
        if (faulty) {
            desk->injectCodeFault();
        }
    }

    Walk(bool f) : faulty(f) {
        fresh();
        for (std::uint64_t i = 1; i <= kMaxFlows; ++i) {
            flow[i] = FlowRec{};
            sent[i] = 0;
        }
        for (const std::string& c : kClients) {
            conn[c] = true;
            view[c] = {};
            claimed[c] = {};
            opened[c] = {};
            pasted[c] = {};
        }
    }

    // What the desk's effects say to the walk: the process to start or end, the code to hand it. Every line checked.
    void take(const SignInEffects& e) {
        for (const std::string& line : e.log) {
            clean(line);
        }
        for (const auto& n : e.notes) {
            clean(n.text);
        }
        for (const auto& s : e.start) {
            if (s.account != kAccount) {
                fail("a process started for an account the walk has not");
            }
            proc = Proc{"running", s.id, kNone};
        }
        for (const std::string& a : e.stop) {
            if (a == kAccount) {
                proc = Proc{};
            }
        }
    }

    // The code's state of every flow, read from the desk: an ended flow keeps what it had when it ended.
    void read() {
        std::map<std::string, std::string> holder;
        for (const SignInOffer& o : desk->offers()) {
            holder[std::to_string(o.id)] = o.opener.empty() ? std::string(kNone) : clientOf(o.opener);
        }
        for (std::uint64_t i = 1; i < next && i <= kMaxFlows; ++i) {
            if (ended.count(i) != 0) {
                continue;
            }
            const std::optional<SignInState> st = desk->stateOf(i);
            if (!st) {
                fail("flow " + std::to_string(i) + " is not known to the desk and never ended");
            }
            FlowRec& f = flow[i];
            f.st = signInStateName(*st);
            const auto cs = desk->codeStateOf(i);
            if (cs) {
                f.opener = holder.count(std::to_string(i)) != 0 ? holder[std::to_string(i)] : std::string(kNone);
                f.asked = cs->first;
                f.sent = cs->second;
            } else {
                f.opener = kNone;
                ended.insert(i);
            }
        }
    }

    std::set<Offer> pubFor(const std::string& c) const {
        std::set<Offer> out;
        for (const SignInOffer& o : desk->offersFor(kTokens.at(c))) {
            Offer x;
            x.id = o.id;
            x.opener = o.opener.empty() ? std::string(kNone)
                     : o.opener == kSignInHeldBy ? std::string("held")
                     : clientOf(o.opener);
            x.url = !o.url.empty();
            x.ask = o.codeWanted;
            if (x.ask && x.opener != c) {
                fail("a window was told a code is wanted for a flow it does not hold");
            }
            out.insert(x);
        }
        return out;
    }

    bool running(std::uint64_t i) const {
        const std::string& st = flow.at(i).st;
        return st == "started" || st == "url-sent" || st == "waiting";
    }
    bool anyLive() const {
        for (const auto& [i, f] : flow) {
            if (f.st == "needed" || running(i)) {
                return true;
            }
        }
        return false;
    }

    std::string record(const std::string& op, const std::string& args) const {
        std::ostringstream r;
        r << "[op |-> " << q(op) << args << ", next |-> " << next << ", flow |-> <<";
        for (std::uint64_t i = 1; i <= kMaxFlows; ++i) {
            const FlowRec& f = flow.at(i);
            r << (i > 1 ? ", " : "") << "[st |-> " << q(f.st) << ", opener |-> " << q(f.opener) << ", asked |-> "
              << b(f.asked) << ", sent |-> " << b(f.sent) << "]";
        }
        r << ">>, proc |-> [st |-> " << q(proc.st) << ", id |-> " << proc.id << ", code |-> " << val(proc.code) << "]";
        auto perClient = [&](const char* name, const std::function<std::string(const std::string&)>& f) {
            r << ", " << name << " |-> [";
            for (std::size_t k = 0; k < kClients.size(); ++k) {
                r << (k ? ", " : "") << kClients[k] << " |-> " << f(kClients[k]);
            }
            r << "]";
        };
        auto ids = [](const std::set<std::uint64_t>& s) {
            std::string out = "{";
            for (std::uint64_t i : s) {
                out += (out.size() > 1 ? ", " : "") + std::to_string(i);
            }
            return out + "}";
        };
        perClient("conn", [&](const std::string& c) { return b(conn.at(c)); });
        perClient("view", [&](const std::string& c) {
            std::string out = "{";
            for (const Offer& o : view.at(c)) {
                out += (out.size() > 1 ? ", " : "") + o.tla();
            }
            return out + "}";
        });
        perClient("claimed", [&](const std::string& c) { return ids(claimed.at(c)); });
        perClient("opened", [&](const std::string& c) { return ids(opened.at(c)); });
        perClient("pasted", [&](const std::string& c) { return ids(pasted.at(c)); });
        r << ", up |-> {";
        bool first = true;
        for (const Msg& m : up) {
            r << (first ? "" : ", ") << m.tla();
            first = false;
        }
        r << "}, strays |-> " << strays << ", signedin |-> " << b(signedin) << ", restarts |-> " << restarts
          << ", sent |-> <<";
        for (std::uint64_t i = 1; i <= kMaxFlows; ++i) {
            r << (i > 1 ? ", " : "") << sent.at(i);
        }
        r << ">>, badfrom |-> " << ids(badfrom) << ", unasked |-> " << ids(unasked) << "]";
        return r.str();
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool faulty = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--faulty-code") {
            faulty = true;
        } else {
            args.push_back(a);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: signin_paste_walk <seed> <steps> [--faulty-code] <out.tla>\n");
        return 2;
    }
    rngState = std::strtoull(args[0].c_str(), nullptr, 10) * 0x9E3779B97F4A7C15ULL + 1;
    const std::uint64_t steps = std::strtoull(args[1].c_str(), nullptr, 10);

    Walk w(faulty);
    std::vector<std::string> records{w.record("init", "")};
    std::map<std::string, std::uint64_t> counts;

    struct Choice {
        std::uint64_t weight;
        std::function<std::string()> run;     // does the step, returns its op's arguments
        std::string op;
    };

    for (std::uint64_t s = 0; s < steps; ++s) {
        std::vector<Choice> choices;
        // The server and the process.
        if (!w.signedin && !w.anyLive() && w.next <= kMaxFlows) {
            choices.push_back({4, [&] {
                w.take(w.desk->need(w.next, kAccount, kProvider));
                ++w.next;
                return std::string();
            }, "need"});
        }
        for (const Msg& m : w.up) {
            choices.push_back({6, [&, m] {
                w.up.erase(m);
                const std::string& token = kTokens.at(m.c);
                if (m.kind == "claim") {
                    w.take(w.desk->claim(m.id, token, w.clock));
                } else if (m.kind == "opened") {
                    w.take(w.desk->opened(m.id, token));
                } else {
                    // The ghosts, from what held before the step: who held the flow, whether its process asked.
                    const std::string holder = m.id <= kMaxFlows ? w.flow.at(m.id).opener : std::string(kNone);
                    const bool asking = w.proc.id == m.id && w.proc.st == "asking";
                    std::string refusal;
                    const SignInEffects e = w.desk->code(m.id, token, SecretValue(kCodeBytes.at(m.v)), &refusal);
                    w.take(e);
                    clean(refusal);
                    if (e.codes.empty() && refusal.empty()) {
                        fail("a code neither taken nor refused with a line");
                    }
                    for (const auto& c : e.codes) {
                        if (c.id != m.id || c.account != kAccount || c.code.bytes() != kCodeBytes.at(m.v)) {
                            fail("a code went to the wrong flow, or changed on the way");
                        }
                        ++w.sent[m.id];
                        if (holder != m.c) {
                            w.badfrom.insert(m.id);
                            ++w.faults;
                        }
                        if (!asking) {
                            w.unasked.insert(m.id);
                        }
                        if (asking) {
                            w.proc.st = "got-code";
                            w.proc.code = m.v;
                        }
                    }
                }
                return ", m |-> " + m.tla();
            }, "receive"});
        }
        if (w.proc.st == "running" && w.flow.at(w.proc.id).st == "started") {
            choices.push_back({4, [&] {
                w.take(w.desk->url(kAccount, w.proc.id, SecretValue(kUrl)));
                w.proc.st = "url";
                return std::string();
            }, "url"});
        }
        if (w.proc.st == "url" && w.running(w.proc.id)) {
            choices.push_back({4, [&] {
                w.take(w.desk->codeWanted(kAccount, w.proc.id));
                w.proc.st = "asking";
                return std::string();
            }, "ask"});
        }
        if (w.proc.st == "got-code" && w.running(w.proc.id)) {
            choices.push_back({2, [&] {
                SignInEffects e;
                if (w.desk->printed(kAccount, &e) != w.proc.id) {
                    fail("the enrollment saved was not its flow's");
                }
                w.take(e);
                w.proc = Proc{};
                w.signedin = true;
                return std::string();
            }, "enroll"});
        }
        if (w.proc.st != kNone) {
            choices.push_back({1, [&] {
                w.take(w.desk->exited(kAccount, w.proc.id, "the walk's process ended"));
                w.proc = Proc{};
                return std::string();
            }, "exit"});
        }
        if (const auto due = w.desk->deadline()) {
            choices.push_back({1, [&, due] {
                std::uint64_t id = 0;
                for (const auto& [i, f] : w.flow) {
                    if (w.running(i)) {
                        id = i;
                    }
                }
                w.clock = std::max(w.clock, *due);
                w.take(w.desk->expire(w.clock));
                return ", i |-> " + std::to_string(id);
            }, "timeout"});
        }
        if (w.restarts < kMaxRestarts) {
            choices.push_back({1, [&] {
                w.take(w.desk->drop());
                w.proc = Proc{};
                w.read();
                w.fresh();
                ++w.restarts;
                return std::string();
            }, "restart"});
        }
        // The windows and the person.
        for (const std::string& c : kClients) {
            if (w.conn[c] && w.view[c] != w.pubFor(c)) {
                choices.push_back({8, [&, c] {
                    w.view[c] = w.pubFor(c);
                    return ", c |-> " + q(c);
                }, "sync"});
            }
            if (w.conn[c]) {
                for (const Offer& o : w.view[c]) {
                    if (o.opener == kNone && w.claimed[c].count(o.id) == 0) {
                        choices.push_back({4, [&, c, o] {
                            w.up.insert({"claim", o.id, c, kNone});
                            w.claimed[c].insert(o.id);
                            return ", c |-> " + q(c) + ", i |-> " + std::to_string(o.id);
                        }, "claim"});
                    }
                    if (o.opener == c && o.url && w.opened[c].count(o.id) == 0) {
                        choices.push_back({4, [&, c, o] {
                            w.opened[c].insert(o.id);
                            w.up.insert({"opened", o.id, c, kNone});
                            return ", c |-> " + q(c) + ", i |-> " + std::to_string(o.id);
                        }, "open"});
                    }
                    if (o.opener == c && o.ask && w.opened[c].count(o.id) != 0 && w.pasted[c].count(o.id) == 0) {
                        const std::string v = kCodes[pick(kCodes.size())];
                        choices.push_back({6, [&, c, o, v] {
                            w.pasted[c].insert(o.id);
                            w.up.insert({"code", o.id, c, v});
                            return ", c |-> " + q(c) + ", v |-> " + v + ", i |-> " + std::to_string(o.id);
                        }, "paste"});
                    }
                }
                if (w.strays < kMaxStrays) {
                    // Mostly for the flow now, sometimes for any.
                    const std::uint64_t i = pick(3) != 0 && w.next > 1 ? std::min<std::uint64_t>(w.next - 1, kMaxFlows)
                                                                       : 1 + pick(kMaxFlows);
                    const std::string v = kCodes[pick(kCodes.size())];
                    choices.push_back({faulty ? 4u : 2u, [&, c, i, v] {
                        ++w.strays;
                        w.up.insert({"code", i, c, v});
                        return ", c |-> " + q(c) + ", i |-> " + std::to_string(i) + ", v |-> " + v;
                    }, "stray"});
                }
                choices.push_back({1, [&, c] {
                    w.conn[c] = false;
                    w.view[c] = {};
                    w.take(w.desk->windowGone(kTokens.at(c)));
                    return ", c |-> " + q(c);
                }, "disconnect"});
            } else {
                choices.push_back({3, [&, c] {
                    w.conn[c] = true;
                    return ", c |-> " + q(c);
                }, "connect"});
            }
        }
        std::uint64_t total = 0;
        for (const Choice& ch : choices) {
            total += ch.weight;
        }
        std::uint64_t at = rnd() % total;
        std::size_t k = 0;
        while (at >= choices[k].weight) {
            at -= choices[k].weight;
            ++k;
        }
        const std::string op = choices[k].op;
        const std::string fields = choices[k].run();
        w.read();
        ++counts[op];
        records.push_back(w.record(op, fields));
    }

    std::ofstream out(args[2]);
    out << "---- MODULE SignInPasteTraceData ----\n"
           "\\* Written by tests/sync/signin_paste_walk: seed " << args[0] << ", " << steps << " steps"
        << (faulty ? ", faulty code" : "") << ".\n"
        << "EXTENDS TLC\n"
        << "CONSTANTS k1, k2\n"
        << "TraceClients == {\"c1\", \"c2\"}\n"
        << "TraceRemote == {\"c2\"}\n"
        << "TraceCodes == {k1, k2}\n"
        << "TraceMaxFlows == " << kMaxFlows << "\n"
        << "TraceMaxStrays == " << kMaxStrays << "\n"
        << "TraceMaxRestarts == " << kMaxRestarts << "\n"
        << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    if (!out) {
        std::fprintf(stderr, "signin_paste_walk: could not write %s\n", args[2].c_str());
        return 1;
    }
    std::string coverage;
    for (const auto& [name, n] : counts) {
        coverage += " " + name + "=" + std::to_string(n);
    }
    std::printf("signin_paste_walk: %zu records; taken:%s%s\n", records.size(), coverage.c_str(),
                faulty ? (", " + std::to_string(w.faults) + " faults played").c_str() : "");
    if (faulty && w.faults == 0) {
        std::fprintf(stderr, "signin_paste_walk: the faulty walk never hit its fault; try more steps\n");
        return 1;
    }
    if (!faulty && counts["enroll"] == 0 && counts["paste"] == 0) {
        std::fprintf(stderr, "signin_paste_walk: the walk never pasted a code; try more steps or another seed\n");
        return 1;
    }
    return 0;
}
