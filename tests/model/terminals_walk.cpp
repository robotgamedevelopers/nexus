// Drives the real code of the server's terminals with a seeded random walk and writes ServerTerminalsTraceData.tla for
// models/ServerTerminalsTrace.tla, which checks that the trace is a behaviour of models/ServerTerminals.tla:
//
//   - the server's decisions are TerminalBook's (src/server/terminal_book.h), one book per run of the server, each with
//     an epoch of its own, and the shell's end is TerminalBook::ended;
//   - each window's actions reach the book through sync::Applier (src/sync/actions.h), made on every link from the
//     store's durable counter, given every action not yet acknowledged, as mosh carries them;
//   - each window's tab follows the state by sync::terminalFate (src/sync/terminal_state.h), as remote_terminals.cpp
//     does.
//
// The windows' choices (what to send, when to link, when to dismiss a lost tab) are the walk's, under the model's
// guards. A terminal's slot is the low half of its id; the input's bytes name its window and key.
//
//   terminals_walk <seed> <steps> [--faulty-replay | --faulty-fate | --faulty-epoch] <out.tla>
//
// --faulty-replay makes each link's Applier from 0, the store's counter forgotten; --faulty-fate asks terminalFate with
// the server's epoch for the tab's (as if ids held none), so a terminal lost to a restart reads as closed; --faulty-epoch
// gives every run of the server the same epoch, so input typed for a lost terminal reaches the next one in its slot.
// TLC must reject all three.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "server/terminal_book.h"
#include "sync/actions.h"
#include "sync/terminal_state.h"

namespace {

using namespace nexus;
using server::TerminalBook;
using server::TerminalEffect;
using sync::Action;
using sync::ActionKind;
using sync::TerminalId;

constexpr int kClients = 2;
constexpr std::uint32_t kSlots = 4;         // creates per run at most: the trace's slots are 1..kSlots
constexpr int kRestarts = 3;
constexpr std::size_t kSends = 80;          // actions per window at most

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return std::size_t(next() % n); }
};

enum class Fault { None, Replay, Fate, Epoch };

// The model's record of an action: [kind, slot, ep, key, size].
struct Act {
    std::string kind;
    std::uint32_t slot = 0, ep = 0, key = 0, size = 0;
    std::string tla() const {
        std::ostringstream o;
        o << "[kind |-> \"" << kind << "\", slot |-> " << slot << ", ep |-> " << ep << ", key |-> " << key
          << ", size |-> " << size << "]";
        return o.str();
    }
};

struct Tab {
    std::string seen = "absent";
    std::uint32_t run = 0;              // the run (model epoch) of the terminal it shows
    std::uint32_t codeEpoch = 0;        // that run's epoch, as the id the tab holds carries it
};

struct Delivered {
    int client;
    std::uint32_t key, ep;
};

struct Window {
    std::vector<Action> outbox;
    std::vector<Act> acts;
    std::uint64_t store = 0;            // the store's counter: the number of its last action applied
    std::unique_ptr<sync::Applier> applier;
    bool linked = true;
    std::uint32_t keys = 0;
    Tab tabs[kSlots + 1];
};

class Walk {
public:
    Walk(Fault fault) : fault_(fault) {
        startRun();
        for (Window& w : windows_) w.applier = std::make_unique<sync::Applier>(0);
    }

    bool step(Rng& rng) {
        // Weighted: the windows mostly send, apply and sync; links drop, shells end and the server restarts now and then.
        static constexpr char kOps[] = "ccciiiiiirrkkaaaaaaaaaeddlllRxssssss";
        for (int tries = 0; tries < 64; ++tries) {
            const int c = int(rng.below(kClients));
            const std::uint32_t x = std::uint32_t(rng.below(kSlots)) + 1;
            Window& w = windows_[c];
            const bool room = w.linked && w.outbox.size() < kSends;
            switch (kOps[rng.below(sizeof kOps - 1)]) {
                case 'c':
                    // Creates in flight and made in this run stay within the trace's slots, 1..kSlots.
                    if (room && created_ + pendingCreates() < kSlots) return send(c, create());
                    break;
                case 'i':
                    if (room && w.tabs[x].seen == "running") return send(c, input(c, x));
                    break;
                case 'r':
                    if (room && live(w.tabs[x])) return send(c, resize(w, x, rng));
                    break;
                case 'k':
                    if (room && live(w.tabs[x])) return send(c, close(w, x));
                    break;
                case 'a':
                    if (w.linked && w.store < w.outbox.size()) return apply(c);
                    break;
                case 'e':
                    if (exitOne(rng)) return true;
                    break;
                case 'd':
                    // Rarer than the rest, which often find nothing to do.
                    if (w.linked && rng.below(10) == 0) return detach(c);
                    break;
                case 'l':
                    if (!w.linked) return attach(c);
                    break;
                case 'R':
                    if (runs_ < kRestarts && rng.below(10) == 0) return restart();
                    break;
                case 'x':
                    if (w.tabs[x].seen == "restarted") return dismiss(c, x);
                    break;
                default:
                    if (w.linked && sync(c)) return true;
                    break;
            }
        }
        return false;
    }

