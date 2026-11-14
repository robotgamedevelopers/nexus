// Trace walk: drives the secret dialog's code (src/sync/secrets.h, through src/sync/hub.h over a MemoryStore) with a
// seeded random walk and writes the trace as SecretsTraceData.tla, for TLC to check against models/Secrets.tla
// (models/SecretsTrace.tla). No files, no clock, no sockets: the time is the walk's counter, a value's file is the
// walk's keeper in memory, and the values are fake ones the walk makes up.
//
//   secrets_walk <seed> <steps> [--faulty-store-late | --faulty-echo] <out.tla>
//
// The pieces are the real ones, wired as the server and the windows wire them: the hub's askSecret, apply,
// expireSecrets and dropSecrets (a restart is a new Hub over the same store); each window's state is the hub's
// Sessions sent as a diff in the window's format (3, or 2 for the old window, which never receives a request) and
// applied to the window's copy, then given to its SecretPrompt; each answer or cancel is an Action encoded by Actions in
// format 3 and parsed back by Actions::parse before the hub applies it. After each step the walk records the code's
// state as the model's variables: the next id (the store's counter), each request's state as the hub reports it, each
// window's link, view, dialog and answered ids, the answers in flight, each account's file, the writes of each
// request's value, the askers' answers, and the step's session lines and log lines.
//
// It also checks, at every step, that no value's bytes are anywhere they must never be: a session line or any entry of
// the store, a log line, an answer to an asker, a diff of the state sent to any window, or an action's description.
//
// The faults show the binding can fail, each played by the walk after the step:
//   --faulty-store-late  an answer for a request no longer open is written to the account's file anyway
//   --faulty-echo        a stored answer's log line carries the value

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
#include "sync/actions.h"
#include "sync/hub.h"
#include "sync/secrets.h"
#include "sync/sessions.h"

namespace {

using namespace nexus;

const std::vector<std::string> kAccts{"a1", "a2"};
const std::vector<std::string> kClients{"c1", "c2", "c3"};
const std::string kOld = "c3";             // a window from before secret requests (state format 2)
constexpr std::uint64_t kMaxRequests = 16;
constexpr std::uint64_t kMaxRestarts = 2;
const char* const kNone = "none";
const char* const kWhat = "oauth-token";

// The fake values, made up here: never a real token. Each is a token of the model (v1, v2).
const std::vector<std::string> kValues{"fake-secret-walk-7Hq2-one", "fake-secret-walk-9Zx4-two"};
const std::vector<std::string> kTokens{"v1", "v2"};

std::uint64_t rngState = 0;
std::uint64_t next() {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return rngState * 2685821657736338717ULL;
}
std::size_t pick(std::size_t n) { return static_cast<std::size_t>(next() % n); }

std::string q(const std::string& s) { return "\"" + s + "\""; }

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "secrets_walk: %s\n", what.c_str());
    std::exit(1);
}

// Fails if any fake value's bytes are in `text`, which `where` names.
void clean(const std::string& text, const std::string& where) {
    for (const std::string& v : kValues) {
        if (text.find(v) != std::string::npos) {
            fail("a value reached " + where);
        }
    }
}

// The token of a value, or of what was pasted for it (with a line break or spaces at its ends).
std::string tokenOf(const std::string& pasted) {
    const std::size_t first = pasted.find_first_not_of(" \t\r\n");
    const std::size_t last = pasted.find_last_not_of(" \t\r\n");
    const std::string value = first == std::string::npos ? std::string() : pasted.substr(first, last - first + 1);
    for (std::size_t i = 0; i < kValues.size(); ++i) {
        if (kValues[i] == value) {
            return kTokens[i];
        }
    }
    fail("a value the walk never typed: " + std::to_string(pasted.size()) + " bytes");
}

// The account's file, in memory: what the server's AccountFiles writes to disk.
struct Keeper : sync::SecretKeeper {
    std::map<std::string, std::string> files;   // account: the value's bytes
    std::size_t writes = 0;
    bool keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
              std::string* why) override {
        if (what != kWhat) {
            if (why != nullptr) {
                *why = "not a file of the account";
            }
            return false;
        }
        files[account] = value.bytes();
        ++writes;
        return true;
    }
};

struct Events : sync::SecretEvents {
    std::vector<sync::SecretReply> replies;     // this step's
    std::vector<sync::SecretNote> notes;
    std::vector<sync::SecretLog> log;
    void answered(const sync::SecretReply& reply) override { replies.push_back(reply); }
    void logged(const sync::SecretLog& line) override { log.push_back(line); }
    void noted(const sync::SecretNote& note) override { notes.push_back(note); }
};

