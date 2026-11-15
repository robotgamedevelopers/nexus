// Drives the real takeover code (src/server/takeover.h: Handover, the handover process's side over nextStep, and
// Drain, the running server's) with a seeded random walk and writes the trace as a TLA+ module (TakeoverTraceData.tla)
// for models/TakeoverTrace.tla, under which TLC checks that the trace is a behaviour of models/Takeover.tla.
//
//   takeover_walk <seed> <steps> [--faulty-drain | --faulty-stop | --faulty-rollback] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64) and its clock is
// made up (from 1000 s), never the system's. It plays everything around the code: setup placing builds (each a build of
// made-up bytes, its id from buildOf) when no handover runs, keeping the one placed before as the previous; a service,
// when the walk has one, starting the placed program when none runs; friends' turns opening and ending; and the clock
// moving, sometimes past a drain's deadline or a start's. Build "b" is broken on the store: started, it never answers.
// The handover's move each time is what its Handover says for what it sees, and the running server's stop is what its
// Drain says at each pass of its loop. Each record is the state after one step: the placed build and the previous one,
// the running one (or none), whether it drains, whether its deadline has passed (Drain::late), whether a turn is open,
// how many builds were placed, whether there is a service, whether a handover runs, whether it saw the placed build
// fail, and whether it rolled back. Builds are written by name: "old" for the server from before takeover (which
// answers no `build`), "a" and "b" for the two a rebuild makes.
//
// --faulty-drain stops a draining server at its first pass, turn or no turn (a drain without the wait); --faulty-stop
// has the handover stop, not drain, a server running another build; --faulty-rollback leaves the broken program in
// place when the handover rolls back (the copy of the previous one skipped). TLC must reject each; that is the
// negative check that shows the binding can fail.

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

using nexus::server::Act;
using nexus::server::Drain;
using nexus::server::Handover;
using nexus::server::Millis;
using nexus::server::Seen;

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
    std::string broken;                         // the build that never comes up on the store
    std::string oldBuild;
    std::string placed;                         // the placed program's build id
    std::string previous;                       // the program kept beside it
    std::optional<Running> server;
    std::optional<Handover> handover;           // the handover process, while it runs
    int places = 0;
    bool service = false;
    Millis now = 1000 * 1000;
};

std::string flag(bool b) {
    return b ? "TRUE" : "FALSE";
}

std::string record(const World& w) {
    std::ostringstream o;
    const bool draining = w.server && w.server->drain.draining();
    o << "[placed |-> \"" << w.names.at(w.placed) << "\", previous |-> \"" << w.names.at(w.previous)
      << "\", server |-> \"" << (w.server ? w.names.at(w.server->build) : std::string("none"))
      << "\", draining |-> " << flag(draining) << ", late |-> " << flag(w.server && w.server->drain.late(w.now))
      << ", turn |-> " << flag(w.server && w.server->turn) << ", places |-> " << w.places << ", service |-> "
      << flag(w.service) << ", handing |-> " << flag(w.handover.has_value()) << ", failed |-> "
      << flag(w.handover && w.handover->failed()) << ", rolled |-> " << flag(w.handover && w.handover->rolledBack())
      << "]";
    return o.str();
}

// What the handover sees: the server's answer to `build`, as the control socket gives it.
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