    std::uint32_t pendingCreates() const {
        std::uint32_t n = 0;
        for (const Window& w : windows_) {
            for (std::size_t i = w.store; i < w.outbox.size(); ++i) n += w.outbox[i].kind == ActionKind::TerminalCreate;
        }
        return n;
    }

    std::string data() const {
        std::ostringstream o;
        o << "---------------------- MODULE ServerTerminalsTraceData ----------------------\n"
          << "\\* Written by tests/model/terminals_walk; do not edit.\n"
          << "EXTENDS Integers, Sequences\n\n"
          << "TraceClients == 1.." << kClients << "\n"
          << "TraceSlots == 1.." << kSlots << "\n"
          << "TraceMaxSends == " << kSends << "\n"
          << "TraceRestarts == " << runs_ << "\n\n"
          << "Trace == <<\n";
        for (std::size_t i = 0; i < steps_.size(); ++i) {
            o << "    " << steps_[i] << (i + 1 < steps_.size() ? ",\n" : "\n");
        }
        o << ">>\n\n=============================================================================\n";
        return o.str();
    }

    void record(const std::string& op, int c = 0, std::uint32_t x = 0, std::uint32_t spawned = 0) {
        std::ostringstream o;
        o << "[op |-> \"" << op << "\", c |-> " << (c + 1) << ", x |-> " << x << ", spawned |-> " << spawned
          << ", epoch |-> " << runs_;
        o << ", act |-> ";
        const Window& cw = windows_[c];
        if (!cw.acts.empty()) o << cw.acts.back().tla();
        else o << "[kind |-> \"none\", slot |-> 0, ep |-> 0, key |-> 0, size |-> 0]";
        o << ", live |-> <<";
        for (std::uint32_t s = 1; s <= kSlots; ++s) {
            const TerminalBook::Entry* e = book_->find(id(epochOf(runs_), s));
            o << (s > 1 ? ", " : "") << "\"" << (e == nullptr ? "none" : e->state == TerminalBook::State::Running ?
                "running" : "ended") << "\"";
        }
        o << ">>, size |-> <<";
        for (std::uint32_t s = 1; s <= kSlots; ++s) {
            const TerminalBook::Entry* e = book_->find(id(epochOf(runs_), s));
            o << (s > 1 ? ", " : "") << (e == nullptr ? 0 : e->columns);
        }
        o << ">>, applied |-> <<";
        for (int i = 0; i < kClients; ++i) {
            const Window& w = windows_[i];
            o << (i ? ", " : "") << (w.applier ? w.applier->applied() : w.store);
        }
        o << ">>, linked |-> <<";
        for (int i = 0; i < kClients; ++i) o << (i ? ", " : "") << (windows_[i].linked ? "TRUE" : "FALSE");
        o << ">>, sent |-> <<";
        for (int i = 0; i < kClients; ++i) o << (i ? ", " : "") << windows_[i].outbox.size();
        o << ">>, delivered |-> <<";
        for (std::uint32_t s = 1; s <= kSlots; ++s) {
            o << (s > 1 ? ", " : "") << "<<";
            for (std::size_t k = 0; k < delivered_[s].size(); ++k) {
                const Delivered& d = delivered_[s][k];
                o << (k ? ", " : "") << "[c |-> " << (d.client + 1) << ", key |-> " << d.key << ", ep |-> " << d.ep
                  << "]";
            }
            o << ">>";
        }
        o << ">>, seen |-> <<";
        for (int i = 0; i < kClients; ++i) {
            o << (i ? ", " : "") << "<<";
            for (std::uint32_t s = 1; s <= kSlots; ++s) o << (s > 1 ? ", " : "") << "\"" << windows_[i].tabs[s].seen << "\"";
            o << ">>";
        }
        o << ">>, seenEp |-> <<";
        for (int i = 0; i < kClients; ++i) {
            o << (i ? ", " : "") << "<<";
            for (std::uint32_t s = 1; s <= kSlots; ++s) o << (s > 1 ? ", " : "") << windows_[i].tabs[s].run;
            o << ">>";
        }
        o << ">>]";
        steps_.push_back(o.str());
    }

private:
    static TerminalId id(std::uint32_t epoch, std::uint32_t slot) { return (TerminalId(epoch) << 32) | slot; }
    static bool live(const Tab& t) { return t.seen == "running" || t.seen == "ended"; }
    std::uint32_t epochOf(std::uint32_t run) const { return fault_ == Fault::Epoch ? 1 : run + 1; }

