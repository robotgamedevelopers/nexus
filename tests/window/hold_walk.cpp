// The binding of the held keys' code to its model: a seeded random walk over the real hold (src/scroll_hold.h) that
// writes a trace for models/scroll/ScrollHoldTrace.tla to check against ScrollHold.tla.
//
//   hold_walk <seed> <steps> [--fault repeat-driven | --fault repeat-changes] <out.tla>
//
// Each step is one of: a press of Up or Down (now and then of a key already down), a release (now and then of a key
// not down, as Prax's KeyUp after the window lost the keyboard), a repeat (mostly of a key down, now and then of one
// not), a blur, or a frame 4 to 33 ms after the last. Presses and releases come at the time of the frame last shown, as
// the board's do. After each step the trace holds the code's up, down, holdDirection and, for a frame, the sign of the
// move holdStep returned.
//
// The faults are test-only variants the trace check must reject: repeat-driven takes a press as nothing and the first
// repeat of a key as its press (the scroll as it was, driven by repeats); repeat-changes takes every repeat as a press.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "scroll_hold.h"

namespace {

using namespace nexus;

const char* name(HoldKey k) {
    return k == HoldKey::Up ? "up" : "down";
}

int sign(double x) {
    return x > 0 ? 1 : (x < 0 ? -1 : 0);
}

} // namespace

int main(int argc, char** argv) {
    std::string fault;
    const char* args[3] = {};
    int n = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fault") == 0 && i + 1 < argc) {
            fault = argv[++i];
            if (fault != "repeat-driven" && fault != "repeat-changes") {
                std::fprintf(stderr, "hold_walk: unknown fault %s; the faults are repeat-driven and repeat-changes\n",
                    fault.c_str());
                return 2;
            }
        } else if (n < 3) {
            args[n++] = argv[i];
        } else {
            n = 4;
        }
    }
    if (n != 3) {
        std::fprintf(stderr, "usage: hold_walk <seed> <steps> [--fault repeat-driven | --fault repeat-changes] "
            "<out.tla>\n");
        return 2;
    }
    const unsigned long seed = std::strtoul(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    std::FILE* out = std::fopen(args[2], "w");
    if (out == nullptr) {
        std::fprintf(stderr, "hold_walk: cannot write %s; check that its directory exists and is writable\n", args[2]);
        return 1;
    }

    std::mt19937_64 rng(seed);
    auto roll = [&](int below) { return int(rng() % std::uint64_t(below)); };

    Hold hold;
    bool physical[2] = {false, false};     // the keys as the walk holds them (the fault's view of what is down)
    double now = 1000;
    std::fprintf(out, "---- MODULE ScrollHoldTraceData ----\nEXTENDS Integers\n\\* hold_walk %s %s%s%s\n", args[0],
        args[1], fault.empty() ? "" : " --fault ", fault.c_str());
    std::fprintf(out, "Trace == <<\n");
    auto record = [&](const char* op, HoldKey key, int moved, bool first) {
        std::fprintf(out, "%s[op |->\"%s\", key |-> \"%s\", up |-> %s, down |-> %s, dir |-> %d, moved |-> %d]\n",
            first ? "  " : ", ", op, name(key), hold.up ? "TRUE" : "FALSE", hold.down ? "TRUE" : "FALSE",
            holdDirection(hold), moved);
    };
    record("init", HoldKey::Up, 0, true);

    int frames = 0, moves = 0, repeats = 0;
    for (long i = 0; i < steps; ++i) {
        const int pick = roll(20);
        const HoldKey key = roll(2) == 0 ? HoldKey::Up : HoldKey::Down;
        bool& isDown = physical[key == HoldKey::Up ? 0 : 1];
        if (pick < 3) {
            // A press: of a key not down, or now and then of one already down.
            if (isDown && roll(4) != 0) {
                --i;
                continue;
            }
            isDown = true;
            if (fault != "repeat-driven")
                hold = holdPress(hold, key, now);
            record("press", key, 0, false);
        } else if (pick < 6) {
            // A release: of a key down, or now and then of one not.
            if (!isDown && roll(4) != 0) {
                --i;
                continue;
            }
            isDown = false;
            hold = holdRelease(hold, key, now);
            record("release", key, 0, false);
        } else if (pick < 10) {
            ++repeats;
            if (fault == "repeat-changes" || (fault == "repeat-driven" && isDown))
                hold = holdPress(hold, key, now);
            else
                hold = holdRepeat(hold, key);
            record("repeat", key, 0, false);
        } else if (pick < 11) {
            physical[0] = physical[1] = false;
            hold = holdBlur(hold);
            record("blur", key, 0, false);
        } else {
            now += (4 + roll(30)) / 1000.0;
            const int moved = sign(holdStep(&hold, now, kHoldSpeed));
            ++frames;
            moves += moved != 0;
            record("frame", key, moved, false);
        }
    }
    std::fprintf(out, ">>\n====\n");
    std::fclose(out);
    std::printf("hold_walk: seed %lu, %ld steps: %d frames (%d moved), %d repeats%s%s\n", seed, steps, frames, moves,
        repeats, fault.empty() ? "" : ", fault ", fault.c_str());
    return 0;
}
