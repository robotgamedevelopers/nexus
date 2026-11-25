// Drives the real nexus::AccountMenuGesture (src/account_menu.h) with a seeded random walk and writes the trace as a TLA+
// module (AccountMenuTraceData.tla) for models/AccountMenuTrace.tla, under which TLC checks that the trace is a
// behaviour of models/AccountMenu.tla.
//
//   account_menu_walk <seed> <steps> [--faulty-sideways] <out.tla>
//
// The layout is fixed, in points: the active tab at 100..200 x 0..40, another tab at 210..310 x 0..40, and the menu
// laid out under the active tab by layoutAccountMenu (two rows). Each step is a press, a move, a lift or a cancel at one
// of the places (the active tab, the other tab, a row, anywhere else); a move is sideways past the threshold from the
// press, or not. The walk is a pure function of its arguments (splitmix64), with no clock.
//
// --faulty-sideways walks a test-only gesture that keeps the menu on a sideways move (the tab never moves). TLC must
// reject its traces: the negative check that shows the binding can fail.

#include "account_menu.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

using namespace nexus;

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    unsigned below(unsigned n) { return unsigned(next() % n); }
};

const MenuBox kActive{100, 0, 200, 40};
const MenuBox kOther{210, 0, 310, 40};

struct Spot { const char* name; float x, y; };

std::string phaseName(AccountMenuGesture::Phase p) {
    return p == AccountMenuGesture::Phase::Open ? "open" : p == AccountMenuGesture::Phase::TabMove ? "tabmove" : "closed";
}

std::string rowName(std::optional<std::size_t> row) {
    return row ? "r" + std::to_string(*row + 1) : "none";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: account_menu_walk <seed> <steps> [--faulty-sideways] <out.tla>\n");
        return 2;
    }
    Rng rng{std::strtoull(argv[1], nullptr, 10)};
    const int steps = std::atoi(argv[2]);
    const bool faulty = argc > 4 && std::strcmp(argv[3], "--faulty-sideways") == 0;
    std::ofstream out(argv[argc - 1]);
    if (!out) {
        std::fprintf(stderr, "account_menu_walk: cannot write %s\n", argv[argc - 1]);
        return 2;
    }
    AccountMenuStyle style;
    const AccountMenuLayout layout = layoutAccountMenu(kActive, 2, 120, 17, 1000, style);
    const Spot spots[] = {
        {"active", 150, 20},
        {"other", 260, 20},
        {"r1", 150, (layout.rows[0].top + layout.rows[0].bottom) / 2},
        {"r2", 150, (layout.rows[1].top + layout.rows[1].bottom) / 2},
        {"off", 600, 600},
    };
    AccountMenuGesture gesture(style.tabMoveThreshold);
    out << "---- MODULE AccountMenuTraceData ----\nTrace == <<\n";
    out << "[op |-> \"init\", at |-> \"off\", way |-> \"down\", phase |-> \"closed\", hovered |-> \"none\", "
           "chosen |-> \"none\"]";
    for (int i = 0; i < steps; ++i) {
        const unsigned kind = rng.below(10);
        const Spot& spot = spots[rng.below(5)];
        std::string op, way = "down";
        AccountMenuGesture::Step step;
        if (kind < 3) {
            op = "press";
            step = gesture.down(spot.x, spot.y, kActive.contains(spot.x, spot.y));
        } else if (kind < 7) {
            op = "move";
            // Sideways: from the press, far to the side at the press's height (past the threshold); else straight to
            // the spot, which from the active tab to a row or anywhere below is mostly downward.
            const bool sideways = rng.below(3) == 0;
            float x = spot.x, y = spot.y;
            if (sideways) {
                way = "sideways";
                x = gesture.originX() + (rng.below(2) ? 1 : -1) * (style.tabMoveThreshold + 30);
                y = gesture.originY();
            } else if (gesture.open()) {
                // Keep a downward move downward: from the press straight down to the spot's height.
                x = gesture.originX();
            }
            if (faulty && sideways && gesture.open()) {
                step = gesture.move(gesture.originX(), gesture.originY(), layout);    // the menu stays
            } else {
                step = gesture.move(x, y, layout);
            }
            // What the record says is where the finger is, as the model abstracts it.
            const char* at = "off";
            for (const Spot& s : spots) {
                const bool inRow = std::string(s.name)[0] == 'r';
                if ((inRow && layout.rowAt(x, y) == std::optional<std::size_t>(s.name[1] - '1')) ||
                    (std::string(s.name) == "active" && kActive.contains(x, y)) ||
                    (std::string(s.name) == "other" && kOther.contains(x, y)))
                    at = s.name;
            }
            out << ",\n[op |-> \"move\", at |-> \"" << at << "\", way |-> \"" << way << "\", phase |-> \""
                << phaseName(gesture.phase()) << "\", hovered |-> \"" << rowName(gesture.hovered())
                << "\", chosen |-> \"none\"]";
            continue;
        } else if (kind < 9) {
            op = "lift";
            step = gesture.up(spot.x, spot.y, layout);
        } else {
            op = "cancel";
            step = gesture.cancel();
        }
        out << ",\n[op |-> \"" << op << "\", at |-> \"" << (op == "cancel" ? "off" : spot.name) << "\", way |-> \"down\", "
            << "phase |-> \"" << phaseName(gesture.phase()) << "\", hovered |-> \"" << rowName(gesture.hovered())
            << "\", chosen |-> \"" << rowName(step.chosen) << "\"]";
    }
    out << "\n>>\n====\n";
    return out ? 0 : 1;
}