    void startRun() {
        book_ = std::make_unique<TerminalBook>(epochOf(runs_));
        created_ = 0;
    }

    std::pair<Action, Act> create() {
        Action a;
        a.kind = ActionKind::TerminalCreate;
        a.text = "w";
        a.columns = 1;
        a.rows = 1;
        return {a, Act{"create"}};
    }

    std::pair<Action, Act> input(int c, std::uint32_t x) {
        Window& w = windows_[c];
        const std::uint32_t key = ++w.keys;
        Action a;
        a.kind = ActionKind::TerminalInput;
        a.terminal = id(w.tabs[x].codeEpoch, x);
        a.secret = sync::SecretValue(std::to_string(c) + ":" + std::to_string(key));
        return {a, Act{"input", x, w.tabs[x].run, key, 0}};
    }

    std::pair<Action, Act> resize(Window& w, std::uint32_t x, Rng& rng) {
        const std::uint32_t z = std::uint32_t(rng.below(2)) + 1;
        Action a;
        a.kind = ActionKind::TerminalResize;
        a.terminal = id(w.tabs[x].codeEpoch, x);
        a.columns = z;
        a.rows = z;
        return {a, Act{"resize", x, w.tabs[x].run, 0, z}};
    }

    std::pair<Action, Act> close(Window& w, std::uint32_t x) {
        Action a;
        a.kind = ActionKind::TerminalClose;
        a.terminal = id(w.tabs[x].codeEpoch, x);
        return {a, Act{"close", x, w.tabs[x].run, 0, 0}};
    }

    bool send(int c, std::pair<Action, Act> sent) {
        Window& w = windows_[c];
        sent.first.num = w.outbox.size() + 1;
        w.outbox.push_back(sent.first);
        w.acts.push_back(sent.second);
        record(sent.second.kind, c, sent.second.slot);
        return true;
    }

    // Mosh carries every action not yet acknowledged; the Applier takes the next one, and no other.
    bool apply(int c) {
        Window& w = windows_[c];
        const std::vector<Action> carried(w.outbox.begin(), w.outbox.begin() + std::ptrdiff_t(w.store + 1));
        const std::vector<Action> fresh = w.applier->apply(carried);
        w.store = w.applier->applied();
        std::uint32_t spawned = 0;
        for (const Action& a : fresh) {
            const server::TerminalOutcome out = book_->apply(a, true);
            const TerminalEffect& e = out.effect;
            const std::uint32_t slot = std::uint32_t(e.id & 0xffffffffu);
            if (slot > kSlots) {
                // Only a faulty walk gets here (an action applied twice made more terminals than the trace has slots):
                // the model has no such slot, and the step's state already differs from the model's.
                continue;
            }
            if (e.kind == TerminalEffect::Kind::Spawn) {
                ++created_;
                spawned = slot;
                delivered_[slot].clear();
            } else if (e.kind == TerminalEffect::Kind::Write) {
                const std::string& bytes = a.secret.bytes();
                const std::size_t colon = bytes.find(':');
                delivered_[slot].push_back(Delivered{std::atoi(bytes.substr(0, colon).c_str()),
                    std::uint32_t(std::strtoul(bytes.c_str() + colon + 1, nullptr, 10)),
                    std::uint32_t(a.terminal >> 32) - 1});
            }
        }
        record("apply", c, 0, spawned);
        return true;
    }

