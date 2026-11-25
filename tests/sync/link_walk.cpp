// Trace walk: drives the link's real code with a seeded random walk and writes the trace as LinkTraceData.tla, for TLC
// to check against models/Link.tla (models/LinkTrace.tla). No sockets, no clock: mosh's bookkeeping is the copy in
// fake_mosh.h, with datagrams lost, repeated and reordered at random.
//
//   link_walk <seed> <steps> <windows> [--faulty-applier | --faulty-mirror | --faulty-tail | --faulty-close |
//             --faulty-ensure] <out.tla>
//
// The pieces are the ones nexus runs: each window's sync::Actions (its list, as mosh's sender holds it), the server's
// sync::Applier per window and one sync::Hub over a MemoryStore with a Responder, the server's sync::Sessions as
// mosh's sender per window holds them, and each window's received Sessions mirrored into a nexus::Model by a
// sync::Mirror, which keeps which session each AI tab shows. The server starts holding one session, AI a's
// (id 1); AI b has none until a window's ensure or create makes one. The Responder stands for the drivers
// (src/server/drivers.h): a send gives the prompt and a placeholder reply, a queue puts the prompt in the queue, and a
// stop is counted. The steps it records:
//   act      a window makes an action: a send, a queue or a stop naming one of the ids the hub may hold (held or not),
//            a create for an AI (copying no session, or one of those ids), or an ensure for an AI
//   deliver  a range of a window's actions reaches the server's Applier: the range mosh's receiver gives the server
//            (get_remote_diff, as the server's link reads it), and at times a range mosh never gives on its own, to
//            exercise the Applier's other paths: one already applied, and one past a gap
//   receive  a window's receiver holds a newer state of the server's (or, after a repeat, the same): the window's
//            mirror applies it
//   grow     the AI's text in a session grows: the hub's open entry (Hub::setOpen), made or longer
//   close    the open entry's block ends: the closed entry committed to the store, and the open entry gone, as the
//            drivers do (Drivers::run: the commit, then showOpen)
//   take     a turn's end with no entry open: the queue's oldest prompt is delivered (takeQueued and the person's
//            entry, in one step of the store, as the supervisor's take gives them)
//   restart  the server ends and starts again: a new Hub over the same store, without the open entries; the turn each
//            belonged to is cut with a notice, as the drivers do when they start from the store
// Datagrams in flight and acknowledgements are not steps of the spec: mosh's own business. At the end the walk lets
// every datagram through until everything is applied and every window holds the server's newest state.
//
// The faults, for the tests that show the check can fail:
//   --faulty-applier  the server applies every action of a range it is given, even those it applied before
//   --faulty-mirror   each window mirrors with a new Mirror each time, which has forgotten what it showed
//   --faulty-tail     the server's tail is one entry longer than the spec's
//   --faulty-close    the server closes an entry without dropping the open one, so the text shows twice
//   --faulty-ensure   the server takes an ensure as a create: the AI gets a session whether or not it has one

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "fake_mosh.h"
#include "model.h"
#include "sync/actions.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#include "sync/sessions.h"