struct Window {
    std::string name;
    bool old = false;
    bool conn = true;
    sync::Sessions got;             // the state as the window last received it
    sync::SecretPrompt prompt;
};

struct Flight {
    std::string wire;               // the action as it travels: Actions in format 3
    std::uint64_t id = 0;
    bool secret = false;
    std::string token;              // the value's token, or none for a cancel
};

struct Req {
    std::string st = "unused";
    std::string acct = kNone;
    std::string asker = kNone;
};

struct Walk {
    sync::MemoryStore store;
    std::unique_ptr<sync::Hub> hub;
    Keeper keeper;
    Events events;
    std::uint64_t now = 1000;
    std::vector<Window> windows;
    std::vector<Flight> up;
    std::map<std::uint64_t, Req> req;           // what each id was asked as, and its state as last reported
    std::map<std::uint64_t, std::size_t> stores;
    std::set<std::string> replies;              // every answer so far, as model records
    std::set<std::uint64_t> answeredIds;
    std::vector<std::string> transcript;        // this step's lines, as model records
    std::vector<std::string> log;
    std::uint64_t restarts = 0;
    std::uint64_t num = 0;
    std::map<sync::SessionId, std::string> askerNames{{0, "nexus"}};

    Walk() {
        for (const std::string& c : kClients) {
            Window w;
            w.name = c;
            w.old = c == kOld;
            w.got.setFormat(w.old ? 2 : sync::kSecretsFormat);
            windows.push_back(std::move(w));
        }
        // Two friends' sessions, whose AIs ask through their drivers.
        hub = std::make_unique<sync::Hub>(store);
        for (const char* f : {"f", "g"}) {
            std::string why;
            sync::Action a;
            a.num = ++num;
            a.kind = sync::ActionKind::Ensure;
            a.friendName = f;
            hub->apply(a, &why, "walk");
        }
        for (sync::SessionId id : store.ids()) {
            askerNames[id] = "s" + std::to_string(id);
        }
        if (askerNames.size() != 3 || askerNames.count(1) == 0 || askerNames.count(2) == 0) {
            fail("the askers' sessions are not 1 and 2");
        }
        configure();
    }

    // The server's start: a hub over the store, with its accounts and the secret dialog wired.
    void start() {
        hub = std::make_unique<sync::Hub>(store);
        configure();
    }

    void configure() {
        accounts::Registry r;
        for (const std::string& a : kAccts) {
            r.accounts.push_back(accounts::Account{a, "claude"});
        }
        hub->useAccounts(r, nullptr);
        hub->secretsWith(&keeper, &events, [this] {
            return std::any_of(windows.begin(), windows.end(), [](const Window& w) { return w.conn && !w.old; });
        }, [this] { return now; });
    }

    std::uint64_t nextId() const { return store.applied(sync::kSecretIdsClient) + 1; }

    // The step's effects, as the model's records; every one of them checked for a value.
    void take(bool echo) {
        transcript.clear();
        log.clear();
        for (const sync::SecretNote& n : events.notes) {
            clean(n.text, "a session line");
            transcript.push_back("[id |-> " + std::to_string(n.id) + ", what |-> " + q(n.what) + ", acct |-> " +
                                 q(n.account) + "]");
        }
        for (const sync::SecretLog& l : events.log) {
            clean(l.line(), "a log line");
            std::string event = l.event;
            if (echo && event == "stored") {
                event = tokenOf(keeper.files[l.account]);
            }
            log.push_back("[id |-> " + std::to_string(l.id) + ", event |-> " + q(event) + "]");
        }
        for (const sync::SecretReply& r : events.replies) {
            clean(r.reason, "an answer to an asker");
            if (!answeredIds.insert(r.id).second) {
                fail("request " + std::to_string(r.id) + " was answered twice");
            }
            replies.insert("[id |-> " + std::to_string(r.id) + ", asker |-> " + q(askerNames.at(r.asker.session)) +
                           ", outcome |-> " + q(r.stored ? "stored" : "refused") + ", reason |-> " +
                           q(r.stored ? kNone : r.reason) + "]");
        }
        events.replies.clear();
        events.notes.clear();
        events.log.clear();
        // Every entry the store holds, every session line included.
        for (sync::SessionId id : store.ids()) {
            for (const sync::EntryRef& e : store.find(id)->transcript) {
                clean(e->text, "the store (session " + std::to_string(id) + ")");
            }
        }
        // Each request's state, as the hub reports it; one it no longer knows (from before a restart) keeps the last.
        for (auto& [id, r] : req) {
            if (const std::optional<sync::SecretState> s = hub->secrets().stateOf(id)) {
                static const char* const names[] = {"open", "stored", "refused", "dropped"};
                r.st = names[static_cast<int>(*s)];
            }
        }
    }

