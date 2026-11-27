// Trace walk: drives in-app sign-in's code (src/sync/signin.h, through src/sync/hub.h over a MemoryStore, and each
// window's SignInOpener and CallbackGate) with a seeded random walk and writes the trace as SignInTraceData.tla, for TLC
// to check against models/SignIn.tla (models/SignInTrace.tla). No real files, no clock, no sockets, no processes: the
// time is the walk's counter, the login files a map behind the hub's SecretKeeper, and `claude setup-token`, the browser
// and the person are the walk's.
//
//   signin_walk <seed> <steps> [--faulty-claim | --faulty-callback | --faulty-stray] <out.tla>
//
// The pieces are the real ones, wired as the server and the windows wire them: the hub's signInWith hooks (start, stop,
// replay, takers), apply (each window's action encoded by Actions in the state format and parsed back), signInUrl,
// signInPrinted, signInExited, signInWindowGone, signInWindowsChanged, expireSignIns, reportSignedIn, requestSignIn,
// retrySecrets and dropSignIns (a restart is a new Hub over the same store, its signed-in accounts from the probe); each
// window's state is the hub's stateFor its claim token, sent as a diff in the state format and applied to the window's
// copy, then given to its SignInOpener; a window on another machine (c2) takes the browser's redirect with a
// CallbackGate over the request's bytes.
//
// The walk plays the rest, as the model's environment: the process for each account (started by the hook, it hands
// over a made-up URL, takes the redirect or the replay on its listener when the state is its own, prints a made-up
// token, or an API key one time in six, which the server refuses: the model's Exit; or exits; told to end, it may
// still print, in time or late, which is taken all the same: the model's Print; or ends: the model's End), the browser
// (each page a window opened, sent to the redirect once, signed in or declined), stray requests to a gate, the links,
// the windows from before sign-ins (a moment with only those linked ends the flow no window holds, needed or running,
// and asks for a paste: the model's FallBack), the person cancelling that paste and the account asked for again (the
// model's AskEnds, then Need), the disk and the restarts. After each step it records the code's state as the model's
// variables (all but `log`, which the model
// keeps per step; every log line is searched for every made-up value instead, and one found fails the walk).
//
// The server mints a fresh flow in the same call that ends one (Hub::signInEffects needAgain) and for every account
// with no login at a start, where the model takes a Need step after: the walk writes such a step as two records, the
// step with the new flows not yet minted (their ids unused, `next` before them), then a `need` record for each.
//
// The faults show the binding can fail:
//   --faulty-claim     a takeover claim starts a second process (SignInDesk::injectClaimFault)
//   --faulty-callback  the server replays a callback whatever its flow's state (SignInDesk::injectCallbackFault); the
//                      walk delivers each callback twice, as a resent action, so the second is replayed too
//   --faulty-stray     the window's gate takes any GET (CallbackGate::injectStrayFault)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "leak_scan.h"
#include "sync/actions.h"
#include "sync/formats.h"
#include "sync/hub.h"
#include "sync/secrets.h"
#include "sync/sessions.h"
#include "sync/signin.h"

namespace {

using namespace nexus;

const std::vector<std::string> kAccts{"a1", "a2"};
const std::vector<std::string> kClients{"c1", "c2"};
const std::string kRemote = "c2";
constexpr std::uint64_t kMaxFlows = 60;
constexpr std::uint64_t kMaxRestarts = 2;
constexpr std::uint64_t kMaxFailures = 6;
constexpr std::uint64_t kMaxLosses = 12;
const char* const kNone = "none";

// A made-up login of a subscription's length: `head`, filled out with `fill` (a letter each login has alone).
std::string fakeLogin(std::string head, char fill) {
    head.resize(sync::kSubscriptionTokenBytes, fill);
    return head;
}

// The made-up logins: two of a subscription's shape (the model's t1, t2), and an API key's, which the server refuses.
const std::vector<std::string> kLogins{fakeLogin("sk-ant-oat01-FAKEwalkTokOneQx7Lm2Vb9Nc4Zr8Tw3Ys6Ud1Pe5Kf0HgJj", 'q'),
                                       fakeLogin("sk-ant-oat01-FAKEwalkTokTwoWp4Hn8Rk2Bv6Mx9Cq3Lz7Gd5Fs1Jt0Yu", 'z')};
const std::vector<std::string> kTokens{"t1", "t2"};
const std::string kApiKey = fakeLogin("sk-ant-api03-FAKEwalkKeyQx7Lm2Vb9Nc4Zr8Tw3Ys6Ud1Pe5Kf0HgJj", 'k');

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

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "signin_walk: %s\n", what.c_str());
    std::exit(1);
}

// Each flow's made-up state and code: of letters found in nothing else the walk writes.
std::string stateOf(std::uint64_t id) { return "fakeWalkSt" + std::to_string(id) + "QzXv"; }
std::string codeOf(std::uint64_t id) { return "fakeWalkCo" + std::to_string(id) + "JpWm"; }
std::uint16_t portOf(std::uint64_t id) { return static_cast<std::uint16_t>(54000 + id); }
std::string urlOf(std::uint64_t id) {
    return "https://claude.com/cai/oauth/authorize?code=true&client_id=walkClientId&response_type=code"
           "&redirect_uri=http%3A%2F%2Flocalhost%3A" + std::to_string(portOf(id)) + "%2Fcallback&state=" + stateOf(id);
}