namespace {

using namespace nexus;
using namespace nexus::sync;
using test::FakeReceiver;
using test::FakeSender;
using test::Packet;
using test::Rng;

const std::vector<std::string> kAIs = {"a", "b"};
constexpr std::size_t kW = 3;
constexpr SessionId kMaxSessions = 4;      // the hub's limit: the ids the spec has are 1..4

enum class Fault { None, Applier, Mirror, Tail, Close, Ensure };

std::string str(const std::string& s) {
    return "\"" + s + "\"";
}

const char* kindName(ActionKind kind) {
    switch (kind) {
        case ActionKind::Send: return "Send";
        case ActionKind::Queue: return "Queue";
        case ActionKind::Stop: return "Stop";
        case ActionKind::Create: return "Create";
        case ActionKind::Ensure: return "Ensure";
        default: return "";
    }
}

// An action's text: "<window> <number>", the window from 1. A stop, a create and an ensure carry none.
std::string textOf(std::size_t c, std::uint64_t n) {
    return std::to_string(c + 1) + " " + std::to_string(n);
}

std::string tuple(const std::string& text) {
    const std::size_t space = text.find(' ');
    return "<<" + text.substr(0, space) + ", " + text.substr(space + 1) + ">>";
}

// The AI's grown text: "g<g>:" and one dot a piece. Its open entry's g and length, as the spec writes them.
std::string grownText(std::uint64_t g, std::size_t len) {
    return "g" + std::to_string(g) + ":" + std::string(len, '.');
}

std::string grownPair(const std::string& text) {
    const std::size_t colon = text.find(':');
    return "<<" + text.substr(1, colon - 1) + ", " + std::to_string(text.size() - colon - 1) + ">>";
}

// The notice that cuts a turn whose open entry g a restart lost.
std::string cutText(std::uint64_t g) {
    return "cut " + std::to_string(g);
}

std::string entry(EntryKind kind, const std::string& text) {
    switch (kind) {
        case EntryKind::Person: return "[k |-> \"P\", text |-> " + tuple(text) + "]";
        case EntryKind::AI:
            if (text == kReceived) {
                return "[k |-> \"F\", text |-> <<>>]";
            }
            return "[k |-> \"G\", text |-> " + grownPair(text) + "]";
        case EntryKind::Notice:
            if (text.rfind("cut ", 0) == 0) {
                return "[k |-> \"X\", text |-> <<" + text.substr(4) + ">>]";
            }
            return "[k |-> \"N\", text |-> <<" + std::to_string(std::strtoull(text.c_str(), nullptr, 10)) + ">>]";
        case EntryKind::Tool:
        case EntryKind::Bus:
            break;      // the walk generates neither tool lines nor bus messages
    }
    return "";
}

EntryRef stored(EntryKind kind, std::string text) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, std::move(text), true});
}

// The drivers' part, as the spec has it: a send is the prompt and its placeholder reply, a queue the prompt at the
// queue's end (taken later, by the walk's take), a stop counted, for the session's record.
class Recorder : public Responder {
public:
    SessionInfo defaults(const std::string& aiName) const override {
        return SessionInfo{aiName, kDefaultHarness, {}, {}, {}};
    }
    std::vector<SessionChange> send(SessionId, const std::string& prompt) override {
        return {SessionChange{stored(EntryKind::Person, prompt), {}}, SessionChange{stored(EntryKind::AI,
            kReceived), {}}};
    }
    std::vector<SessionChange> queue(SessionId, const std::string& prompt) override {
        return {SessionChange{nullptr, prompt}};
    }
    std::vector<SessionChange> stop(SessionId) override {
        ++stops;
        return {};
    }
    void sent(SessionId, bool) override {}
    void configured(SessionId, const SessionInfo&) override {}
    std::size_t stops = 0;
};

struct Window {
    FakeSender<Actions> up;              // the window's list, as mosh's sender holds it
    FakeReceiver<Sessions> down;         // the server's state, as the window's receiver holds it
    std::uint64_t downNum = 0;
    std::vector<Action> made;            // every action it made
    Model model{std::vector<AIInfo>{{"a", "m"}, {"b", "m"}}};
    std::unique_ptr<Mirror> mirror = std::make_unique<Mirror>();
    std::size_t last = 1;                // the index (from 1) of the server's state it holds
    std::vector<Packet> upFlight, downFlight;
    std::vector<std::uint64_t> upAcks, downAcks;
};

// The AI's open entry in a session, as the walk made it: g 0 is none.
struct Open {
    std::uint64_t g = 0;
    std::size_t len = 0;
};

struct Server {
    MemoryStore store;
    Recorder responder;
    Limits limits;
    std::unique_ptr<Hub> hub;
    std::vector<FakeReceiver<Actions>> up;
    std::vector<Applier> applier;
    std::vector<FakeSender<Sessions>> down;
    std::vector<Sessions> states;
    std::map<SessionId, Open> open;                         // each session's open entry
    std::map<SessionId, std::vector<std::string>> stops;    // each session's stops, as the spec writes them
    std::uint64_t made = 0, grows = 0, restarts = 0;
    Server(std::size_t windows, Limits l)
        : limits(l), hub(std::make_unique<Hub>(store, l, &responder)), up(windows), applier(windows), down(windows) {
        // The store as the server finds it: AI a's session, from before (the spec's Start).
        Action a;
        a.kind = ActionKind::Ensure;
        a.aiName = "a";
        hub->apply(a);
        states.push_back(hub->state());
        for (FakeSender<Sessions>& d : down) {
            d.current = hub->state();
        }
    }
};

