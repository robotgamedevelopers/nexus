// Drives the real nexus::TabLabel (src/remote.h) with a seeded random walk and writes the trace as a TLA+ module
// (TabLabelTraceData.tla) for models/TabLabelTrace.tla, under which TLC checks that the trace is a behaviour of
// models/TabLabel.tla.
//
//   label_walk <seed> <steps> <remotes> [--faulty-before] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64) and its clock is
// made up (from 1000 s, a frame or a few hundred milliseconds a step), never the system's. Each step is one frame's
// observation: sessions start, run, switch remote and end, the title changes or stays, and frames come inside a
// session's tail and after it. Each record holds the observation's arguments, `late` (whether the frame's time is past
// the tail of the last session to end, reckoned by the walk from its own clock), the code's state after it, and the
// label the code draws for every title.
//
// --faulty-before feeds the code one more observation outside a session, unrecorded, just before each session's first:
// the code then takes the label to return to from a title ssh may already have set, as its first version did. TLC must
// reject its traces; that is the negative check that shows the binding can fail.

#include "remote.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

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

std::string str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

const std::vector<std::string> kTitles = {"", "sh", "nova@space: ~", "vim notes.txt", "left"};
const std::vector<std::string> kAllRemotes = {"space", "hetzner2", "example.com"};

// One record: the observation's arguments, then the code's state after it and the label it draws for every title.
std::string record(const nexus::TabLabel& code, const std::string& remote, const std::string& title, bool late) {
    std::ostringstream o;
    o << "[remote |-> " << str(remote) << ", title |-> " << str(title) << ", late |-> " << (late ? "TRUE" : "FALSE")
      << ", code |-> [remote |-> " << str(code.remote()) << ", before |-> " << str(code.before()) << ", after |-> "
      << str(code.after()) << ", masking |-> " << (code.masking() ? "TRUE" : "FALSE") << ", tail |-> "
      << (code.tail() ? "TRUE" : "FALSE") << "], labels |-> (";
    for (std::size_t i = 0; i < kTitles.size(); ++i)
        o << (i ? " @@ " : "") << str(kTitles[i]) << " :> " << str(code.label(kTitles[i]));
    o << ")]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    bool faulty = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-before") == 0)
            faulty = true;
        else
            args.push_back(argv[i]);
    }
    if (args.size() != 4) {
        std::fprintf(stderr, "usage: label_walk <seed> <steps> <remotes 1-3> [--faulty-before] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    const std::size_t count = std::size_t(std::strtoul(args[2], nullptr, 10));
    if (steps < 1 || count < 1 || count > kAllRemotes.size()) {
        std::fprintf(stderr, "label_walk: steps must be at least 1, and remotes 1 to 3\n");
        return 2;
    }
    const std::vector<std::string> remotes(kAllRemotes.begin(), kAllRemotes.begin() + long(count));
    Rng rng{seed};
    nexus::TabLabel code;
    std::vector<std::string> trace{record(code, "", "", false)};
    // Milliseconds per step: a frame at 60 Hz, or a pause.
    const long kStepMs[] = {17, 17, 100, 300, 600};
    long ms = 0;
    double ended = -1;          // when the last session to end was seen to end
    std::string remote;         // the session the last observation saw
    std::string title;
    int sessions = 0, tails = 0;
    for (long k = 0; k < steps; ++k) {
        ms += kStepMs[rng.below(5)];
        const double time = 1000.0 + double(ms) / 1000.0;
        std::string next = remote;
        const std::size_t roll = rng.below(10);
        if (remote.empty()) {
            if (roll < 3)
                next = remotes[rng.below(remotes.size())];
        } else if (roll < 2) {
            next.clear();
        } else if (roll < 3) {
            next = remotes[rng.below(remotes.size())];
        }
        if (rng.below(2) == 0)
            title = kTitles[rng.below(kTitles.size())];
        if (faulty && remote.empty() && !next.empty())
            code.observe("", title, time);
        const bool ends = !remote.empty() && next.empty();
        const bool late = ended >= 0 && !(time < ended + nexus::kSessionTailSeconds);
        code.observe(next, title, time);
        if (ends) {
            ended = time;
            ++sessions;
        }
        if (code.tail())
            ++tails;
        trace.push_back(record(code, next, title, ends ? false : late));
        remote = next;
    }
    std::ofstream out(args[3]);
    if (!out) {
        std::fprintf(stderr, "label_walk: cannot write %s\n", args[3]);
        return 2;
    }
    out << "---- MODULE TabLabelTraceData ----\n";
    out << "\\* Written by tests/model/label_walk: seed " << seed << ", " << steps << " steps, " << count << " remotes"
        << (faulty ? ", faulty-before" : "") << ".\n";
    out << "EXTENDS TLC\n";
    out << "TraceTitles == {";
    for (std::size_t i = 0; i < kTitles.size(); ++i)
        out << (i ? ", " : "") << str(kTitles[i]);
    out << "}\nTraceRemotes == {";
    for (std::size_t i = 0; i < remotes.size(); ++i)
        out << (i ? ", " : "") << str(remotes[i]);
    out << "}\nTrace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i)
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    out << ">>\n====\n";
    std::printf("label_walk: %zu records, %d sessions ended, %d observations in a tail\n", trace.size(), sessions, tails);
    return 0;
}