// The model value of the login `bytes` gives away (leak_scan.h), or empty.
std::string leaked(const std::string& text) {
    static const std::vector<test::LeakScan> scans{test::LeakScan(kLogins[0]), test::LeakScan(kLogins[1])};
    for (std::size_t i = 0; i < scans.size(); ++i) {
        if (scans[i].in(text)) {
            return kTokens[i];
        }
    }
    return std::string();
}

// A log line: none may hold a login, the API key, a URL, a state or a code.
void cleanLine(const std::string& line) {
    static const test::LeakScan key(kApiKey);
    if (!leaked(line).empty() || key.in(line) || line.find("fakeWalk") != std::string::npos ||
            line.find("walkClientId") != std::string::npos) {
        fail("a log line gives a value away");
    }
}

struct Keeper : sync::SecretKeeper {
    std::map<std::string, std::string> files;
    std::size_t writes = 0;     // durable writes so far
    bool failing = false;
    sync::Kept keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                    std::string* why) override {
        (void)what;
        if (failing) {
            *why = "the disk is full (the walk's)";
            return sync::Kept::Failed;
        }
        files[account] = value.bytes();
        ++writes;
        return sync::Kept::Durable;
    }
};

struct Events : sync::SecretEvents {
    void answered(const sync::SecretReply&) override {}
    void logged(const sync::SecretLog& line) override { cleanLine(line.line()); }
    void signInLogged(const std::string& line) override { cleanLine(line); }
};

struct Proc {
    std::string st = kNone;
    std::uint64_t id = 0;
    bool ending = false;    // told to end (the stop hook); it runs, and may print, until the walk ends it
};

struct Flight {
    std::string wire;
    std::string kind;
    std::uint64_t id = 0;
    std::string c;
    bool match = true;
    bool ok = true;
    std::string client;     // the hub's client it is applied under
};

struct Window {
    std::string name;
    std::string token;
    bool remote = false;
    bool conn = true;
    sync::Sessions got;
    sync::SignInOpener opener;
    std::set<std::uint64_t> browser;
    std::map<std::uint64_t, sync::SignInTarget> pages;  // the target of each page opened
    std::unique_ptr<sync::CallbackGate> gate;
    std::uint64_t gateFlow = 0;
    std::string leak;
};

struct FlowRec {
    std::string st = "unused";
    std::string acct = kNone;
    std::string opener = kNone;
    bool denied = false;
};

struct Walk {
    sync::MemoryStore store;
    std::unique_ptr<sync::Hub> hub;
    Keeper keeper;
    Events events;
    std::uint64_t now = 1000;
    std::vector<Window> windows;
    std::vector<Flight> up;
    std::map<std::string, Proc> proc;
    std::map<std::string, int> procs;
    std::map<std::uint64_t, int> cbs;
    std::set<std::uint64_t> badcb;
    std::set<std::uint64_t> denied;
    std::map<std::uint64_t, FlowRec> flows;         // as last reported (an old hub's flows are not the new one's)
    std::map<std::uint64_t, std::string> taken;
    std::map<std::uint64_t, std::string> said;      // the login each flow's process printed, in time or late
    std::map<std::uint64_t, std::string> heldBySecret;  // the login of each capture's secret request
    std::set<std::uint64_t> lost;
    std::set<std::string> signedBefore;             // the probe at a start: the logins that work
    std::uint64_t restarts = 0;
    std::uint64_t failures = 0;
    std::uint64_t losses = 0;
    std::uint64_t num = 0;
    std::string faulty;
    std::size_t faults = 0;

    Walk(const std::string& fault) : faulty(fault) {
        for (const std::string& c : kClients) {
            Window w;
            w.name = c;
            w.token = "tok" + c + "Walk";
            w.remote = c == kRemote;
            w.got.setFormat(sync::kStateFormat);
            w.opener.setToken(w.token);
            windows.push_back(std::move(w));
        }
        for (const std::string& a : kAccts) {
            proc[a] = Proc{};
            procs[a] = 0;
        }
    }

    // The server's start: a hub over the store, its accounts (signed in as the probe says), the one-chance store and
    // sign-in wired; with `hooks` false, all but sign-in (the model's Init, before the first needs).
    void start(bool hooks) {
        hub = std::make_unique<sync::Hub>(store);
        accounts::Registry r;
        for (const std::string& a : kAccts) {
            r.accounts.push_back(accounts::Account{a, "claude"});
        }
        hub->probeSignInWith([this](const accounts::Account& a) { return signedBefore.count(a.name) != 0; });
        hub->useAccounts(r, nullptr);
        hub->secretsWith(&keeper, &events, [] { return true; }, [this] { return now; });
        if (faulty == "claim") {
            hub->injectSignInClaimFault();
        }
        if (faulty == "callback") {
            hub->injectSignInCallbackFault();
        }
        if (hooks) {
            wire();
        }
    }