    std::string viewOf(const Window& w) const {
        std::string out = "{";
        bool first = true;
        for (const sync::SecretRequest& r : w.prompt.view()) {
            out += std::string(first ? "" : ", ") + "[id |-> " + std::to_string(r.id) + ", acct |-> " + q(r.account) +
                ", asker |-> " + q(askerNames.at(r.asker)) + "]";
            first = false;
        }
        return out + "}";
    }

    std::string dialogOf(const Window& w) const {
        const sync::SecretRequest* r = w.prompt.showing();
        const std::string& typed = w.prompt.typed().bytes();
        return "[id |-> " + std::to_string(r != nullptr ? r->id : 0) + ", typed |-> " +
            q(typed.empty() ? std::string(kNone) : tokenOf(typed)) + "]";
    }

    std::string doneOf(const Window& w) const {
        std::string out = "{";
        bool first = true;
        for (std::uint64_t id : w.prompt.answered()) {
            out += std::string(first ? "" : ", ") + std::to_string(id);
            first = false;
        }
        return out + "}";
    }

    template <class F>
    std::string byClient(F f) const {
        std::string out = "(";
        for (std::size_t i = 0; i < windows.size(); ++i) {
            out += std::string(i == 0 ? "" : " @@ ") + q(windows[i].name) + " :> " + f(windows[i]);
        }
        return out + ")";
    }

    static std::string set(const std::vector<std::string>& items) {
        std::set<std::string> s(items.begin(), items.end());
        std::string out = "{";
        bool first = true;
        for (const std::string& x : s) {
            out += std::string(first ? "" : ", ") + x;
            first = false;
        }
        return out + "}";
    }

    static std::string flight(const Flight& m) {
        return "[id |-> " + std::to_string(m.id) + ", kind |-> " + q(m.secret ? "secret" : "cancel") + ", v |-> " +
            q(m.secret ? m.token : kNone) + "]";
    }

    std::string record() const {
        std::ostringstream o;
        o << "next |-> " << nextId() << ", req |-> (";
        for (std::uint64_t id = 1; id <= kMaxRequests; ++id) {
            auto it = req.find(id);
            const Req r = it == req.end() ? Req{} : it->second;
            o << (id == 1 ? "" : " @@ ") << id << " :> [st |-> " << q(r.st) << ", acct |-> " << q(r.acct)
              << ", asker |-> " << q(r.asker) << "]";
        }
        o << "), conn |-> " << byClient([](const Window& w) { return std::string(w.conn ? "TRUE" : "FALSE"); });
        o << ", view |-> " << byClient([this](const Window& w) { return viewOf(w); });
        o << ", dialog |-> " << byClient([this](const Window& w) { return dialogOf(w); });
        o << ", done |-> " << byClient([this](const Window& w) { return doneOf(w); });
        std::vector<std::string> flights;
        for (const Flight& m : up) {
            flights.push_back(flight(m));
        }
        o << ", up |-> " << set(flights) << ", file |-> (";
        for (std::size_t i = 0; i < kAccts.size(); ++i) {
            auto it = keeper.files.find(kAccts[i]);
            o << (i == 0 ? "" : " @@ ") << q(kAccts[i]) << " :> "
              << q(it == keeper.files.end() ? std::string(kNone) : tokenOf(it->second));
        }
        o << "), stores |-> (";
        for (std::uint64_t id = 1; id <= kMaxRequests; ++id) {
            auto it = stores.find(id);
            o << (id == 1 ? "" : " @@ ") << id << " :> " << (it == stores.end() ? 0 : it->second);
        }
        o << "), replies |-> " << set(std::vector<std::string>(replies.begin(), replies.end()))
          << ", transcript |-> " << set(transcript) << ", log |-> " << set(log) << ", restarts |-> " << restarts;
        return o.str();
    }

    // The window receives the server's state in its format: false when its view and dialog are as they were.
    bool sync(Window& w) {
        sync::Sessions state = hub->state();
        state.setFormat(w.got.format());
        const std::string diff = state.diff_from(w.got);
        clean(diff, "the state sent to window " + w.name);
        w.got.apply_string(diff);
        if (w.old) {
            if (!w.got.requests().empty()) {
                fail("the old window received a request");
            }
            return false;
        }
        const std::string before = viewOf(w) + dialogOf(w);
        w.prompt.received(w.got.requests());
        return viewOf(w) + dialogOf(w) != before;
    }