    bool exitOne(Rng& rng) {
        std::vector<TerminalId> running;
        for (const auto& [tid, e] : book_->terminals()) {
            if (e.state == TerminalBook::State::Running) running.push_back(tid);
        }
        if (running.empty()) return false;
        const TerminalId tid = running[rng.below(running.size())];
        book_->ended(tid, server::endedLine(false, 0));
        record("exit", 0, std::uint32_t(tid & 0xffffffffu));
        return true;
    }

    bool detach(int c) {
        Window& w = windows_[c];
        w.store = w.applier->applied();
        w.applier.reset();
        w.linked = false;
        record("detach", c);
        return true;
    }

    bool attach(int c) {
        Window& w = windows_[c];
        w.applier = std::make_unique<sync::Applier>(fault_ == Fault::Replay ? 0 : w.store);
        w.store = w.applier->applied();
        w.linked = true;
        record("attach", c);
        return true;
    }

    bool restart() {
        ++runs_;
        startRun();
        for (Window& w : windows_) {
            if (w.applier) w.store = w.applier->applied();
            w.applier.reset();
            w.linked = false;
        }
        record("restart");
        return true;
    }

    bool dismiss(int c, std::uint32_t x) {
        windows_[c].tabs[x].seen = "absent";
        record("dismiss", c, x);
        return true;
    }

    // The window takes the state: a tab that holds a terminal asks terminalFate; an empty one takes a terminal the
    // state holds in its slot. Records a step only when a tab changed.
    bool sync(int c) {
        Window& w = windows_[c];
        std::vector<sync::TerminalRef> views;
        for (const auto& [tid, e] : book_->terminals()) {
            auto v = std::make_shared<sync::TerminalView>();
            v->id = tid;
            v->ended = e.ended;
            views.push_back(v);
        }
        const std::uint32_t epoch = book_->epoch();
        bool changed = false;
        for (std::uint32_t x = 1; x <= kSlots; ++x) {
            Tab& t = w.tabs[x];
            const Tab was = t;
            if (live(t)) {
                const std::uint64_t held = fault_ == Fault::Fate ? epoch : t.codeEpoch;
                switch (sync::terminalFate(id(t.codeEpoch, x), held, epoch, views)) {
                    case sync::TerminalFate::Running: t.seen = "running"; break;
                    case sync::TerminalFate::Ended: t.seen = "ended"; break;
                    case sync::TerminalFate::Restarted: t.seen = "restarted"; break;
                    case sync::TerminalFate::Closed: t.seen = "absent"; break;
                }
            } else if (t.seen == "absent") {
                if (const TerminalBook::Entry* e = book_->find(id(epoch, x))) {
                    t.seen = e->state == TerminalBook::State::Running ? "running" : "ended";
                    t.run = runs_;
                    t.codeEpoch = epoch;
                }
            }
            changed = changed || t.seen != was.seen || t.run != was.run;
        }
        if (changed) record("sync", c);
        return changed;
    }

    Fault fault_;
    std::unique_ptr<TerminalBook> book_;
    std::uint32_t runs_ = 0;
    std::uint32_t created_ = 0;
    Window windows_[kClients];
    std::vector<Delivered> delivered_[kSlots + 1];
    std::vector<std::string> steps_;
};

} // namespace

int main(int argc, char** argv) {
    Fault fault = Fault::None;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-replay") == 0) fault = Fault::Replay;
        else if (std::strcmp(argv[i], "--faulty-fate") == 0) fault = Fault::Fate;
        else if (std::strcmp(argv[i], "--faulty-epoch") == 0) fault = Fault::Epoch;
        else args.push_back(argv[i]);
    }
    if (args.size() != 3) {
        std::fprintf(stderr,
            "usage: terminals_walk <seed> <steps> [--faulty-replay | --faulty-fate | --faulty-epoch] <out.tla>\n");
        return 2;
    }
    Rng rng{std::strtoull(args[0], nullptr, 10)};
    const long steps = std::strtol(args[1], nullptr, 10);
    Walk walk(fault);
    walk.record("init");
    long taken = 0;
    for (long i = 0; i < steps; ++i) {
        if (walk.step(rng)) ++taken;
    }
    std::ofstream out(args[2]);
    out << walk.data();
    if (!out) {
        std::fprintf(stderr, "terminals_walk: could not write %s\n", args[2]);
        return 1;
    }
    std::printf("terminals_walk: %ld steps of %ld taken\n", taken, steps);
    return taken > 0 ? 0 : 1;
}