    void wire() {
        sync::SignInHooks h;
        h.start = [this](std::uint64_t id, const std::string& account) {
            // A process still ending is ended first (SignInRunner::start); any other is a second one.
            if (proc[account].st == kNone || !proc[account].ending) {
                ++procs[account];
            }
            proc[account] = Proc{"running", id, false};
            if (procs[account] > 1 && faulty == "claim") {
                ++faults;
            }
        };
        // Told to end: it runs on, and may print, until the walk's "end" step (SIGTERM, then SIGKILL after a grace).
        h.stop = [this](const std::string& account) {
            if (proc[account].st != kNone) {
                proc[account].ending = true;
            }
        };
        h.replay = [this](std::uint64_t id, const std::string& account, const sync::SignInTarget& target,
                          const sync::SecretValue& query) {
            ++cbs[id];
            const std::string state = sync::queryValue(query.bytes(), "state");
            const bool code = !sync::queryValue(query.bytes(), "code").empty();
            if (state != stateOf(id) || target.state != stateOf(id)) {
                badcb.insert(id);
            }
            if (cbs[id] > 1 && faulty == "callback") {
                ++faults;
            }
            if (!code) {
                denied.insert(id);
            }
            Proc& p = proc[account];
            if (p.id == id && p.st == "url" && state == stateOf(id)) {
                p.st = code ? "got-code" : "got-denied";
            }
        };
        // The windows read sign-ins, but for the walk's "fallback" step (only windows from before sign-ins linked).
        h.takers = [this] { return !oldOnly; };
        hub->signInWith(std::move(h), "server");
    }
    bool oldOnly = false;

    std::uint64_t nextId() const { return store.applied(sync::kSignInIdsClient) + 1; }

    // The flows as the hub reports them now.
    void readFlows() {
        std::map<std::uint64_t, std::string> openers;
        for (const sync::SignInOffer& o : hub->signIns().offers()) {
            openers[o.id] = o.opener;
            flows[o.id].acct = o.account;
        }
        for (auto& [id, f] : flows) {
            if (const std::optional<sync::SignInState> s = hub->signIns().stateOf(id)) {
                f.st = sync::signInStateName(*s);
            }
            const auto it = openers.find(id);
            f.opener = kNone;
            if (it != openers.end() && !it->second.empty()) {
                f.opener = clientOf(it->second);
            }
            f.denied = denied.count(id) != 0;
        }
    }

    std::string clientOf(const std::string& token) const {
        for (const Window& w : windows) {
            if (w.token == token) {
                return w.name;
            }
        }
        fail("a flow held by a token no window has");
    }

    std::string heldOf(const std::string& account) const {
        for (const sync::SecretRequest& r : hub->secrets().holding()) {
            if (r.account == account) {
                const auto it = heldBySecret.find(r.id);
                if (it == heldBySecret.end()) {
                    fail("a login held that the walk never printed");
                }
                return it->second;
            }
        }
        return kNone;
    }

    // The paste open for `account` (the model's ask): its request id, or 0.
    std::uint64_t asked(const std::string& account) const {
        for (const sync::SecretRequest& r : hub->secrets().open()) {
            if (r.account == account) {
                return r.id;
            }
        }
        return 0;
    }

    std::string tokenOfFile(const std::string& account) const {
        const auto it = keeper.files.find(account);
        if (it == keeper.files.end()) {
            return kNone;
        }
        for (std::size_t i = 0; i < kLogins.size(); ++i) {
            if (kLogins[i] == it->second) {
                return kTokens[i];
            }
        }
        fail("a file holds a login the walk never printed");
    }

    static std::string val(const std::string& t) { return t == kNone ? q(t) : t; }

    template <class F>
    std::string byClient(F f) const {
        std::string out = "(";
        for (std::size_t i = 0; i < windows.size(); ++i) {
            out += std::string(i == 0 ? "" : " @@ ") + q(windows[i].name) + " :> " + f(windows[i]);
        }
        return out + ")";
    }
    template <class F>
    static std::string byAcct(F f) {
        std::string out = "(";
        for (std::size_t i = 0; i < kAccts.size(); ++i) {
            out += std::string(i == 0 ? "" : " @@ ") + q(kAccts[i]) + " :> " + f(kAccts[i]);
        }
        return out + ")";
    }
    template <class F>
    static std::string byId(F f) {
        std::string out = "(";
        for (std::uint64_t id = 1; id <= kMaxFlows; ++id) {
            out += std::string(id == 1 ? "" : " @@ ") + std::to_string(id) + " :> " + f(id);
        }
        return out + ")";
    }
    static std::string ids(const std::set<std::uint64_t>& s) {
        std::string out = "{";
        bool first = true;
        for (std::uint64_t id : s) {
            out += std::string(first ? "" : ", ") + std::to_string(id);
            first = false;
        }
        return out + "}";
    }
    static std::string flight(const Flight& m) {
        return "[kind |-> " + q(m.kind) + ", id |-> " + std::to_string(m.id) + ", c |-> " + q(m.c) + ", match |-> " +
            b(m.match) + ", ok |-> " + b(m.ok) + "]";
    }

