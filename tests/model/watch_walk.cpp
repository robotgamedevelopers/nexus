// Drives the real foreground-watch synchronization functions (src/terminal_watch.h) with a seeded random walk across
// background-poll and frame-thread interleavings and writes TerminalWatchTraceData.tla for models/TerminalWatchTrace.tla.
//
//   watch_walk <seed> <steps> [--faulty-aba | --faulty-stale] <out.tla>
//
// --faulty-aba reproduces the pre-fix one-way bug where a frame's title observation updates `seen` without updating
// `shared`; --faulty-stale lets an overtaken background poll overwrite `shared` instead of asking for a recheck. TLC
// must reject both traces.

#include "terminal_watch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace nexus;

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

enum class Fault { None, Aba, Stale };

// Groups 1 (shell, remote "") and 2, 3 (ssh sessions, remotes "2" and "3").
int remoteNumber(const std::string& remote) {
    if (remote == "2")
        return 2;
    if (remote == "3")
        return 3;
    return 0;
}

std::string remoteString(int group) {
    if (group == 2)
        return "2";
    if (group == 3)
        return "3";
    return "";
}

std::string seenRecord(const ForegroundSeen& s) {
    std::ostringstream o;
    o << "[seq |-> " << s.seq << ", group |-> " << s.group << ", remote |-> " << remoteNumber(s.remote) << "]";
    return o.str();
}

std::string stepRecord(const char* op, int osGroup, const ForegroundSeen& shared, const ForegroundSeen& seen,
        bool recheck, bool pollActive, bool frameActive, bool changed, bool askedRecheck) {
    std::ostringstream o;
    o << "[op |-> \"" << op << "\", osGroup |-> " << osGroup << ", shared |-> " << seenRecord(shared)
      << ", seen |-> " << seenRecord(seen) << ", recheck |-> " << (recheck ? "TRUE" : "FALSE")
      << ", pollActive |-> " << (pollActive ? "TRUE" : "FALSE")
      << ", frameActive |-> " << (frameActive ? "TRUE" : "FALSE")
      << ", changed |-> " << (changed ? "TRUE" : "FALSE")
      << ", askedRecheck |-> " << (askedRecheck ? "TRUE" : "FALSE") << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    Fault fault = Fault::None;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-aba") == 0)
            fault = Fault::Aba;
        else if (std::strcmp(argv[i], "--faulty-stale") == 0)
            fault = Fault::Stale;
        else
            args.push_back(argv[i]);
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: watch_walk <seed> <steps> [--faulty-aba | --faulty-stale] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    if (steps < 1) {
        std::fprintf(stderr, "watch_walk: steps must be at least 1\n");
        return 2;
    }

    Rng rng{seed};
    int osGroup = 1;
    ForegroundSeen shared;
    ForegroundSeen seen;
    bool recheck = false;

    bool pollActive = false;
    ForegroundSeen pollSnap;
    int pollGroup = -1;
    std::string pollRemote;

    bool frameActive = false;
    int frameGroup = -1;
    std::string frameRemote;

    std::vector<std::string> trace{
        stepRecord("init", osGroup, shared, seen, recheck, pollActive, frameActive, false, false)};
    int overlaps = 0;
    int faults = 0;

    for (long k = 0; k < steps; ++k) {
        std::vector<int> enabled;
        enabled.push_back(0); // env change
        if (!frameActive && shared.seq > seen.seq)
            enabled.push_back(1); // frame-take
        if (!frameActive)
            enabled.push_back(2); // frame-start
        if (frameActive)
            enabled.push_back(3); // frame-commit
        if (!pollActive)
            enabled.push_back(4); // poll-start
        if (pollActive)
            enabled.push_back(5); // poll-commit

        const int pick = enabled[rng.below(enabled.size())];
        const char* op = "";
        bool changed = false;
        bool askedRecheck = false;

        switch (pick) {
        case 0: {
            const int others[2] = {osGroup == 1 ? 2 : 1, osGroup == 3 ? 2 : 3};
            osGroup = others[rng.below(2)];
            op = "env";
            break;
        }
        case 1:
            takeForegroundWatch(shared, seen);
            op = "frame-take";
            break;
        case 2:
            takeForegroundWatch(shared, seen);
            frameGroup = osGroup;
            frameRemote = foregroundNeedsArguments(seen, frameGroup) ? remoteString(frameGroup) : seen.remote;
            frameActive = true;
            op = "frame-start";
            break;
        case 3:
            if (fault == Fault::Aba) {
                if (frameGroup != seen.group || frameRemote != seen.remote) {
                    ++faults;
                }
                seen = ForegroundSeen{seen.seq + 1, frameGroup, frameRemote};
                askedRecheck = false;
            } else {
                askedRecheck = commitFrameForeground(shared, seen, frameGroup, frameRemote);
                if (askedRecheck) {
                    recheck = true;
                    ++overlaps;
                }
            }
            changed = true;
            frameActive = false;
            frameGroup = -1;
            frameRemote.clear();
            op = "frame-commit";
            break;
        case 4:
            recheck = false;
            pollSnap = shared;
            pollGroup = osGroup;
            pollRemote = pollNeedsArguments(pollSnap, pollGroup) ? remoteString(pollGroup) : pollSnap.remote;
            pollActive = true;
            op = "poll-start";
            break;
        case 5:
            if (fault == Fault::Stale && shared.seq != pollSnap.seq && pollNeedsArguments(pollSnap, pollGroup)) {
                shared = ForegroundSeen{shared.seq + 1, pollGroup, pollRemote};
                changed = true;
                askedRecheck = false;
                ++overlaps;
                ++faults;
            } else {
                const PollCommit c = commitPollForeground(shared, pollSnap, pollGroup, pollRemote);
                changed = c.changed;
                askedRecheck = c.recheck;
                if (c.recheck) {
                    recheck = true;
                    ++overlaps;
                }
            }
            pollActive = false;
            pollSnap = ForegroundSeen{};
            pollGroup = -1;
            pollRemote.clear();
            op = "poll-commit";
            break;
        }
        trace.push_back(
            stepRecord(op, osGroup, shared, seen, recheck, pollActive, frameActive, changed, askedRecheck));
    }

    std::ofstream out(args[2]);
    if (!out) {
        std::fprintf(stderr, "watch_walk: cannot write %s\n", args[2]);
        return 2;
    }
    out << "---- MODULE TerminalWatchTraceData ----\n";
    out << "\\* Written by tests/model/watch_walk: seed " << seed << ", " << steps << " steps.\n";
    out << "EXTENDS Integers\n";
    out << "TraceGroups == {1, 2, 3}\n";
    out << "TraceSshGroups == {2, 3}\n";
    out << "Trace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i)
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    out << ">>\n====\n";
    std::printf("watch_walk: %zu records, %d overlapped commits, %d faults\n", trace.size(), overlaps, faults);
    if (fault == Fault::None && overlaps == 0) {
        std::fprintf(stderr, "watch_walk: walk never exercised an overlapped commit; try more steps\n");
        return 1;
    }
    if (fault != Fault::None && faults == 0) {
        std::fprintf(stderr, "watch_walk: faulty walk never hit its fault; try more steps\n");
        return 1;
    }
    return 0;
}
