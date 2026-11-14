// Drives the real takeover code (src/server/takeover.h: nextStep, the installer's side, and Drain, the running
// server's) with a seeded random walk and writes the trace as a TLA+ module (TakeoverTraceData.tla) for
// models/TakeoverTrace.tla, under which TLC checks that the trace is a behaviour of models/Takeover.tla.
//
//   takeover_walk <seed> <steps> [--faulty-drain | --faulty-stop] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64) and its clock is
// made up (from 1000 s), never the system's. It plays everything around the code: the installer placing builds (each a
// build of made-up bytes, its id from buildOf), the service starting the placed program when none runs, friends'
// turns opening and ending, and the clock moving, sometimes past a drain's deadline. The installer's move each time is
// what nextStep says for what it sees, and the running server's stop is what its Drain says at each pass of its loop.
// Each record is the state after one step: the placed build, the running one (or none), whether it drains, whether its
// deadline has passed (Drain::late), whether a turn is open, and how many builds were placed. Builds are written by
// name: "old" for the server from before takeover (which answers no `build`), "a" and "b" for the two a rebuild makes.
//
// --faulty-drain stops a draining server at its first pass, turn or no turn (a drain without the wait); --faulty-stop
// has the installer stop, not drain, a server running another build. TLC must reject both; that is the negative check
// that shows the binding can fail.

#include "server/takeover.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nexus::server::Drain;
using nexus::server::Millis;
using nexus::server::Seen;
using nexus::server::Step;

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

constexpr int kMaxPlaces = 3;

// A running server: its program's build, the Drain it holds (a server from before takeover holds none), and whether
// a friend's turn is open on it.
struct Running {
    std::string build;
    bool knowsTakeover = true;
    Drain drain;
    bool turn = false;
};

struct World {
    std::map<std::string, std::string> names;   // build id -> its name in the model
    std::string placed;                         // the placed program's build id
    std::optional<Running> server;
    int places = 0;
    Millis now = 1000 * 1000;
};

std::string flag(bool b) {
    return b ? "TRUE" : "FALSE";
}

std::string record(const World& w) {
    std::ostringstream o;
    const bool draining = w.server && w.server->drain.draining();
    o << "[placed |-> \"" << w.names.at(w.placed) << "\", server |-> \""
      << (w.server ? w.names.at(w.server->build) : std::string("none")) << "\", draining |-> " << flag(draining)
      << ", late |-> " << flag(w.server && w.server->drain.late(w.now)) << ", turn |-> "
      << flag(w.server && w.server->turn) << ", places |-> " << w.places << "]";
    return o.str();
}

// What the installer sees: the server's answer to `build`, as the control socket gives it.
Seen look(const World& w) {
    Seen s;
    s.placed = w.placed;
    if (!w.server) {
        return s;
    }
    s.answers = true;
    if (w.server->knowsTakeover) {
        const std::string reply = nexus::server::buildReply(w.server->build, w.server->drain.draining());
        if (!nexus::server::parseBuildReply(reply, &s)) {
            std::fprintf(stderr, "takeover_walk: the code does not read its own reply: %s\n", reply.c_str());
            std::exit(1);
        }
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    bool faultyDrain = false;
    bool faultyStop = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-drain") == 0) {
            faultyDrain = true;
        } else if (std::strcmp(argv[i], "--faulty-stop") == 0) {
            faultyStop = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: takeover_walk <seed> <steps> [--faulty-drain | --faulty-stop] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    if (steps < 1) {
        std::fprintf(stderr, "takeover_walk: steps must be at least 1\n");
        return 2;
    }
    Rng rng{seed};
    // Three programs: the one from before takeover and two a rebuild makes, each a build of its own bytes.
    const std::string oldBuild = nexus::server::buildOf("nexus-server from before takeover");
    const std::vector<std::string> builds = {nexus::server::buildOf("nexus-server, rebuild a"),
                                             nexus::server::buildOf("nexus-server, rebuild b")};
    World w;
    w.names = {{oldBuild, "old"}, {builds[0], "a"}, {builds[1], "b"}};
    w.placed = oldBuild;
    if (rng.below(2) == 0) {
        w.server = Running{oldBuild, false, Drain{}, false};
    }
    std::vector<std::string> trace{record(w)};
    int drained = 0, stopped = 0, cut = 0, started = 0;
    for (long k = 0; k < steps; ++k) {
        switch (rng.below(8)) {
            case 0: {   // the installer places a build (setup, rebuild), at most kMaxPlaces
                if (w.places < kMaxPlaces && rng.below(3) == 0) {
                    const std::string& b = builds[rng.below(builds.size())];
                    if (b != w.placed) {
                        w.placed = b;
                        ++w.places;
                    }
                }
                break;
            }
            case 1:
            case 2: {   // the installer looks and takes nextStep's step: only after it placed a build, as setup places
                        // itself before it looks
                if (w.places == 0) {
                    break;
                }
                const Step step = nexus::server::nextStep(look(w));
                if (step == Step::AskStop || (faultyStop && step == Step::AskDrain)) {
                    cut += w.server->turn ? 1 : 0;
                    w.server.reset();
                    ++stopped;
                } else if (step == Step::AskDrain) {
                    w.server->drain.request(w.now);
                }
                // Start is the service's (case 3); Wait and Done change nothing.
                break;
            }
            case 3: {   // the service starts the placed program when none runs
                if (!w.server) {
                    w.server = Running{w.placed, w.placed != oldBuild, Drain{}, false};
                    ++started;
                }
                break;
            }
            case 4: {   // a turn opens or ends
                if (w.server) {
                    w.server->turn = !w.server->turn;
                }
                break;
            }
            case 5: {   // the clock: a moment, or, while draining, sometimes past the deadline
                w.now += (w.server && w.server->drain.draining() && rng.below(4) == 0) ? nexus::server::kDrainMs : 50;
                break;
            }
            default: {  // a pass of the running server's loop
                if (w.server && w.server->knowsTakeover) {
                    const Drain& d = w.server->drain;
                    const bool stop = faultyDrain ? d.draining() : d.stopNow(w.server->turn, w.now);
                    if (stop) {
                        cut += w.server->turn ? 1 : 0;
                        w.server.reset();
                        ++drained;
                    }
                }
                break;
            }
        }
        trace.push_back(record(w));
    }
    std::ofstream out(args[2]);
    if (!out) {
        std::fprintf(stderr, "takeover_walk: cannot write %s\n", args[2]);
        return 2;
    }
    out << "---- MODULE TakeoverTraceData ----\n";
    out << "\\* Written by tests/server/takeover_walk: seed " << seed << ", " << steps << " steps"
        << (faultyDrain ? ", faulty-drain" : "") << (faultyStop ? ", faulty-stop" : "") << ".\n";
    out << "EXTENDS TLC\n";
    out << "TraceBuilds == {\"a\", \"b\"}\nTraceOld == \"old\"\nTraceNone == \"none\"\nTraceMaxPlaces == " << kMaxPlaces
        << "\nTrace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    std::printf("takeover_walk: %zu records, %d starts, %d drained, %d stopped, %d turns cut\n", trace.size(), started,
        drained, stopped, cut);
    return 0;
}