    // An answer or a cancel, as the window sends it: one action, encoded in format 3.
    void send(sync::Action action, const std::string& token) {
        clean(sync::describe(action), "an action's description");
        sync::Actions mine(++num);
        mine.setFormat(sync::kSecretsFormat);
        sync::Actions base(num);
        base.setFormat(sync::kSecretsFormat);
        const std::uint64_t id = action.request;
        const bool secret = action.kind == sync::ActionKind::SecretAnswer;
        mine.add(std::move(action));
        up.push_back(Flight{mine.diff_from(base), id, secret, token});
    }

    bool inFlight(std::uint64_t id, bool secret, const std::string& token) const {
        return std::any_of(up.begin(), up.end(), [&](const Flight& m) {
            return m.id == id && m.secret == secret && (!secret || m.token == token);
        });
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string faulty;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-store-late") == 0) {
            faulty = "store-late";
        } else if (std::strcmp(argv[i], "--faulty-echo") == 0) {
            faulty = "echo";
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: secrets_walk <seed> <steps> [--faulty-store-late | --faulty-echo] <out.tla>\n");
        return 2;
    }
    rngState = std::strtoull(args[0].c_str(), nullptr, 10) * 0x9E3779B97F4A7C15ULL + 1;
    const long steps = std::strtol(args[1].c_str(), nullptr, 10);

    Walk w;
    w.take(false);
    std::vector<std::string> records;
    records.push_back("[op |-> \"init\", " + w.record() + "]");
    std::map<std::string, std::size_t> taken;
    std::size_t faults = 0;

