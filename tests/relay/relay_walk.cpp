// Drives the window's side of the relays' life (src/relay/relay_life.h: RelayLife, as src/relay/relay_port_win.cpp
// drives it) with a seeded random walk and writes the trace as a TLA+ module (RelayLifeTraceData.tla) for
// models/RelayLifeTrace.tla, under which TLC checks that the trace is a behaviour of models/RelayLife.tla.
//
//   relay_walk <seed> <steps> [--faulty-resend | --faulty-early-end | --faulty-ack-delivered] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64) and its clock is
// made up, never the system's. It plays the port and everything around RelayLife, as the port does: the window's
// actions (send, numbered by RelayLife, each with its number as its text, so that the world knows an action by its
// bytes and not by the code's bookkeeping) and polls; relays started when mayStart says so, and only once the last
// one's process exited; the bytes RelayLife hands over (take) read by a real RelayServer per relay; each relay's Hello
// back, and its link's state as real StateWriter frames carrying the server's first unapplied action of that relay's
// link; the server applying what each relay's link delivered, per link in order, a relay that ended included (what
// was in flight); relays that end, by a crash or by bytes that are not the protocol (refused by RelayLife, then ended
// by the port); the clock, by whole seconds while RelayLife waits; and at the end, sometimes, the window's exit.
//
// --faulty-resend writes again to the next relay the actions a relay that ended cut (a port that "loses nothing");
// --faulty-early-end tells RelayLife a relay ended when its output closes, before its process exits; and
// --faulty-ack-delivered has the relays report as applied what they delivered. TLC must reject all three.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "relay/relay_ends.h"
#include "relay/relay_life.h"

namespace {

using namespace nexus;
using namespace nexus::relay;

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

// The trace checks no state space, so more relays come and go and more actions cross than in MCRelayLife.cfg.
constexpr std::uint64_t kMaxRelays = 24;
constexpr std::uint64_t kMaxActions = 48;

// One relay, as the world sees it: its process, its end of the pipe, and what its link delivered and the server
// applied.
struct Relay {
    std::uint64_t gen = 0;
    bool process = true;        // its process runs
    bool hello = false;         // it wrote its Hello to the window
    RelayServer end;
    std::vector<std::uint64_t> delivered;   // the window's numbers, in its link's order
    std::size_t applied = 0;                // the server applied delivered[0 .. applied)
};

struct World {
    std::vector<std::unique_ptr<Relay>> relays;
    Relay* live = nullptr;                  // the relay RelayLife holds live
    std::set<std::uint64_t> told;
    std::vector<std::set<std::uint64_t>> wr = std::vector<std::set<std::uint64_t>>(kMaxActions + 1);
    std::vector<std::set<std::uint64_t>> ap = std::vector<std::set<std::uint64_t>>(kMaxActions + 1);
    LifeMs now = 1000 * 1000;
    int procs() const {
        int n = 0;
        for (const auto& r : relays) n += r->process ? 1 : 0;
        return n;
    }
};

const char* phaseName(RelayLife::Phase p) {
    switch (p) {
    case RelayLife::Phase::Waiting: return "wait";
    case RelayLife::Phase::Hello: return "hello";
    case RelayLife::Phase::Running: return "running";
    case RelayLife::Phase::Stopped: return "stopped";
    }
    return "?";
}

template <class C>
std::string set(const C& items) {
    std::ostringstream o;
    o << "{";
    bool first = true;
    for (const auto n : items) {
        o << (first ? "" : ", ") << n;
        first = false;
    }
    o << "}";
    return o.str();
}

std::string sets(const std::vector<std::set<std::uint64_t>>& all) {
    std::ostringstream o;
    o << "<<";
    for (std::uint64_t n = 1; n <= kMaxActions; ++n) o << (n > 1 ? ", " : "") << set(all[n]);
    o << ">>";
    return o.str();
}

std::string record(const World& w, const RelayLife& life) {
    std::ostringstream o;
    o << "[phase |-> \"" << phaseName(life.phase()) << "\", procs |-> " << w.procs() << ", gen |-> " << life.relays()
      << ", left |-> " << life.leftMs(w.now) / 1000 << ", backoff |-> " << life.backoffMs() / 1000 << ", sent |-> "
      << life.sent() << ", written |-> " << life.written() << ", base |-> " << life.base() << ", ackHigh |-> "
      << life.acked() << ", cut |-> " << set(std::set<std::uint64_t>(life.cut().begin(), life.cut().end()))
      << ", told |-> " << set(w.told) << ", wr |-> " << sets(w.wr) << ", ap |-> " << sets(w.ap) << "]";
    return o.str();
}

sync::Action numbered(std::uint64_t n) {
    sync::Action a;
    a.kind = sync::ActionKind::Send;
    a.session = 1;
    a.text = "action " + std::to_string(n);
    return a;
}

std::uint64_t numberOf(const sync::Action& a) {
    return std::strtoull(a.text.c_str() + std::strlen("action "), nullptr, 10);
}

// The relay's state frame: its link's state, with the server's first unapplied action of its link.
std::string stateOf(Relay* r, std::uint64_t next) {
    LinkState s;
    s.phase = sync::LinkPhase::Connected;
    auto v = std::make_shared<sync::SessionView>();
    v->id = 1;
    v->info.aiName = "ada";
    v->info.harness = "claude";
    s.sessions = sync::Sessions({v});
    s.sessions.setFormat(sync::kStateFormat);
    s.sessions.setNextAction(next);
    return r->end.state(s);
}

} // namespace

