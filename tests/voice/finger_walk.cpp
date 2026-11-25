// Drives the real nexus::FingerScroll (src/finger_scroll.h) with a seeded random walk and writes the trace as a TLA+
// module (FingerScrollTraceData.tla) for models/FingerScrollTrace.tla, under which TLC checks that the trace is a
// behaviour of models/FingerScroll.tla.
//
//   finger_walk <seed> <steps> [--faulty-caught] <out.tla>
//
// The list is in rows of 50 px in a view of 2 rows (100 px); its range is 0 to 3 rows (a content shorter than the view,
// a range below 0, is 0). Each step is one of: a frame some ms to half a second on; a finger down somewhere in the view
// (the code shows the motion at the press's time first, which the walk writes as a frame of its own); its move some ms
// on, a few px to most of a row; its up; a cancel; or a change of the range. The walk is a pure function of its
// arguments: its random numbers come from the seed (splitmix64), and its time is seconds it counts itself, never the
// clock's. Each record holds the step and the code's state after it, quantised to the model's: the offset in whole
// rows rounded away from 0 (so the range 0..max in px is 0..max in rows, and any part of a row past an end is past it),
// a fling's way, a spring's end, side, and stage (out while its speed is away from its end).
//
// --faulty-caught walks ForgetfulScroll instead: a test-only scroller whose press forgets that it caught a moving list,
// so its up would tap the row under it. TLC must reject its traces (no step of the spec reaches the press's state: a
// deadlock); that is the negative check that shows the binding can fail.

#include "finger_scroll.h"
#include "scroll_glide.h"

#include <algorithm>
#include <cmath>
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

constexpr double kRow = 50;
constexpr double kView = 100;
constexpr int kMaxRows = 3;
constexpr int kBand = 2;  // a drag past an end shows under the view's height: 2 rows
constexpr int kBound = 4; // a spring carried on out stays under 4 rows past its end at these speeds
constexpr double kDts[] = {0.004, 1.0 / 120, 1.0 / 60, 0.1, 0.5};
constexpr double kMoveDts[] = {1.0 / 60, 0.033};
constexpr double kDys[] = {-40, -15, -6, -2, 2, 6, 15, 40};

// The test-only fault: a press that caught a moving list forgets it. Hides FingerScroll::down; the walk is a template.
class ForgetfulScroll : public FingerScroll {
public:
    void down(double y, double t, double now) {
        FingerScroll::down(y, t, now);
        caught_ = false;
    }
};

// The scroller, with its motion read for the record.
template <typename Base>
class Walked : public Base {
public:
    bool caught() const { return this->caught_; }
    int flingWay() const { return this->v0_ > 0 ? 1 : this->v0_ < 0 ? -1 : 0; }
    double springEnd() const { return this->end_; }
    int springSide() const { return this->side_; }
    // A spring's speed where it shows, away from its end (out) or not (back).
    bool springOut() const {
        const double v = springSpeed(this->p0_, this->v0_, std::max(0.0, this->shown_ - this->start_));
        return v * this->side_ > 0;
    }
};

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    int below(int n) { return int(next() % uint64_t(n)); }
};

// Whole rows, rounded away from 0.
int rows(double x) {
    return x < 0 ? -int(std::ceil(-x / kRow)) : int(std::ceil(x / kRow));
}

const char* b(bool v) { return v ? "TRUE" : "FALSE"; }

template <typename Scroll>
int walk(uint64_t seed, int steps, const char* path) {
    Rng rng{seed};
    Walked<Scroll> s;
    double now = 0, y = 50;
    int catches = 0;
    std::vector<std::string> records;
    std::string error;
    auto record = [&](const char* op, int m, bool tap) {
        const FingerPhase phase = s.phase();
        const int off = rows(s.offset());
        if (off < -kBound || off > kMaxRows + kBound) {
            std::ostringstream e;
            e << "step " << records.size() << " (" << op << "): offset " << s.offset() << " px is " << off
              << " rows, outside the model's " << -kBound << ".." << kMaxRows + kBound;
            if (error.empty()) error = e.str();
        }
        const bool spring = phase == FingerPhase::Spring;
        std::ostringstream o;
        o << "[op |-> \"" << op << "\", m |-> " << m << ", phase |-> \"" << fingerPhaseName(phase)
          << "\", off |-> " << off << ", max |-> " << std::lround(s.max() / kRow) << ", caught |-> " << b(s.caught())
          << ", dir |-> " << (phase == FingerPhase::Fling ? s.flingWay() : 0) << ", send |-> "
          << (spring ? std::lround(s.springEnd() / kRow) : 0) << ", side |-> " << (spring ? s.springSide() : 0)
          << ", stage |-> \"" << (spring ? (s.springOut() ? "out" : "back") : "none") << "\", tap |-> " << b(tap)
          << "]";
        records.push_back(o.str());
    };
    s.setRange(0, kView);
    record("init", 0, false);
    for (int i = 0; i < steps; ++i) {
        const int pick = rng.below(20);
        if (pick < 7) {
            now += kDts[rng.below(s.moving() ? 3 : 5)]; // a motion is mostly seen frame by frame
            s.frame(now);
            record("tick", 0, false);
        } else if (pick < 9) {
            s.frame(now);
            record("tick", 0, false);
            if (s.moving()) ++catches;
            y = 5 + rng.below(90);
            s.down(y, now, now);
            record("down", 0, false);
        } else if (pick < 14) {
            now += kMoveDts[rng.below(2)];
            y += kDys[rng.below(8)];
            s.move(y, now);
            record("move", 0, false);
        } else if (pick < 16) {
            const bool tap = s.up(now, now);
            record("up", 0, tap);
        } else if (pick < 17) {
            s.cancel(now);
            record("cancel", 0, false);
        } else {
            const int m = rng.below(kMaxRows + 2) - 1; // -1: the content shorter than the view
            s.setRange(m * kRow, kView);
            record("range", std::max(0, m), false);
        }
    }
    if (!error.empty()) {
        std::fprintf(stderr, "finger_walk: %s\n", error.c_str());
        return 1;
    }
    std::ofstream file(path);
    if (!file) {
        std::fprintf(stderr, "finger_walk: cannot write %s\n", path);
        return 1;
    }
    file << "---- MODULE FingerScrollTraceData ----\n";
    file << "\\* Written by tests/voice/finger_walk (seed " << seed << ", " << steps
         << " steps): one trace of the real nexus::FingerScroll, in rows of " << kRow << " px.\n";
    file << "EXTENDS Integers\n";
    file << "TraceMaxes == 0 .. " << kMaxRows << "\nTraceBand == " << kBand << "\nTraceBound == " << kBound << "\n";
    file << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i)
        file << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    file << ">>\n====\n";
    std::printf("finger_walk: %zu records (%d presses on a moving list) written to %s\n", records.size(), catches,
                path);
    return file.good() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool faulty = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-caught") == 0) faulty = true;
        else args.push_back(argv[i]);
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: finger_walk <seed> <steps> [--faulty-caught] <out.tla>\n");
        return 2;
    }
    const uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const int steps = std::atoi(args[1]);
    return faulty ? walk<ForgetfulScroll>(seed, steps, args[2]) : walk<FingerScroll>(seed, steps, args[2]);
}