    for (long step = 0; step < steps; ++step) {
        ++w.now;
        std::string op;
        std::string fields;
        const std::size_t choice = pick(1000);
        Window& c = w.windows[pick(w.windows.size())];
        if (choice < 15) {
            if (w.nextId() > kMaxRequests) {
                continue;
            }
            op = "ask";
            const std::vector<sync::SessionId> askers{0, 1, 2};
            const sync::SessionId asker = askers[pick(askers.size())];
            const std::string acct = kAccts[pick(kAccts.size())];
            const std::uint64_t id = w.nextId();
            std::string refused;
            if (!w.hub->askSecret(sync::SecretAsker{asker, id, asker == 0 ? "" : "f"}, acct, kWhat, "the walk asks",
                                  &refused)) {
                fail("an ask was refused: " + refused);
            }
            w.req[id] = Req{"open", acct, w.askerNames.at(asker)};
            fields = "asker |-> " + q(w.askerNames.at(asker)) + ", acct |-> " + q(acct);
        } else if (choice < 400) {
            if (!c.conn || !w.sync(c)) {
                continue;
            }
            op = "sync";
            fields = "c |-> " + q(c.name);
        } else if (choice < 520) {
            if (c.prompt.showing() == nullptr || !c.prompt.typed().empty()) {
                continue;
            }
            op = "type";
            const std::size_t v = pick(kValues.size());
            // A paste may bring a line break or spaces at its ends, which the dialog cuts at Enter: the field holds
            // what was pasted, and the model's dialog the value it carries.
            const char* pads[] = {"", " ", "\n", "\r\n"};
            c.prompt.type(kValues[v] + pads[pick(4)]);
            fields = "c |-> " + q(c.name) + ", v |-> " + q(kTokens[v]);
        } else if (choice < 640) {
            const sync::SecretRequest* r = c.prompt.showing();
            if (r == nullptr || c.prompt.typed().empty() ||
                    w.inFlight(r->id, true, tokenOf(c.prompt.typed().bytes()))) {
                continue;
            }
            op = "submit";
            const std::string token = tokenOf(c.prompt.typed().bytes());
            sync::Action a;
            a.kind = sync::ActionKind::SecretAnswer;
            if (!c.prompt.submit(&a.request, &a.secret)) {
                fail("a typed dialog did not submit");
            }
            if (tokenOf(a.secret.bytes()) != token) {
                fail("the value sent is not the value typed");
            }
            w.send(std::move(a), token);
            fields = "c |-> " + q(c.name);
        } else if (choice < 680) {
            const sync::SecretRequest* r = c.prompt.showing();
            if (r == nullptr || w.inFlight(r->id, false, "")) {
                continue;
            }
            op = "cancel";
            sync::Action a;
            a.kind = sync::ActionKind::SecretCancel;
            c.prompt.cancel(&a.request);
            w.send(std::move(a), kNone);
            fields = "c |-> " + q(c.name);
        } else if (choice < 690) {
            if (!c.conn) {
                continue;
            }
            op = "disconnect";
            c.conn = false;
            c.prompt.lost();
            c.got = sync::Sessions();
            c.got.setFormat(c.old ? 2 : sync::kSecretsFormat);
            fields = "c |-> " + q(c.name);
        } else if (choice < 700) {
            if (c.conn) {
                continue;
            }
            op = "connect";
            c.conn = true;
            fields = "c |-> " + q(c.name);
        } else if (choice < 930) {
            if (w.up.empty()) {
                continue;
            }
            op = "receive";
            const std::size_t i = pick(w.up.size());
            Flight m = w.up[i];
            w.up.erase(w.up.begin() + static_cast<std::ptrdiff_t>(i));
            const std::vector<sync::Action> actions = sync::Actions::parse(m.wire, sync::kSecretsFormat);
            sync::wipeBytes(m.wire);
            if (actions.size() != 1) {
                fail("an action in flight read back as " + std::to_string(actions.size()));
            }
            const bool wasOpen = w.hub->secrets().isOpen(m.id);
            const std::size_t writes = w.keeper.writes;
            std::string why;
            w.hub->apply(actions[0], &why, "");
            clean(why, "the hub's reason");
            if (faulty == "store-late" && !wasOpen && m.secret) {
                std::string ignored;
                w.keeper.keep(w.req[m.id].acct, kWhat, actions[0].secret, &ignored);
                ++faults;
            }
            w.stores[m.id] += w.keeper.writes - writes;
            fields = "m |-> " + Walk::flight(m);
        } else if (choice < 950) {
            const std::optional<std::uint64_t> due = w.hub->secretDeadline();
            if (!due) {
                continue;
            }
            op = "timeout";
            std::uint64_t oldest = 0;
            for (const sync::SecretRequest& r : w.hub->secrets().open()) {
                if (oldest == 0 || r.id < oldest) {
                    oldest = r.id;
                }
            }
            w.now = *due;
            w.hub->expireSecrets();
            fields = "id |-> " + std::to_string(oldest);
        } else {
            // A restart now and then, after the first third, so that most requests run their course first.
            if (w.restarts >= kMaxRestarts || choice >= 956 || step < steps / 3) {
                continue;
            }
            op = "restart";
            w.hub->dropSecrets();
            w.take(false);  // the dropped requests, while the old hub still reports them
            const std::vector<std::string> lines = w.log;
            w.hub.reset();
            w.start();
            ++w.restarts;
            w.log = lines;
            records.push_back("[op |-> \"restart\", " + w.record() + "]");
            ++taken[op];
            continue;
        }
        const bool echo = faulty == "echo" && op == "receive" && !w.events.log.empty() &&
            w.events.log.front().event == "stored";
        if (echo) {
            ++faults;
        }
        w.take(echo);
        records.push_back("[op |-> " + q(op) + (fields.empty() ? "" : ", " + fields) + ", " + w.record() + "]");
        ++taken[op];
    }

    std::ofstream out(args[2]);
    out << "---- MODULE SecretsTraceData ----\n"
           "\\* Written by tests/sync/secrets_walk: seed " << args[0] << ", " << steps << " steps"
        << (faulty.empty() ? std::string() : ", faulty " + faulty) << ".\n"
        << "EXTENDS TLC\n"
        << "TraceClients == {\"c1\", \"c2\", \"c3\"}\n"
        << "TraceOldClients == {\"c3\"}\n"
        << "TraceAccounts == {\"a1\", \"a2\"}\n"
        << "TraceAskers == {\"nexus\", \"s1\", \"s2\"}\n"
        << "TraceValues == {\"v1\", \"v2\"}\n"
        << "TraceMaxRequests == " << kMaxRequests << "\n"
        << "TraceMaxRestarts == " << kMaxRestarts << "\n"
        << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    if (!out) {
        std::fprintf(stderr, "secrets_walk: could not write %s\n", args[2].c_str());
        return 1;
    }
    std::string coverage;
    for (const auto& [name, n] : taken) {
        coverage += " " + name + "=" + std::to_string(n);
    }
    std::printf("secrets_walk: %zu records; taken:%s%s\n", records.size(), coverage.c_str(),
                faulty.empty() ? "" : (", " + std::to_string(faults) + " faults played").c_str());
    if (!faulty.empty() && faults == 0) {
        std::fprintf(stderr, "secrets_walk: the faulty walk never hit its fault; try more steps\n");
        return 1;
    }
    return 0;
}