int main(int argc, char** argv) {
    bool faultyResend = false, faultyEarlyEnd = false, faultyAckDelivered = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-resend") == 0) {
            faultyResend = true;
        } else if (std::strcmp(argv[i], "--faulty-early-end") == 0) {
            faultyEarlyEnd = true;
        } else if (std::strcmp(argv[i], "--faulty-ack-delivered") == 0) {
            faultyAckDelivered = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: relay_walk <seed> <steps> [--faulty-resend | --faulty-early-end | "
            "--faulty-ack-delivered] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    if (steps < 1) {
        std::fprintf(stderr, "relay_walk: steps must be at least 1\n");
        return 2;
    }
    Rng rng{seed};
    World w;
    RelayLife life;
    std::vector<std::string> trace{record(w, life)};
    int crashes = 0, refused = 0, starts = 0, applied = 0, acks = 0, polls = 0;
    bool stopped = false;
    // The relay whose output closed while its process runs on (--faulty-early-end).
    Relay* lingering = nullptr;
    for (long k = 0; k < steps; ++k) {
        // Near the end, sometimes, the window exits.
        if (!stopped && k > steps - steps / 10 && rng.below(steps / 10 + 1) == 0) {
            life.stop();
            stopped = true;
            trace.push_back(record(w, life));
            continue;
        }
        switch (rng.below(12)) {
            case 0: {   // the window hands an action over
                if (!stopped && life.sent() < kMaxActions) {
                    const std::uint64_t n = life.sent() + 1;
                    if (life.send(numbered(n)) != n) {
                        std::fprintf(stderr, "relay_walk: send numbered other than %llu\n",
                            static_cast<unsigned long long>(n));
                        return 1;
                    }
                }
                break;
            }
            case 1: {   // the port starts a relay: RelayLife says when, and none while one's process runs
                if (stopped || !life.mayStart(w.now) || life.relays() >= kMaxRelays) break;
                if (w.procs() > 0 && !faultyEarlyEnd) break;
                if (!life.started(w.now)) {
                    std::fprintf(stderr, "relay_walk: started refused although mayStart\n");
                    return 1;
                }
                auto r = std::make_unique<Relay>();
                r->gen = life.relays();
                w.live = r.get();
                w.relays.push_back(std::move(r));
                ++starts;
                break;
            }
            case 2:
            case 3: {   // the port's writer: the next bytes RelayLife hands it, read by the live relay
                std::string bytes, why;
                if (w.live == nullptr || !life.take(&bytes)) break;
                if (!w.live->end.feed(bytes.data(), bytes.size(), &why)) {
                    std::fprintf(stderr, "relay_walk: the relay refused the window's bytes: %s\n", why.c_str());
                    return 1;
                }
                for (const RelayServer::Request& r : w.live->end.take()) {
                    if (r.kind != RelayServer::Request::Kind::Action) continue;
                    const std::uint64_t n = numberOf(r.action);
                    w.wr[n].insert(w.live->gen);
                    w.live->delivered.push_back(n);
                }
                if (rng.below(8) == 0) life.history(1, 0, 40);
                break;
            }
            case 4: {   // the live relay's Hello reaches the window
                if (w.live == nullptr || w.live->hello) break;
                const std::string hello = w.live->end.open();
                std::string why;
                if (!life.feed(hello.data(), hello.size(), &why)) {
                    std::fprintf(stderr, "relay_walk: the window refused the relay's hello: %s\n", why.c_str());
                    return 1;
                }
                w.live->hello = true;
                break;
            }
            case 5:
            case 6: {   // the server applies the next action some relay's link delivered (one ended: in flight)
                std::vector<Relay*> ready;
                for (const auto& r : w.relays)
                    if (r->applied < r->delivered.size()) ready.push_back(r.get());
                if (ready.empty()) break;
                Relay* r = ready[rng.below(ready.size())];
                w.ap[r->delivered[r->applied]].insert(r->gen);
                ++r->applied;
                ++applied;
                break;
            }
            case 7: {   // the live relay's link's state reaches the window
                if (w.live == nullptr || !w.live->hello) break;
                const std::size_t done = faultyAckDelivered ? w.live->delivered.size() : w.live->applied;
                const std::string up = stateOf(w.live, done + 1);
                std::string why;
                if (!life.feed(up.data(), up.size(), &why)) {
                    std::fprintf(stderr, "relay_walk: the window refused the relay's state: %s\n", why.c_str());
                    return 1;
                }
                ++acks;
                break;
            }
            case 8: {   // the live relay ends: a crash, or bytes that are not the protocol (refused, then ended)
                if (w.live == nullptr || rng.below(5) != 0) break;
                Relay* r = w.live;
                std::string why = "crashed";
                if (r->hello && rng.below(3) == 0) {
                    const std::string junk = std::string(1, '\x09') + "junk";
                    if (life.feed(junk.data(), junk.size(), &why)) {
                        std::fprintf(stderr, "relay_walk: the window took bytes that are not the protocol\n");
                        return 1;
                    }
                    trace.push_back(record(w, life));
                    ++refused;
                } else {
                    ++crashes;
                }
                if (faultyEarlyEnd) {
                    lingering = r;      // its output closed; its process exits later
                } else {
                    r->process = false;
                }
                w.live = nullptr;
                const std::vector<std::uint64_t> before = life.cut();
                life.ended(why, w.now);
                if (faultyResend) {
                    // Each handed over again as a new action, one step each: the next relay is written the old
                    // action under a new number.
                    trace.push_back(record(w, life));
                    for (std::size_t i = before.size(); i < life.cut().size(); ++i) {
                        life.send(numbered(life.cut()[i]));
                        if (i + 1 < life.cut().size()) trace.push_back(record(w, life));
                    }
                }
                break;
            }
            case 9: {   // the window's poll: the actions cut since the last
                if (stopped) break;
                LinkState s;
                if (life.poll(&s)) {
                    w.told.insert(s.cut.begin(), s.cut.end());
                    ++polls;
                }
                break;
            }
            default: {  // the clock: by whole seconds while RelayLife waits, by anything else otherwise
                if (lingering != nullptr && rng.below(2) == 0) {
                    lingering->process = false;
                    lingering = nullptr;
                }
                if (life.phase() == RelayLife::Phase::Waiting) {
                    w.now += 1000;
                } else {
                    const LifeMs moves[] = {50, 1000, 4000, kRanWellMs};
                    w.now += moves[rng.below(4)];
                }
                break;
            }
        }
        trace.push_back(record(w, life));
        // After the window's exit, its relay ends by itself (its input closed).
        if (stopped && w.live != nullptr && rng.below(4) == 0) {
            w.live->process = false;
            w.live = nullptr;
            life.ended("stopped", w.now);
            trace.push_back(record(w, life));
        }
    }
    std::ofstream out(args[2]);
    if (!out) {
        std::fprintf(stderr, "relay_walk: cannot write %s\n", args[2]);
        return 2;
    }
    out << "---- MODULE RelayLifeTraceData ----\n";
    out << "\\* Written by tests/relay/relay_walk: seed " << seed << ", " << steps << " steps"
        << (faultyResend ? ", faulty-resend" : "") << (faultyEarlyEnd ? ", faulty-early-end" : "")
        << (faultyAckDelivered ? ", faulty-ack-delivered" : "") << ".\n";
    out << "EXTENDS TLC\n";
    out << "TraceMaxRelays == " << kMaxRelays << "\nTraceMaxActions == " << kMaxActions << "\nTrace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    std::printf("relay_walk: %zu records, %d relays started, %d crashed, %d refused, %llu actions, %d applied, "
        "%d states, %zu cut, %d polls%s\n", trace.size(), starts, crashes, refused,
        static_cast<unsigned long long>(life.sent()), applied, acks, life.cut().size(), polls,
        stopped ? ", the window exited" : "");
    return 0;
}
