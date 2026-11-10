// Drives the real nexus::OverlayRules with a seeded random walk and writes the trace as a TLA+ module
// (OverlayTraceData.tla) for models/OverlayTrace.tla, under which TLC checks that the trace is a behaviour of
// models/Overlay.tla.
//
//   overlay_walk <seed> <steps> [--faulty-hide-unkeyed | --faulty-hotkey-hides] <out.tla>
//
// The rules' answers are carried out on a stand-in for the window, as main.cpp's carryOut carries them out through
// Prax: Show makes it visible, gives it the keyboard or not (the OS's answer, drawn from the seed) and arms the key
// check; Hide makes it hidden, without the keyboard, and cancels the check. Between inputs the OS takes the keyboard
// away (Command-Tab, a click on another app) or gives it back (a click on the overlay). Each record holds the input,
// the act the rules answered, and the state after it: visible, active, the rules' keyed() and whether the key check is
// pending.
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from the
// clock or the OS.
//
// --faulty-hide-unkeyed walks rules whose frame hides any shown overlay without the keyboard, so one is hidden before
// the keyboard arrives. --faulty-hotkey-hides walks rules whose hotkey hides any shown overlay, with the keyboard or
// not. TLC must reject each one's traces; that is the negative check that shows the binding can fail.

#include "overlay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

using nexus::OverlayAct;
using nexus::OverlayRules;

enum class Fault { None, HideUnkeyed, HotkeyHides };

// The rules under test, with a fault laid over the real answers when one is asked for.
struct Rules {
    OverlayRules real;
    Fault fault = Fault::None;

    OverlayAct hotkey(bool visible, bool active) {
        OverlayAct a = real.hotkey(visible, active);
        return fault == Fault::HotkeyHides && visible ? OverlayAct::Hide : a;
    }
    OverlayAct escape(bool visible) { return real.escape(visible); }
    OverlayAct observe(bool visible, bool active) {
        OverlayAct a = real.observe(visible, active);
        return fault == Fault::HideUnkeyed && visible && !active ? OverlayAct::Hide : a;
    }
    OverlayAct keyCheck(bool visible, bool active) { return real.keyCheck(visible, active); }
};

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

// The stand-in for the window, which carries out the rules' answers.
struct Window {
    bool visible = false;
    bool active = false;
    bool armed = false;     // the key check's timer

    void carry(OverlayAct a, bool granted) {
        if (a == OverlayAct::Show) {
            visible = true;
            active = granted;
            armed = true;
        } else if (a == OverlayAct::Hide) {
            visible = false;
            active = false;
            armed = false;
        }
    }
};

const char* name(OverlayAct a) {
    switch (a) {
    case OverlayAct::None: return "None";
    case OverlayAct::Show: return "Show";
    case OverlayAct::Hide: return "Hide";
    }
    return "?";
}

const char* tla(bool b) {
    return b ? "TRUE" : "FALSE";
}

std::string record(const char* op, OverlayAct a, const Window& w, bool keyed) {
    return std::string("[op |-> \"") + op + "\", act |-> \"" + name(a) + "\", visible |-> " + tla(w.visible) +
        ", active |-> " + tla(w.active) + ", keyed |-> " + tla(keyed) + ", armed |-> " + tla(w.armed) + "]";
}

int walk(std::uint64_t seed, std::uint64_t steps, Fault fault, const char* path) {
    Rng rng{seed};
    Rules rules;
    rules.fault = fault;
    Window w;
    std::vector<std::string> records{record("init", OverlayAct::None, w, rules.real.keyed())};
    std::size_t shows = 0, hides = 0, refused = 0;
    while (records.size() <= steps) {
        const char* op = nullptr;
        OverlayAct a = OverlayAct::None;
        // The OS answers a show with the keyboard three times in four.
        const bool granted = rng.below(4) != 0;
        switch (rng.below(10)) {
        case 0:
        case 1:
            op = "hotkey";
            a = rules.hotkey(w.visible, w.active);
            break;
        case 2:
            op = "escape";
            a = rules.escape(w.visible);
            break;
        case 3:
        case 4:
        case 5:
            op = "observe";
            a = rules.observe(w.visible, w.active);
            break;
        case 6:
            if (!w.armed)
                continue;
            op = "keycheck";
            w.armed = false;
            a = rules.keyCheck(w.visible, w.active);
            break;
        case 7:
        case 8:
            if (!w.active)
                continue;
            op = "lose";
            w.active = false;
            break;
        default:
            if (!w.visible || w.active)
                continue;
            op = "click";
            w.active = true;
            break;
        }
        w.carry(a, granted);
        shows += a == OverlayAct::Show;
        hides += a == OverlayAct::Hide;
        refused += a == OverlayAct::Show && !granted;
        records.push_back(record(op, a, w, rules.real.keyed()));
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "overlay_walk: cannot write %s\n", path);
        return 1;
    }
    const char* faultName = fault == Fault::HideUnkeyed ? ", faulty: hide unkeyed" :
        fault == Fault::HotkeyHides ? ", faulty: hotkey hides" : "";
    out << "---- MODULE OverlayTraceData ----\n";
    out << "\\* Written by tests/model/overlay_walk: seed " << seed << ", " << steps << " steps" << faultName << ".\n";
    out << "Trace == <<\n";
    for (std::size_t k = 0; k < records.size(); ++k)
        out << "  " << records[k] << (k + 1 < records.size() ? ",\n" : "\n");
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "overlay_walk: writing %s failed\n", path);
        return 1;
    }
    std::printf("overlay_walk: seed %llu, %zu steps%s: %zu shows (%zu without the keyboard), %zu hides -> %s\n",
        static_cast<unsigned long long>(seed), records.size() - 1, faultName, shows, refused, hides, path);
    return 0;
}

bool parse(const char* s, std::uint64_t& v) {
    char* end = nullptr;
    v = std::strtoull(s, &end, 10);
    return end != s && *end == '\0';
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> rest;
    Fault fault = Fault::None;
    int faults = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--faulty-hide-unkeyed") {
            fault = Fault::HideUnkeyed;
            ++faults;
        } else if (a == "--faulty-hotkey-hides") {
            fault = Fault::HotkeyHides;
            ++faults;
        } else {
            rest.push_back(a);
        }
    }
    std::uint64_t seed = 0, steps = 0;
    if (rest.size() != 3 || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) || faults > 1) {
        std::fprintf(stderr, "usage: overlay_walk <seed> <steps> [--faulty-hide-unkeyed | --faulty-hotkey-hides] "
            "<out.tla>\n");
        return 2;
    }
    return walk(seed, steps, fault, rest[2].c_str());
}