// The placed program is started (by the service or the handover): it comes up, unless it is the broken build.
bool startPlaced(World& w) {
    if (w.server || w.placed == w.broken) {
        return false;
    }
    w.server = Running{w.placed, w.placed != w.oldBuild, Drain{}, false};
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool faultyDrain = false;
    bool faultyStop = false;
    bool faultyRollback = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-drain") == 0) {
            faultyDrain = true;
        } else if (std::strcmp(argv[i], "--faulty-stop") == 0) {
            faultyStop = true;
        } else if (std::strcmp(argv[i], "--faulty-rollback") == 0) {
            faultyRollback = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr,
            "usage: takeover_walk <seed> <steps> [--faulty-drain | --faulty-stop | --faulty-rollback] <out.tla>\n");
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
    w.oldBuild = oldBuild;
    w.broken = builds[1];
    w.placed = oldBuild;
    w.previous = oldBuild;
    w.service = rng.below(2) == 0;
    if (rng.below(2) == 0) {
        w.server = Running{oldBuild, false, Drain{}, false};
    }
    std::vector<std::string> trace{record(w)};
    int drained = 0, stopped = 0, cut = 0, started = 0, rolledBack = 0, handedOver = 0;
    int faults = 0;     // steps a faulty mode changed: a faulty walk with none checks nothing
    for (long k = 0; k < steps; ++k) {
        switch (rng.below(9)) {
            case 0: {   // setup places a build, at most kMaxPlaces, and only when no handover runs (it refuses then)
                if (!w.handover && w.places < kMaxPlaces && rng.below(3) == 0) {
                    const std::string& b = builds[rng.below(builds.size())];
                    if (b != w.placed) {
                        w.previous = w.placed;
                        w.placed = b;
                        ++w.places;
                        w.handover.emplace(w.placed, w.previous);
                    }
                }
                break;
            }
            case 1:
            case 2: {   // the handover looks and does what its Handover says
                if (!w.handover) {
                    break;
                }
                const Act act = w.handover->step(look(w), w.now);
                if (act == Act::AskStop || (faultyStop && act == Act::AskDrain)) {
                    faults += act == Act::AskDrain ? 1 : 0;
                    cut += w.server->turn ? 1 : 0;
                    w.server.reset();
                    ++stopped;
                } else if (act == Act::AskDrain) {
                    w.server->drain.request(w.now);
                } else if (act == Act::Start) {
                    started += startPlaced(w) ? 1 : 0;
                } else if (act == Act::RollBack) {
                    if (faultyRollback) {
                        ++faults;
                    } else {
                        w.placed = w.handover->placed();
                    }
                    ++rolledBack;
                } else if (act == Act::Done) {
                    w.handover.reset();
                    ++handedOver;
                } else if (act == Act::Fail && !faultyRollback) {   // a faulty rollback leaves the broken build: Fail
                    std::fprintf(stderr, "takeover_walk: the handover failed, with a previous build that works\n");
                    return 1;
                }
                break;
            }
            case 3: {   // the service, when there is one, starts the placed program when none runs
                if (w.service) {
                    started += startPlaced(w) ? 1 : 0;
                }
                break;
            }
            case 4: {   // a turn opens or ends
                if (w.server) {
                    w.server->turn = !w.server->turn;
                }
                break;
            }
            case 5: {   // the clock: a moment, or sometimes past a drain's deadline or a start's
                Millis by = 50;
                if (rng.below(4) == 0) {
                    by = (w.server && w.server->drain.draining()) ? nexus::server::kDrainMs : nexus::server::kStartMs;
                }
                w.now += by;
                break;
            }
            default: {  // a pass of the running server's loop
                if (w.server && w.server->knowsTakeover) {
                    const Drain& d = w.server->drain;
                    const bool stop = faultyDrain ? d.draining() : d.stopNow(w.server->turn, w.now);
                    faults += stop && !d.stopNow(w.server->turn, w.now) ? 1 : 0;
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
        << (faultyDrain ? ", faulty-drain" : "") << (faultyStop ? ", faulty-stop" : "")
        << (faultyRollback ? ", faulty-rollback" : "") << ".\n";
    out << "EXTENDS TLC\n";
    out << "TraceBuilds == {\"a\", \"b\"}\nTraceBroken == {\"b\"}\nTraceOld == \"old\"\nTraceNone == \"none\"\n"
        << "TraceMaxPlaces == " << kMaxPlaces << "\nTrace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "takeover_walk: cannot write %s\n", args[2]);
        return 2;
    }
    std::printf("takeover_walk: %zu records, %d starts, %d drained, %d stopped, %d turns cut, %d rolled back, "
                "%d handed over, %d faulty steps\n", trace.size(), started, drained, stopped, cut, rolledBack,
                handedOver, faults);
    if ((faultyDrain || faultyStop || faultyRollback) && faults == 0) {
        std::fprintf(stderr, "takeover_walk: the faulty mode changed no step at this seed; pick another seed\n");
        return 1;
    }
    return 0;
}