    std::string viewOf(const Window& w) const {
        std::set<std::string> items;
        for (const sync::SignInOffer& o : w.opener.view()) {
            std::string opener = kNone;
            if (o.opener == w.token) {
                opener = w.name;
            } else if (o.opener == sync::kSignInHeldBy) {
                opener = "held";
            } else if (!o.opener.empty()) {
                fail("window " + w.name + " was told another window's claim token");
            }
            items.insert("[id |-> " + std::to_string(o.id) + ", acct |-> " + q(o.account) + ", opener |-> " +
                         q(opener) + ", url |-> " + b(!o.url.empty()) + "]");
        }
        if (!w.leak.empty()) {
            items.insert("[id |-> 0, acct |-> " + w.leak + ", opener |-> " + q(kNone) + ", url |-> FALSE]");
        }
        std::string out = "{";
        bool first = true;
        for (const std::string& x : items) {
            out += std::string(first ? "" : ", ") + x;
            first = false;
        }
        return out + "}";
    }

    // The code's state as the model's variables; with `unminted`, the flows from that id on as not yet minted.
    std::string record(std::uint64_t unminted = 0) const {
        const std::uint64_t nextShown = unminted != 0 ? unminted : nextId();
        std::ostringstream o;
        o << "next |-> " << nextShown;
        o << ", flow |-> " << byId([&](std::uint64_t id) {
            auto it = flows.find(id);
            const FlowRec f = it == flows.end() || id >= nextShown ? FlowRec{} : it->second;
            return "[st |-> " + q(f.st) + ", acct |-> " + q(f.acct) + ", opener |-> " + q(f.opener) +
                ", denied |-> " + b(f.denied) + "]";
        });
        o << ", proc |-> " << byAcct([&](const std::string& a) {
            const Proc& p = proc.at(a);
            return "[st |-> " + q(p.st) + ", id |-> " + std::to_string(p.id) + ", ending |-> " + b(p.ending) + "]";
        });
        o << ", procs |-> " << byAcct([&](const std::string& a) { return std::to_string(procs.at(a)); });
        o << ", cbs |-> " << byId([&](std::uint64_t id) {
            auto it = cbs.find(id);
            return std::to_string(it == cbs.end() ? 0 : it->second);
        });
        o << ", badcb |-> " << ids(badcb);
        o << ", signedin |-> " << byAcct([&](const std::string& a) { return b(hub->signedIn().count(a) != 0); });
        o << ", losses |-> " << losses;
        o << ", conn |-> " << byClient([](const Window& w) { return b(w.conn); });
        o << ", armed |-> " << byClient([](const Window& w) { return b(w.opener.armed()); });
        o << ", view |-> " << byClient([this](const Window& w) { return viewOf(w); });
        o << ", claimed |-> " << byClient([](const Window& w) { return ids(w.opener.claimed()); });
        o << ", opened |-> " << byClient([](const Window& w) { return ids(w.opener.opened()); });
        o << ", browser |-> " << byClient([](const Window& w) { return ids(w.browser); });
        o << ", gate |-> " << byClient([](const Window& w) { return std::to_string(w.opener.gate()); });
        std::set<std::string> flights;
        for (const Flight& m : up) {
            flights.insert(flight(m));
        }
        o << ", up |-> {";
        bool first = true;
        for (const std::string& f : flights) {
            o << (first ? "" : ", ") << f;
            first = false;
        }
        o << "}";
        o << ", file |-> " << byAcct([this](const std::string& a) { return val(tokenOfFile(a)); });
        o << ", held |-> " << byAcct([this](const std::string& a) { return val(heldOf(a)); });
        o << ", disk |-> " << q(keeper.failing ? "failing" : "ok") << ", failures |-> " << failures;
        o << ", taken |-> " << byId([&](std::uint64_t id) {
            auto it = taken.find(id);
            return val(it == taken.end() ? std::string(kNone) : it->second);
        });
        o << ", lost |-> " << ids(lost) << ", restarts |-> " << restarts;
        o << ", said |-> " << byId([&](std::uint64_t id) {
            auto it = said.find(id);
            return val(it == said.end() ? std::string(kNone) : it->second);
        });
        o << ", ask |-> " << byAcct([this](const std::string& a) { return b(asked(a) != 0); });
        return o.str();
    }

    // The window receives its own state from the server, in format 3; true when its view or its gate changed.
    bool sync(Window& w) {
        const std::string before = viewOf(w) + std::to_string(w.opener.gate());
        sync::Sessions state = hub->stateFor(w.token);
        state.setFormat(sync::kStateFormat);
        const std::string diff = state.diff_from(w.got);
        if (const std::string t = leaked(diff); !t.empty()) {
            w.leak = t;
        }
        w.got.apply_string(diff);
        w.opener.received(w.got.offers());
        settleGate(w);
        return viewOf(w) + std::to_string(w.opener.gate()) != before;
    }