class Walk {
public:
    Walk(std::size_t windows, Fault fault) : fault_(fault), server_(windows, limits(fault)), windows_(windows) {
        records_.push_back(record("[op |-> \"init\""));
    }

    void act(Rng& rng, std::size_t c) {
        Window& w = windows_[c];
        Action a;
        // Creates are rare, so that most of the walk runs with sessions to spare: ensures then count.
        const std::size_t pick = rng.below(20);
        a.kind = pick < 4 ? ActionKind::Stop : pick < 8 ? ActionKind::Queue : pick < 17 ? ActionKind::Send
               : pick < 18 ? ActionKind::Create : ActionKind::Ensure;
        if (a.kind == ActionKind::Create) {
            a.aiName = kAIs[rng.below(kAIs.size())];
            a.session = rng.chance(2) ? 0 : 1 + rng.below(kMaxSessions);
        } else if (a.kind == ActionKind::Ensure) {
            a.aiName = kAIs[rng.below(kAIs.size())];
        } else {
            // Mostly a session the window shows, at times any id.
            const SessionId shown = w.mirror->session(w.model.tab(rng.below(kAIs.size())).id);
            a.session = shown != 0 && !rng.chance(4) ? shown : 1 + rng.below(kMaxSessions);
            if (a.kind != ActionKind::Stop) {
                a.text = textOf(c, w.made.size() + 1);
            }
        }
        a.num = w.up.current.add(a);
        w.made.push_back(a);
        records_.push_back(record("[op |-> \"act\", c |-> " + std::to_string(c + 1) + ", a |-> " + target(a)));
    }

    // A datagram from the window reaches the server; the server reads what mosh's receiver gives it.
    void upArrives(Rng& rng, std::size_t c) {
        Window& w = windows_[c];
        if (w.upFlight.empty()) {
            return;
        }
        const std::size_t i = rng.below(w.upFlight.size());
        w.upAcks.push_back(server_.up[c].receive(w.upFlight[i]));
        if (!rng.chance(3)) {
            w.upFlight.erase(w.upFlight.begin() + static_cast<std::ptrdiff_t>(i));
        }
        take(c, Actions::parse(server_.up[c].remoteDiff()));
    }

    // A range mosh would not give on its own: one already applied, or one past a gap.
    void stray(Rng& rng, std::size_t c) {
        const Window& w = windows_[c];
        const std::uint64_t applied = server_.applier[c].applied();
        std::uint64_t first = 0, lst = 0;
        if (applied > 0 && rng.chance(2)) {
            first = 1 + rng.below(applied);
            lst = first + rng.below(w.made.size() - first + 1);
        } else if (w.made.size() >= applied + 2) {
            first = applied + 2 + rng.below(w.made.size() - applied - 1);
            lst = first + rng.below(w.made.size() - first + 1);
        } else {
            return;
        }
        take(c, std::vector<Action>(w.made.begin() + static_cast<std::ptrdiff_t>(first - 1),
            w.made.begin() + static_cast<std::ptrdiff_t>(lst)));
    }

    void upSend(Rng& rng, std::size_t c, bool timedOut = false) {
        windows_[c].upFlight.push_back(windows_[c].up.send(rng, timedOut));
    }

    void upAck(Rng& rng, std::size_t c) {
        Window& w = windows_[c];
        if (!w.upAcks.empty()) {
            w.up.acknowledge(w.upAcks[rng.below(w.upAcks.size())]);
        }
    }

    void downSend(Rng& rng, std::size_t c, bool timedOut = false) {
        windows_[c].downFlight.push_back(server_.down[c].send(rng, timedOut));
    }

