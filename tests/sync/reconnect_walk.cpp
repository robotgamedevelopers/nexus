// Drives the real reconnect decision (src/sync/redial.h: Redial, as the window's link thread, src/link/client.cpp,
// drives it) with a seeded random walk and writes the trace as a TLA+ module (ReconnectTraceData.tla) for
// models/ReconnectTrace.tla, under which TLC checks that the trace is a behaviour of models/Reconnect.tla.
//
//   reconnect_walk <seed> <steps> [--faulty-cached | --faulty-no-reset] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64) and its clock is
// made up (from 1000 s), never the system's. It plays everything around the code: servers started (the service) and
// gone (a clean stop, whose shutdown reaches the window or is lost, or a crash, which tells nothing), the link's
// passes, which run the bootstrap exactly when Redial::due says so (it reaches the server running, or fails with none),
// the session's server heard, and the clock moving, sometimes past kQuietMs. Each record is the state after one step:
// the server running and how many were started, the server whose session the link holds and whether it heard it (the
// world's), and Redial's quiet, serverEnded and timerDue (the code's), and gen, flipped at each session made.
//
// --faulty-cached makes a session again from the old CONNECT line when no server runs (the old server's port and key);
// --faulty-no-reset has a heard server start a new backoff rather than end it. TLC must reject both; that is the
// negative check that shows the binding can fail.

#include "sync/redial.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sync/client.h"

namespace {

using nexus::sync::Redial;
using nexus::sync::RedialMs;

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

constexpr int kMaxServers = 8;   // the trace checks no state space, so more servers come and go than in MCReconnect.cfg

struct World {
    int srv = 0;        // the server running, 0 for none
    int started = 0;
    int link = 0;       // the server whose session the link holds, 0 for none
    bool heard = false; // the link heard that session's server
    int gen = 0;
    RedialMs now = 1000 * 1000;
};

std::string flag(bool b) {
    return b ? "TRUE" : "FALSE";
}

std::string record(const World& w, const Redial& r) {
    std::ostringstream o;
    o << "[srv |-> " << w.srv << ", started |-> " << w.started << ", link |-> " << w.link << ", heard |-> "
      << flag(w.heard) << ", quiet |-> " << flag(r.quiet(w.now)) << ", ended |-> " << flag(r.serverEnded())
      << ", due |-> " << flag(r.timerDue(w.now)) << ", gen |-> " << w.gen << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    bool faultyCached = false;
    bool faultyNoReset = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-cached") == 0) {
            faultyCached = true;
        } else if (std::strcmp(argv[i], "--faulty-no-reset") == 0) {
            faultyNoReset = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: reconnect_walk <seed> <steps> [--faulty-cached | --faulty-no-reset] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    if (steps < 1) {
        std::fprintf(stderr, "reconnect_walk: steps must be at least 1\n");
        return 2;
    }
    Rng rng{seed};
    World w;
    Redial redial;
    std::vector<std::string> trace{record(w, redial)};
    // The clock's moves: a moment, a backoff's length or so, and past kQuietMs.
    const std::vector<RedialMs> moves = {20, 250, 1000, 2500, nexus::sync::kQuietMs + 500};
    int dials = 0, failed = 0, told = 0, crashes = 0, lives = 0;
    bool wasLive = false;
    for (long k = 0; k < steps; ++k) {
        switch (rng.below(10)) {
            case 0: {   // the service starts a server
                if (w.srv == 0 && w.started < kMaxServers && rng.below(2) == 0) {
                    w.srv = ++w.started;
                }
                break;
            }
            case 1: {   // the server goes: stopped or replaced (its shutdown may reach the window), or crashed
                if (w.srv != 0 && rng.below(3) == 0) {
                    const bool stops = rng.below(2) == 0;
                    if (stops && w.link == w.srv && rng.below(4) != 0) {
                        redial.ended();
                        ++told;
                    }
                    crashes += stops ? 0 : 1;
                    w.srv = 0;
                }
                break;
            }
            case 2:
            case 3:
            case 4: {   // a pass of the link's loop: the bootstrap, when Redial says it is due
                if (!redial.due(w.now)) {
                    break;
                }
                if (w.srv != 0) {
                    w.link = w.srv;
                    w.heard = false;
                    w.gen = 1 - w.gen;
                    redial.dialed(w.now);
                    ++dials;
                } else if (faultyCached && w.link != 0) {
                    w.heard = false;
                    w.gen = 1 - w.gen;
                    redial.dialed(w.now);
                    ++dials;
                } else {
                    redial.failed(w.now);
                    ++failed;
                }
                break;
            }
            case 5:
            case 6: {   // the session's server is heard: its state arrives
                if (w.link != 0 && w.link == w.srv) {
                    if (faultyNoReset) {
                        redial.dialed(w.now);
                    } else {
                        redial.heard(w.now);
                    }
                    w.heard = true;
                }
                break;
            }
            default: {  // the clock
                w.now += moves[rng.below(moves.size())];
                break;
            }
        }
        const bool live = w.link != 0 && w.link == w.srv && w.heard && !redial.lost(w.now);
        lives += live && !wasLive ? 1 : 0;
        wasLive = live;
        trace.push_back(record(w, redial));
    }
    std::ofstream out(args[2]);
    if (!out) {
        std::fprintf(stderr, "reconnect_walk: cannot write %s\n", args[2]);
        return 2;
    }
    out << "---- MODULE ReconnectTraceData ----\n";
    out << "\\* Written by tests/sync/reconnect_walk: seed " << seed << ", " << steps << " steps"
        << (faultyCached ? ", faulty-cached" : "") << (faultyNoReset ? ", faulty-no-reset" : "") << ".\n";
    out << "EXTENDS TLC\n";
    out << "TraceMaxServers == " << kMaxServers << "\nTrace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    std::printf("reconnect_walk: %zu records, %d servers, %d sessions made, %d attempts failed, %d ends told, "
        "%d crashes, live %d times\n", trace.size(), w.started, dials, failed, told, crashes, lives);
    return 0;
}