    // The walk's gate object follows the opener's: none once it has closed.
    static void settleGate(Window& w) {
        if (w.opener.gate() == 0 || w.opener.gate() != w.gateFlow) {
            w.gate.reset();
            w.gateFlow = 0;
        }
    }

    void send(Window& w, sync::Action action, const std::string& kind, bool match, bool ok) {
        action.text = w.token;
        // Each action under a client of its own, numbered 1: the flights arrive in any order (SignIn.tla up is a set),
        // which one client's counter would refuse; the counter is the link's, not sign-in's.
        ++num;
        sync::Actions mine(1);
        mine.setFormat(sync::kStateFormat);
        sync::Actions base(1);
        base.setFormat(sync::kStateFormat);
        const std::uint64_t id = action.request;
        mine.add(std::move(action));
        up.push_back(Flight{mine.diff_from(base), kind, id, w.name, match, ok, w.name + "-" + std::to_string(num)});
    }

    Window& window(const std::string& name) {
        for (Window& w : windows) {
            if (w.name == name) {
                return w;
            }
        }
        fail("no window " + name);
    }
};

bool onServersMachine(const sync::SignInOffer&, const void* context) {
    return !static_cast<const Window*>(context)->remote;
}

} // namespace

int main(int argc, char** argv) {
    std::string faulty;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-claim") == 0) {
            faulty = "claim";
        } else if (std::strcmp(argv[i], "--faulty-callback") == 0) {
            faulty = "callback";
        } else if (std::strcmp(argv[i], "--faulty-stray") == 0) {
            faulty = "stray";
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: signin_walk <seed> <steps> [--faulty-claim | --faulty-callback | --faulty-stray] "
                             "<out.tla>\n");
        return 2;
    }
    rngState = std::strtoull(args[0].c_str(), nullptr, 10) * 0x9E3779B97F4A7C15ULL + 1;
    const long steps = std::strtol(args[1].c_str(), nullptr, 10);

    Walk w(faulty);
    w.start(false);
    w.readFlows();
    std::vector<std::string> records;
    records.push_back("[op |-> \"init\", " + w.record() + "]");
    std::map<std::string, std::size_t> counts;

    // Writes the step's records: the step with any flows it minted not yet there, then a need for each.
    auto emit = [&](const std::string& op, const std::string& fields, std::uint64_t nextBefore) {
        w.readFlows();
        const std::uint64_t nextAfter = w.nextId();
        if (op != "need") {
            records.push_back("[op |-> " + q(op) + (fields.empty() ? "" : ", " + fields) + ", " +
                              w.record(nextAfter > nextBefore ? nextBefore : 0) + "]");
            ++counts[op];
        }
        for (std::uint64_t id = nextBefore; id < nextAfter; ++id) {
            records.push_back("[op |-> \"need\", a |-> " + q(w.flows[id].acct) + ", " +
                              w.record(id + 1 < nextAfter ? id + 1 : 0) + "]");
            ++counts["need"];
        }
    };

    // The first start's needs: one flow for each account with no login.
    {
        const std::uint64_t before = w.nextId();
        w.wire();
        emit("need", "", before);
    }

    for (long step = 0; step < steps; ++step) {
        ++w.now;
        if (w.nextId() + kAccts.size() > kMaxFlows) {
            break;
        }
        const std::uint64_t nextBefore = w.nextId();
        std::string op;
        std::string fields;
        const std::size_t choice = pick(1000);
        Window& c = w.windows[pick(w.windows.size())];
        const std::string a = kAccts[pick(kAccts.size())];
        Proc& p = w.proc[a];
        if (choice < 60) {
            if (!c.conn || c.opener.armed()) {
                continue;
            }
            op = "show";
            c.opener.shown();
            fields = "c |-> " + q(c.name);
        } else if (choice < 80) {
            if (!c.opener.armed()) {
                continue;
            }
            op = "hide";
            c.opener.hidden();
            fields = "c |-> " + q(c.name);
        } else if (choice < 300) {
            if (!c.conn || !w.sync(c)) {
                continue;
            }
            op = "sync";
            fields = "c |-> " + q(c.name);
        } else if (choice < 420) {
            if (!c.conn) {
                continue;
            }
            const sync::SignInOpener::Step s = c.opener.next(onServersMachine, &c);
            if (s.kind == sync::SignInOpener::Step::Kind::None) {
                continue;
            }
            if (s.kind == sync::SignInOpener::Step::Kind::Refused) {
                fail("a window refused a sign-in URL the server offered");
            }
            sync::Action action;
            action.request = s.id;
            if (s.kind == sync::SignInOpener::Step::Kind::Claim) {
                op = "claim";
                action.kind = sync::ActionKind::SignInClaim;
                w.send(c, std::move(action), "claim", true, true);
            } else {
                op = "open";
                c.browser.insert(s.id);
                c.pages[s.id] = s.target;
                if (s.gate) {
                    c.gate = std::make_unique<sync::CallbackGate>(s.target);
                    c.gateFlow = s.id;
                    if (faulty == "stray") {
                        c.gate->injectStrayFault();
                    }
                }
                action.kind = sync::ActionKind::SignInOpened;
                w.send(c, std::move(action), "opened", true, true);
            }
            fields = "c |-> " + q(c.name) + ", i |-> " + std::to_string(s.id);
        } else if (choice < 445) {
            if (!c.conn || !c.remote || !c.gate) continue;
            sync::Action action;
            const auto id = c.opener.cancelFor(a, &action.secret);
            if (!id) continue;
            const bool match = sync::queryValue(action.secret.bytes(), "state") == stateOf(id);
            const bool denied = sync::queryValue(action.secret.bytes(), "error") == "access_denied";
            if (!match || !denied) fail("account switch did not decline its own callback");
            c.browser.erase(id);
            Walk::settleGate(c);
            action.kind = sync::ActionKind::SignInCallback;
            action.request = id;
            w.send(c, std::move(action), "callback", match, false);
            op = "cancel";
            fields = "c |-> " + q(c.name) + ", i |-> " + std::to_string(id);
        } else if (choice < 500) {
            if (c.browser.empty()) {
                continue;
            }
            auto it = c.browser.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(pick(c.browser.size())));
            const std::uint64_t i = *it;
            const bool ok = pick(5) != 0;
            op = "redirect";
            c.browser.erase(i);
            const sync::SignInTarget& page = c.pages.at(i);
            const std::string target = page.path + "?" + (ok ? "code=" + codeOf(i) : std::string("error=access_denied")) +
                "&state=" + page.state;
            if (c.remote) {
                // The browser on the window's machine: to the gate, when it listens on this page's port.
                if (c.gate && c.gateFlow == i) {
                    sync::SecretValue query;
                    std::string response;
                    const sync::CallbackGate::Verdict v =
                        c.gate->take("GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n", &query, &response);
                    if (v == sync::CallbackGate::Verdict::Take) {
                        sync::Action action;
                        action.kind = sync::ActionKind::SignInCallback;
                        action.request = i;
                        action.secret = query;
                        const bool match = sync::queryValue(query.bytes(), "state") == stateOf(i);
                        const bool code = !sync::queryValue(query.bytes(), "code").empty();
                        w.send(c, std::move(action), "callback", match, code);
                        c.opener.gateClosed();
                        Walk::settleGate(c);
                    }
                }
            } else {
                // The browser on the server's machine: to the listener of the process serving this page, if any.
                for (const std::string& acct : kAccts) {
                    Proc& lp = w.proc[acct];
                    if (lp.id == i && lp.st == "url" && page.state == stateOf(i)) {
                        lp.st = ok ? "got-code" : "got-denied";
                    }
                }
            }
            fields = "c |-> " + q(c.name) + ", i |-> " + std::to_string(i) + ", ok |-> " + b(ok);
        } else if (choice < 530) {
            if (!c.gate) {
                continue;
            }
            op = "stray";
            const std::uint64_t i = c.gateFlow;
            const char* const strays[] = {"GET /favicon.ico HTTP/1.1\r\n\r\n",
                                          "GET /callback?code=fakeWalkStrayCo&state=fakeWalkOtherSt HTTP/1.1\r\n\r\n",
                                          "POST /callback HTTP/1.1\r\n\r\n"};
            sync::SecretValue query;
            std::string response;
            if (c.gate->take(strays[pick(3)], &query, &response) == sync::CallbackGate::Verdict::Take) {
                sync::Action action;
                action.kind = sync::ActionKind::SignInCallback;
                action.request = i;
                action.secret = query;
                w.send(c, std::move(action), "callback", sync::queryValue(query.bytes(), "state") == stateOf(i), true);
                c.opener.gateClosed();
                Walk::settleGate(c);
                ++w.faults;
            }
            fields = "c |-> " + q(c.name);
        } else if (choice < 545) {
            if (!c.conn) {
                continue;
            }
            op = "disconnect";
            c.conn = false;
            c.opener.lost();
            Walk::settleGate(c);
            c.got = sync::Sessions();
            c.got.setFormat(sync::kStateFormat);
            c.leak.clear();
            w.hub->signInWindowGone(c.token);
            fields = "c |-> " + q(c.name);
        } else if (choice < 560) {
            if (c.conn) {
                continue;
            }
            op = "connect";
            c.conn = true;
            fields = "c |-> " + q(c.name);
        } else if (choice < 760) {
            if (w.up.empty()) {
                continue;
            }
            op = "receive";
            const std::size_t i = pick(w.up.size());
            Flight m = w.up[i];
            w.up.erase(w.up.begin() + static_cast<std::ptrdiff_t>(i));
            const std::vector<sync::Action> actions = sync::Actions::parse(m.wire, sync::kStateFormat);
            sync::wipeBytes(m.wire);
            if (actions.size() != 1) {
                fail("an action in flight read back as " + std::to_string(actions.size()));
            }
            std::string why;
            if (!w.hub->apply(actions[0], &why, m.client)) {
                fail("the hub refused an action: " + why);
            }
            // A callback is delivered again, as a window resends an action it has no acknowledgement for, under a
            // number of its own: the server replays it once all the same.
            if (m.kind == "callback") {
                sync::Action again = actions[0];
                again.num = 1;
                w.hub->apply(again, &why, m.client + "-again");
            }
            fields = "m |-> " + Walk::flight(m);
        } else if (choice < 810) {
            if (p.st != "running" || w.hub->signIns().stateOf(p.id) != sync::SignInState::Started) {
                continue;
            }
            op = "url";
            w.hub->signInUrl(a, p.id, sync::SecretValue(urlOf(p.id)));
            p.st = "url";
            fields = "a |-> " + q(a);
        } else if (choice < 860) {
            if (p.st != "got-code") {
                continue;
            }
            // One held login at a time, so that a retry writes one (the model's Retry is per account).
            const bool otherHeld = std::any_of(kAccts.begin(), kAccts.end(),
                                               [&](const std::string& x) { return w.heldOf(x) != kNone; });
            if (w.keeper.failing && otherHeld) {
                continue;
            }
            const std::uint64_t id = p.id;
            const bool ending = p.ending;
            --w.procs[a];
            p = Proc{};
            if (pick(6) == 0) {
                // An API key: refused, the model's Exit; from a process told to end (its flow ended), dropped, and the
                // process ends: the model's End.
                op = ending ? "end" : "exit";
                w.hub->signInPrinted(a, sync::SecretValue(kApiKey));
                fields = "a |-> " + q(a);
            } else {
                // In time, or late (the process told to end, its flow expired): taken either way.
                op = "print";
                if (ending) {
                    ++counts["print-late"];
                }
                if (w.asked(a) != 0) {
                    ++counts["print-while-asked"];
                }
                const std::size_t v = pick(kLogins.size());
                std::set<std::uint64_t> before;
                for (const sync::SecretRequest& r : w.hub->secrets().holding()) {
                    before.insert(r.id);
                }
                const std::size_t writes = w.keeper.writes;
                w.hub->signInPrinted(a, sync::SecretValue(kLogins[v]));
                w.said[id] = kTokens[v];
                // Taken as the code took it: written to the account's file now, or held for another write. A login the
                // code dropped is in neither, and EveryPrintTaken fails on the trace.
                bool took = w.keeper.writes != writes && w.keeper.files[a] == kLogins[v];
                for (const sync::SecretRequest& r : w.hub->secrets().holding()) {
                    if (before.count(r.id) == 0) {
                        w.heldBySecret[r.id] = kTokens[v];
                        took = took || r.account == a;
                    }
                }
                if (took) {
                    w.taken[id] = kTokens[v];
                }
                fields = "a |-> " + q(a) + ", v |-> " + kTokens[v];
            }
        } else if (choice < 870) {
            if (p.st == kNone) {
                continue;
            }
            const std::uint64_t id = p.id;
            const bool ending = p.ending;
            --w.procs[a];
            p = Proc{};
            if (ending) {
                // A process told to end ends: the runner reports nothing (its flow ended first).
                op = "end";
            } else {
                op = "exit";
                w.hub->signInExited(a, id, "it exited 1");
            }
            fields = "a |-> " + q(a);
        } else if (choice < 875) {
            const std::optional<std::uint64_t> due = w.hub->signInDeadline();
            if (!due) {
                continue;
            }
            op = "timeout";
            std::set<std::uint64_t> running;
            for (const auto& [id, f] : w.flows) {
                const std::optional<sync::SignInState> s = w.hub->signIns().stateOf(id);
                if (s && sync::signInRunning(*s)) {
                    running.insert(id);
                }
            }
            w.now = std::max(w.now, *due);
            w.hub->expireSignIns();
            std::uint64_t ended = 0;
            for (std::uint64_t id : running) {
                if (w.hub->signIns().stateOf(id) == sync::SignInState::Expired) {
                    if (ended != 0) {
                        fail("two flows expired in one step");
                    }
                    ended = id;
                }
            }
            if (ended == 0) {
                fail("a deadline passed and no flow expired");
            }
            fields = "id |-> " + std::to_string(ended);
        } else if (choice < 880) {
            // Only windows from before sign-ins are linked a moment: a flow no window holds (needed, or running after
            // its window went) ends, its process told to end, and its account is asked for by a paste instead (the
            // model's FallBack, one flow a step: so only with one such flow). The paste stays open until "askend", or
            // until a late login closes it.
            std::vector<std::uint64_t> unheld;
            for (const auto& [id, f] : w.flows) {
                const std::optional<sync::SignInState> s = w.hub->signIns().stateOf(id);
                if (s && (*s == sync::SignInState::Needed || (sync::signInRunning(*s) && f.opener == kNone))) {
                    unheld.push_back(id);
                }
            }
            // A few a walk: each spends a flow id, and the walk ends when they run out.
            if (unheld.size() != 1 || counts["fallback"] >= 6) {
                continue;
            }
            const std::uint64_t i = unheld[0];
            const std::string acct = w.flows[i].acct;
            const bool wasRunning = w.hub->signIns().stateOf(i) != sync::SignInState::Needed;
            op = "fallback";
            w.oldOnly = true;
            w.hub->signInWindowsChanged();
            w.oldOnly = false;
            if (w.hub->signIns().stateOf(i) != sync::SignInState::Dropped) {
                fail("a flow no window holds did not end when only older windows were linked");
            }
            if (wasRunning) {
                ++counts["fallback-running"];
                if (w.proc[acct].id == i && w.proc[acct].st != kNone && !w.proc[acct].ending) {
                    fail("a running flow fell back to a paste, and its process was not told to end");
                }
            }
            const bool expect = w.hub->signedIn().count(acct) == 0 && w.heldOf(acct) == kNone;
            if (expect != (w.asked(acct) != 0)) {
                fail(expect ? "a flow ended for older windows, and no paste was asked for" :
                              "a paste was asked for an account signed in, or whose login is held");
            }
            fields = "id |-> " + std::to_string(i);
        } else if (choice < 888) {
            // The paste asked for ends without a value (the person cancels it); then a newer window back, or an AI's
            // `config sign-in`, needs the account again (a "need" record after this one, if it mints a flow).
            const std::uint64_t r = w.asked(a);
            if (r == 0) {
                continue;
            }
            op = "askend";
            sync::Action cancel;
            cancel.kind = sync::ActionKind::SecretCancel;
            cancel.request = r;
            std::string why;
            if (!w.hub->apply(cancel, &why, std::string())) {
                fail("the hub refused a paste's cancel: " + why);
            }
            std::string said;
            w.hub->requestSignIn(a, &said);
            fields = "a |-> " + q(a);
        } else if (choice < 905) {
            if (w.hub->signedIn().count(a) == 0 || w.losses >= kMaxLosses) {
                continue;
            }
            op = "lose";
            ++w.losses;
            w.hub->reportSignedIn(a, false);
            fields = "a |-> " + q(a);
        } else if (choice < 945) {
            const std::optional<std::uint64_t> due = w.hub->secretRetryAt();
            if (!due || w.keeper.failing) {
                continue;
            }
            std::string heldAcct;
            for (const std::string& x : kAccts) {
                if (w.heldOf(x) != kNone) {
                    heldAcct = x;
                }
            }
            op = "retry";
            w.now = std::max(w.now, *due);
            w.hub->retrySecrets();
            fields = "a |-> " + q(heldAcct);
        } else if (choice < 949) {
            if (w.keeper.failing || w.failures >= kMaxFailures) {
                continue;
            }
            op = "diskfail";
            w.keeper.failing = true;
            ++w.failures;
        } else if (choice < 990) {
            if (!w.keeper.failing) {
                continue;
            }
            op = "diskheal";
            w.keeper.failing = false;
        } else {
            if (w.restarts >= kMaxRestarts || step < steps / 3) {
                continue;
            }
            op = "restart";
            for (const auto& [id, t] : w.taken) {
                if (w.heldOf(w.flows[id].acct) == t) {
                    w.lost.insert(id);
                }
            }
            w.hub->dropSignIns();
            w.readFlows();
            w.signedBefore.clear();
            for (const std::string& x : kAccts) {
                if (w.hub->signedIn().count(x) != 0) {
                    w.signedBefore.insert(x);
                }
                w.proc[x] = Proc{};
                w.procs[x] = 0;
            }
            w.hub.reset();
            w.heldBySecret.clear();
            w.start(true);
            ++w.restarts;
        }
        emit(op, fields, nextBefore);
    }

    std::ofstream out(args[2]);
    out << "---- MODULE SignInTraceData ----\n"
           "\\* Written by tests/sync/signin_walk: seed " << args[0] << ", " << steps << " steps"
        << (faulty.empty() ? std::string() : ", faulty " + faulty) << ".\n"
        << "EXTENDS TLC\n"
        << "CONSTANTS t1, t2\n"
        << "TraceClients == {\"c1\", \"c2\"}\n"
        << "TraceRemote == {\"c2\"}\n"
        << "TraceAccounts == {\"a1\", \"a2\"}\n"
        << "TraceTokens == {t1, t2}\n"
        << "TraceMaxFlows == " << kMaxFlows << "\n"
        << "TraceMaxRestarts == " << kMaxRestarts << "\n"
        << "TraceMaxFailures == " << kMaxFailures << "\n"
        << "TraceMaxLosses == " << kMaxLosses << "\n"
        << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    if (!out) {
        std::fprintf(stderr, "signin_walk: could not write %s\n", args[2].c_str());
        return 1;
    }
    std::string coverage;
    for (const auto& [name, n] : counts) {
        coverage += " " + name + "=" + std::to_string(n);
    }
    std::printf("signin_walk: %zu records; taken:%s%s\n", records.size(), coverage.c_str(),
                faulty.empty() ? "" : (", " + std::to_string(w.faults) + " faults played").c_str());
    if (!faulty.empty() && w.faults == 0) {
        std::fprintf(stderr, "signin_walk: the faulty walk never hit its fault; try more steps\n");
        return 1;
    }
    return 0;
}