    void downArrives(Rng& rng, std::size_t c) {
        Window& w = windows_[c];
        if (w.downFlight.empty()) {
            return;
        }
        const std::size_t i = rng.below(w.downFlight.size());
        w.downAcks.push_back(w.down.receive(w.downFlight[i]));
        if (!rng.chance(3)) {
            w.downFlight.erase(w.downFlight.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // The window mirrors when its receiver's newest state changes, and now and then again (a repeat must add
        // nothing).
        if (w.down.latestNum() == w.downNum && !rng.chance(4)) {
            return;
        }
        w.downNum = w.down.latestNum();
        std::size_t j = 0;
        for (std::size_t k = server_.states.size(); k > 0 && j == 0; --k) {
            if (server_.states[k - 1] == w.down.latest()) {
                j = k;
            }
        }
        if (j == 0) {
            // mosh's state 0, before the server's first state arrives: the empty one, which the spec does not have.
            // The window mirrors nothing from it.
            if (w.down.latest() == Sessions()) {
                return;
            }
            std::fprintf(stderr, "link_walk: window %zu holds a state the server never had\n", c + 1);
            std::exit(1);
        }
        if (j < w.last) {
            return;     // an older state than the one held: mosh never goes back (its receiver keeps the newest)
        }
        if (fault_ == Fault::Mirror) {
            w.mirror = std::make_unique<Mirror>();
        }
        w.mirror->apply(w.model, w.down.latest());
        w.last = j;
        records_.push_back(record("[op |-> \"receive\", c |-> " + std::to_string(c + 1) + ", j |-> " +
            std::to_string(j)));
    }

    void downAck(Rng& rng, std::size_t c) {
        Window& w = windows_[c];
        if (!w.downAcks.empty()) {
            server_.down[c].acknowledge(w.downAcks[rng.below(w.downAcks.size())]);
        }
    }

    // The AI's text grows in a session the server holds that has had a prompt (an AI replies to one).
    void grow(Rng& rng) {
        const SessionId id = 1 + rng.below(kMaxSessions);
        const StoredSession* s = server_.store.find(id);
        if (s == nullptr || (s->transcript.empty() && s->queue.empty())) {
            return;
        }
        Open& o = server_.open[id];
        if (o.g == 0) {
            o.g = ++server_.made;
            o.len = 0;
        }
        ++o.len;
        ++server_.grows;
        if (!server_.hub->setOpen(id, grownText(o.g, o.len))) {
            std::fprintf(stderr, "link_walk: the hub did not take the open entry of session %llu\n",
                static_cast<unsigned long long>(id));
            std::exit(1);
        }
        pushState();
        records_.push_back(record("[op |-> \"grow\", t |-> " + std::to_string(id)));
    }

    // The open entry's block ends: committed, closed, then the open entry gone (Drivers::run).
    void close(Rng& rng) {
        const SessionId id = 1 + rng.below(kMaxSessions);
        Open& o = server_.open[id];
        if (o.g == 0) {
            return;
        }
        if (!server_.hub->commit(id, {SessionChange{stored(EntryKind::AI, grownText(o.g, o.len)), {}}})) {
            std::fprintf(stderr, "link_walk: the store did not take the closed entry\n");
            std::exit(1);
        }
        if (fault_ != Fault::Close) {
            server_.hub->setOpen(id, std::string());
        }
        o = Open{};
        pushState();
        records_.push_back(record("[op |-> \"close\", t |-> " + std::to_string(id)));
    }

    // A turn's end with no entry open: the queue's oldest prompt leaves it, as the person's entry.
    void takeQueued(Rng& rng) {
        const SessionId id = 1 + rng.below(kMaxSessions);
        const StoredSession* s = server_.store.find(id);
        if (server_.open[id].g != 0 || s == nullptr || s->queue.empty()) {
            return;
        }
        const std::string prompt = s->queue.front();
        if (!server_.hub->commit(id, {sync::takeQueued(), SessionChange{stored(EntryKind::Person, prompt), {}}})) {
            std::fprintf(stderr, "link_walk: the store did not take the take\n");
            std::exit(1);
        }
        pushState();
        records_.push_back(record("[op |-> \"take\", t |-> " + std::to_string(id)));
    }

    // The server restarts over its store: a new hub, with no open entries; each lost one's turn is cut.
    void restart() {
        server_.hub = std::make_unique<Hub>(server_.store, server_.limits, &server_.responder);
        for (SessionId id = 1; id <= kMaxSessions; ++id) {
            Open& o = server_.open[id];
            if (o.g != 0) {
                server_.hub->commit(id, {SessionChange{stored(EntryKind::Notice, cutText(o.g)), {}}});
            }
            o = Open{};
        }
        ++server_.restarts;
        pushState();
        records_.push_back(record("[op |-> \"restart\""));
    }

    bool settled() const {
        for (std::size_t c = 0; c < windows_.size(); ++c) {
            if (server_.applier[c].applied() != windows_[c].made.size() || windows_[c].last != server_.states.size()) {
                return false;
            }
        }
        return true;
    }

    // Everything through, as mosh does once the network is good: each sender sends from what was acknowledged, the
    // datagram arrives, the acknowledgement returns.
    void flush(Rng& rng) {
        for (int round = 0; round < 20 && !settled(); ++round) {
            for (std::size_t c = 0; c < windows_.size(); ++c) {
                Window& w = windows_[c];
                w.upFlight.clear();
                upSend(rng, c, true);
                upArrives(rng, c);
                w.up.acknowledge(server_.up[c].latestNum());
            }
            for (std::size_t c = 0; c < windows_.size(); ++c) {
                Window& w = windows_[c];
                w.downFlight.clear();
                downSend(rng, c, true);
                downArrives(rng, c);
                server_.down[c].acknowledge(w.down.latestNum());
            }
        }
    }

    std::size_t windows() const { return windows_.size(); }
    std::size_t states() const { return server_.states.size(); }
    std::size_t sessions() const { return server_.store.size(); }
    std::size_t notices() const {
        std::size_t n = 0;
        for (const Window& w : windows_) {
            for (std::size_t i = 0; i < w.model.tab_count(); ++i) {
                for (const Entry& e : w.model.tab(i).transcript) {
                    n += e.kind == EntryKind::Notice ? 1 : 0;
                }
            }
        }
        return n;
    }
    std::size_t refusedRanges() const { return refused_; }
    std::size_t stops() const { return server_.responder.stops; }
    std::uint64_t grows() const { return server_.grows; }
    std::uint64_t restarts() const { return server_.restarts; }
    const std::vector<std::string>& records() const { return records_; }

private:
    static Limits limits(Fault fault) {
        Limits l;
        l.queue = 1000;
        l.queueBytes = 1 << 20;
        l.sessions = kMaxSessions;
        l.tail.entries = fault == Fault::Tail ? kW + 1 : kW;
        l.tail.bytes = 1 << 20;
        return l;
    }

    static std::string target(const Action& a) {
        return "[kind |-> " + str(kindName(a.kind)) + ", sid |-> " + std::to_string(a.session) + ", AI |-> " +
            str(a.aiName) + "]";
    }

    // A change is a new state for every window's sender.
    void pushState() {
        if (!(server_.hub->state() == server_.states.back())) {
            server_.states.push_back(server_.hub->state());
            for (FakeSender<Sessions>& d : server_.down) {
                d.current = server_.hub->state();
            }
        }
    }

    // The server's Applier takes a range; the hub applies what it newly applies.
    void take(std::size_t c, const std::vector<Action>& range) {
        if (range.empty()) {
            return;
        }
        std::vector<Action> applied;
        if (fault_ == Fault::Applier) {
            if (range.front().num <= server_.applier[c].applied() + 1) {
                applied = range;
            }
            server_.applier[c].apply(range);
        } else {
            std::string why;
            applied = server_.applier[c].apply(range, &why);
            refused_ += why.empty() ? 0 : 1;
        }
        for (Action a : applied) {
            const std::size_t before = server_.responder.stops;
            if (fault_ == Fault::Ensure && a.kind == ActionKind::Ensure) {
                a.kind = ActionKind::Create;
            }
            server_.hub->apply(a);
            if (a.kind == ActionKind::Stop) {
                server_.stops[a.session].push_back("[a |-> <<" + std::to_string(c + 1) + ", " +
                    std::to_string(a.num) + ">>, ok |-> " + (server_.responder.stops > before ? "TRUE" : "FALSE") +
                    "]");
            }
        }
        pushState();
        records_.push_back(record("[op |-> \"deliver\", c |-> " + std::to_string(c + 1) + ", first |-> " +
            std::to_string(range.front().num) + ", lst |-> " + std::to_string(range.back().num)));
    }

    // Per id 1..kMaxSessions, as the spec's functions on Ids: a tuple.
    template <typename F>
    void perId(std::ostringstream& o, F each) const {
        o << "<<";
        for (SessionId id = 1; id <= kMaxSessions; ++id) {
            o << (id > 1 ? ", " : "");
            each(id, server_.store.find(id));
        }
        o << ">>";
    }

    // Per window, a record per AI.
    template <typename F>
    void perTab(std::ostringstream& o, F each) const {
        o << "<<";
        for (std::size_t c = 0; c < windows_.size(); ++c) {
            o << (c ? ", " : "") << "[";
            for (std::size_t t = 0; t < kAIs.size(); ++t) {
                o << (t ? ", " : "") << kAIs[t] << " |-> ";
                each(windows_[c], windows_[c].model.tab(t));
            }
            o << "]";
        }
        o << ">>";
    }

    // The record: the step, then the code's whole state.
    std::string record(const std::string& head) const {
        std::ostringstream o;
        o << head;
        o << ", acts |-> <<";
        for (std::size_t c = 0; c < windows_.size(); ++c) {
            o << (c ? ", " : "") << "<<";
            for (std::size_t k = 0; k < windows_[c].made.size(); ++k) {
                o << (k ? ", " : "") << target(windows_[c].made[k]);
            }
            o << ">>";
        }
        o << ">>, applied |-> <<";
        for (std::size_t c = 0; c < windows_.size(); ++c) {
            o << (c ? ", " : "") << server_.applier[c].applied();
        }
        o << ">>, ses |-> ";
        perId(o, [&](SessionId, const StoredSession* s) {
            o << "[ai |-> " << str(s == nullptr ? std::string() : s->info.aiName) << "]";
        });
        o << ", nextSid |-> " << server_.store.nextId() << ", tr |-> ";
        perId(o, [&](SessionId, const StoredSession* s) {
            o << "<<";
            for (std::size_t k = 0; s != nullptr && k < s->transcript.size(); ++k) {
                o << (k ? ", " : "") << entry(s->transcript[k]->kind, s->transcript[k]->text);
            }
            o << ">>";
        });
        o << ", q |-> ";
        perId(o, [&](SessionId, const StoredSession* s) {
            o << "<<";
            for (std::size_t k = 0; s != nullptr && k < s->queue.size(); ++k) {
                o << (k ? ", " : "") << tuple(s->queue[k]);
            }
            o << ">>";
        });
        // The open entries as the hub shows them: the newest entry of each session's state, when it is open.
        o << ", open |-> ";
        perId(o, [&](SessionId id, const StoredSession*) {
            const SessionView* v = server_.hub->state().find(id);
            const bool open = v != nullptr && !v->entries.empty() && !v->entries.back()->closed;
            const std::string text = open ? v->entries.back()->text : std::string();
            o << "[g |-> " << (open ? text.substr(1, text.find(':') - 1) : std::string("0")) << ", len |-> "
              << (open ? text.size() - text.find(':') - 1 : 0) << "]";
        });
        o << ", stops |-> ";
        perId(o, [&](SessionId id, const StoredSession*) {
            o << "<<";
            auto it = server_.stops.find(id);
            for (std::size_t k = 0; it != server_.stops.end() && k < it->second.size(); ++k) {
                o << (k ? ", " : "") << it->second[k];
            }
            o << ">>";
        });
        o << ", made |-> [g |-> " << server_.made << ", grows |-> " << server_.grows << ", restarts |-> "
          << server_.restarts << "]";
        o << ", states |-> " << server_.states.size() << ", last |-> <<";
        for (std::size_t c = 0; c < windows_.size(); ++c) {
            o << (c ? ", " : "") << windows_[c].last;
        }
        o << ">>, shown |-> ";
        perTab(o, [&](const Window& w, const Tab& tab) { o << w.mirror->session(tab.id); });
        o << ", view |-> ";
        perTab(o, [&](const Window&, const Tab& tab) {
            o << "<<";
            for (std::size_t k = 0; k < tab.transcript.size(); ++k) {
                o << (k ? ", " : "") << entry(tab.transcript[k].kind, tab.transcript[k].text);
            }
            o << ">>";
        });
        o << ", vq |-> ";
        perTab(o, [&](const Window&, const Tab& tab) {
            o << "<<";
            for (std::size_t k = 0; k < tab.queue.size(); ++k) {
                o << (k ? ", " : "") << tuple(tab.queue[k]);
            }
            o << ">>";
        });
        o << ", seen |-> ";
        perTab(o, [&](const Window& w, const Tab& tab) { o << "[next |-> " << w.mirror->seen(tab.id).next << "]"; });
        o << ", live |-> ";
        perTab(o, [&](const Window& w, const Tab& tab) {
            const SessionEntry* e = w.mirror->open(tab.id);
            if (e == nullptr) {
                o << "[k |-> \"none\", text |-> <<>>]";
            } else {
                o << "[k |-> \"O\", text |-> " << grownPair(e->text) << "]";
            }
        });
        o << "]";
        return o.str();
    }

    Fault fault_;
    Server server_;
    std::vector<Window> windows_;
    std::vector<std::string> records_;
    std::size_t refused_ = 0;
};

int walk(std::uint64_t seed, std::uint64_t steps, std::size_t windows, Fault fault, const char* path) {
    Rng rng{seed};
    Walk w(windows, fault);
    for (std::uint64_t s = 0; s < steps; ++s) {
        const std::size_t c = rng.below(windows);
        switch (rng.below(16)) {
            case 0:
            case 1: w.act(rng, c); break;
            case 2:
            case 3: w.upSend(rng, c); break;
            case 4:
            case 5: w.upArrives(rng, c); break;
            case 6: w.upAck(rng, c); break;
            case 7: w.stray(rng, c); break;
            case 8: w.downSend(rng, c); break;
            case 9:
            case 10: w.downArrives(rng, c); break;
            case 11: w.downAck(rng, c); break;
            case 12:
            case 13: w.grow(rng); break;
            case 14: w.close(rng); break;
            default:
                if (rng.chance(8)) {
                    w.restart();
                } else {
                    w.takeQueued(rng);
                }
                break;
        }
    }
    w.flush(rng);
    if (!w.settled() && fault == Fault::None) {
        std::fprintf(stderr, "link_walk: the walk did not settle\n");
        return 1;
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "link_walk: cannot write %s\n", path);
        return 1;
    }
    const char* faults[] = {"", ", faulty applier", ", faulty mirror", ", faulty tail", ", faulty close",
                            ", faulty ensure"};
    out << "---- MODULE LinkTraceData ----\n";
    out << "\\* Written by tests/sync/link_walk: seed " << seed << ", " << steps << " steps, " << windows << " windows"
        << faults[static_cast<int>(fault)] << ".\n";
    out << "EXTENDS Integers\n";
    out << "TraceClients == 1.." << windows << "\n";
    out << "TraceAIs == {\"a\", \"b\"}\n";
    out << "TraceStart == <<\"a\">>\n";
    out << "TraceMaxSessions == " << kMaxSessions << "\n";
    out << "TraceMaxActions == " << steps << "\n";
    out << "TraceW == " << kW << "\n";
    out << "TraceMaxGrows == " << steps << "\n";
    out << "TraceMaxRestarts == " << steps << "\n";
    out << "Trace == <<\n";
    const auto& records = w.records();
    for (std::size_t k = 0; k < records.size(); ++k) {
        out << "  " << records[k] << (k + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "link_walk: writing %s failed\n", path);
        return 1;
    }
    std::printf("link_walk: seed %llu, %llu steps, %zu windows%s: %zu records, %zu server states, %zu sessions, "
                "%zu notices, %zu ranges refused, %zu stops, %llu grows, %llu restarts -> %s\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(steps), windows,
                faults[static_cast<int>(fault)], records.size(), w.states(), w.sessions(), w.notices(),
                w.refusedRanges(), w.stops(), static_cast<unsigned long long>(w.grows()),
                static_cast<unsigned long long>(w.restarts()), path);
    return 0;
}

bool parse(const char* s, std::uint64_t& v) {
    char* end = nullptr;
    v = std::strtoull(s, &end, 10);
    return end != s && *end == '\0';
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> rest;
    Fault fault = Fault::None;
    int faults = 0;
    const std::map<std::string, Fault> named = {{"--faulty-applier", Fault::Applier},
        {"--faulty-mirror", Fault::Mirror}, {"--faulty-tail", Fault::Tail}, {"--faulty-close", Fault::Close},
        {"--faulty-ensure", Fault::Ensure}};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto it = named.find(a);
        if (it != named.end()) {
            fault = it->second;
            ++faults;
        } else {
            rest.push_back(a);
        }
    }
    std::uint64_t seed = 0, steps = 0, windows = 0;
    if (rest.size() != 4 || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) ||
            !parse(rest[2].c_str(), windows) || windows < 1 || windows > 4 || faults > 1) {
        std::fprintf(stderr, "usage: link_walk <seed> <steps> <windows 1..4> [--faulty-applier | --faulty-mirror | "
                     "--faulty-tail | --faulty-close | --faulty-ensure] <out.tla>\n");
        return 2;
    }
    return walk(seed, steps, static_cast<std::size_t>(windows), fault, rest[3].c_str());
}
