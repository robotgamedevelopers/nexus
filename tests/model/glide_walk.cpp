// Drives the real nexus::GlideScroller (src/scroll_glide.h) with a seeded random walk and writes the trace as a TLA+
// module (GlideTraceData.tla) for models/GlideTrace.tla, under which TLC checks that the trace is a behaviour of
// models/Glide.tla.
//
//   glide_walk <seed> <steps> [--faulty-keyup] <out.tla>
//
// The page is a fake one in whole px, as Prax keeps its scroll in whole units of its own: 2000 px of scroll in a view
// 800 px high; its keys and notches scroll by a line (40 px), a page less a line, to an end, or three lines. The walk is
// a pure function of its arguments: its random numbers come from the seed (splitmix64), and its time is whole
// milliseconds it counts itself, never the clock's. Each step is one of: a frame (4, 8, 16 or 33 ms on), an arrow
// pressed, its OS repeat, its release (or the window losing focus, which Prax reports as the release), another key, a
// notch, a trackpad's scroll, a press of the pointer, the page's own scroll, or the page leaving the screen. Each
// record holds the step and the scroller's state after it.
//
// --faulty-keyup walks FaultyKeyUpScroller instead: a test-only scroller that loses the arrow's release, so its hold
// scrolls on. TLC must reject its traces; that is the negative check that shows the binding can fail.

#include "scroll_glide.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace nexus;

constexpr double kMax = 2000;
constexpr double kView = 800;
constexpr double kLine = 40;
constexpr int kDts[] = {4, 8, 16, 33};
constexpr int kKeyDeltas[] = {-2000, -760, 760, 2000};
constexpr int kWheelDeltas[] = {-120, 120};
constexpr int kPadDeltas[] = {-37, -10, 10, 37};
constexpr int kSpots[] = {0, 500, 1000, 1500, 2000};

struct Page final : GlidePage {
    double y = 1000;
    double top() override { return y; }
    void setTop(double v) override { y = std::round(std::clamp(v, 0.0, kMax)); }
    double max() override { return kMax; }
    double height() override { return kView; }
};

// The test-only fault: the arrow's release is lost. Hides GlideScroller::keyUp; the walk is a template.
class FaultyKeyUpScroller : public GlideScroller {
public:
    void keyUp(GlidePage&, double, int) {}
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
    template <typename T, std::size_t N>
    T pick(const T (&a)[N]) { return a[below(int(N))]; }
};

const char* modeName(GlideKind k) {
    switch (k) {
        case GlideKind::Coast: return "coast";
        case GlideKind::Hold: return "hold";
        case GlideKind::Spring: return "spring";
        case GlideKind::Idle: break;
    }
    return "idle";
}

// How many ms a coast's or a spring's cap has left at `now`.
long capLeft(const Glide& g, double now) {
    const double cap = g.kind == GlideKind::Spring ? kSpringMaxSeconds : g.kind == GlideKind::Coast ? kCoastMaxSeconds : 0;
    if (cap == 0)
        return 0;
    if (g.pending)
        return std::lround(cap * 1000);
    return std::lround((g.start + cap - now) * 1000);
}

struct Step {
    const char* op;
    int d = 0;
    int s = 0;
    int dt = 0;
    int y = 0;
};

template <typename Scroller>
std::string walk(uint64_t seed, int steps) {
    Rng rng{seed};
    Page page;
    Scroller sc;
    long ms = 1000;
    bool pressed = false;
    int arrow = 0;
    std::ostringstream out;
    auto record = [&](const Step& st) {
        const double now = double(ms) / 1000;
        const Glide& g = sc.glide();
        out << "  [op |-> \"" << st.op << "\", d |-> " << st.d << ", s |-> " << st.s << ", dt |-> " << st.dt
            << ", y |-> " << st.y << ", pos |-> " << std::lround(page.y) << ", over |-> "
            << std::lround(sc.overscroll()) << ", mode |-> \"" << modeName(g.kind) << "\", pending |-> "
            << (g.pending ? "TRUE" : "FALSE") << ", held |-> " << (sc.held() ? "TRUE" : "FALSE") << ", target |-> "
            << std::lround(g.target) << ", left |-> " << capLeft(g, now) << ", applied |-> "
            << std::lround(sc.applied()) << "]";
    };
    record(Step{"init"});
    for (int i = 0; i < steps; ++i) {
        out << ",\n";
        const double now = double(ms) / 1000;
        const int r = rng.below(100);
        Step st{"frame"};
        if (r < 50) {
            st.dt = rng.pick(kDts);
            ms += st.dt;
            sc.frame(page, double(ms) / 1000);
        } else if (r < 58) {
            if (!pressed) {
                st.op = "press";
                arrow = rng.below(2) == 0 ? -1 : 1;
                st.s = arrow;
                pressed = true;
                sc.keyDown(page, now, arrow, arrow, false, [&] { page.setTop(page.y + arrow * kLine); });
            } else if (rng.below(2) == 0) {
                st.op = "repeat";
                sc.keyDown(page, now, arrow, arrow, true, [&] { page.setTop(page.y + arrow * kLine); });
            } else {
                st.op = "release";
                pressed = false;
                sc.keyUp(page, now, arrow);
            }
        } else if (r < 66) {
            st.op = "key";
            st.d = rng.pick(kKeyDeltas);
            sc.keyDown(page, now, 0, st.d > 0 ? 1 : -1, false, [&] { page.setTop(page.y + st.d); });
        } else if (r < 76) {
            st.op = "wheel";
            st.d = rng.pick(kWheelDeltas);
            sc.wheel(page, now, st.d > 0 ? 1 : -1, [&] { page.setTop(page.y + st.d); });
        } else if (r < 81) {
            st.op = "pad";
            st.d = rng.pick(kPadDeltas);
            sc.interrupt(page, now);
            page.setTop(page.y + st.d);
        } else if (r < 83) {
            st.op = "click";
            sc.interrupt(page, now);
        } else if (r < 87) {
            st.op = "page";
            st.y = rng.pick(kSpots);
            if (std::fabs(st.y - page.y) < 2) {
                st.op = "frame";
                st.y = 0;
                st.dt = rng.pick(kDts);
                ms += st.dt;
                sc.frame(page, double(ms) / 1000);
            } else {
                page.y = st.y;
            }
        } else if (r < 89) {
            st.op = "hide";
            sc.hide(page, now);
        } else {
            st.dt = rng.pick(kDts);
            ms += st.dt;
            sc.frame(page, double(ms) / 1000);
        }
        record(st);
    }
    return out.str();
}

template <std::size_t N>
std::string set(const int (&a)[N]) {
    std::string s = "{";
    for (std::size_t i = 0; i < N; ++i)
        s += (i != 0 ? ", " : "") + std::to_string(a[i]);
    return s + "}";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: glide_walk <seed> <steps> [--faulty-keyup] <out.tla>\n");
        return 2;
    }
    const uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const int steps = std::atoi(argv[2]);
    bool faulty = false;
    for (int i = 3; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--faulty-keyup") {
            faulty = true;
        } else {
            std::fprintf(stderr, "glide_walk: unknown option %s; the one option is --faulty-keyup\n", argv[i]);
            return 2;
        }
    }
    const std::string trace = faulty ? walk<FaultyKeyUpScroller>(seed, steps) : walk<GlideScroller>(seed, steps);
    std::ofstream file(argv[argc - 1]);
    if (!file) {
        std::fprintf(stderr, "glide_walk: cannot write %s; check that its directory exists and is writable\n",
            argv[argc - 1]);
        return 1;
    }
    file << "---- MODULE GlideTraceData ----\nEXTENDS Integers, Sequences\n"
         << "TraceMax == " << int(kMax) << "\nTraceBound == " << int(kView) << "\nTraceLine == " << int(kLine)
         << "\nTraceBump == " << int(kBumpPixels) << "\nTraceKeyDeltas == " << set(kKeyDeltas)
         << "\nTraceWheelDeltas == " << set(kWheelDeltas) << "\nTracePadDeltas == " << set(kPadDeltas)
         << "\nTraceSpots == " << set(kSpots) << "\nTraceDts == " << set(kDts)
         << "\nTraceCoastTicks == " << std::lround(kCoastMaxSeconds * 1000)
         << "\nTraceSpringTicks == " << std::lround(kSpringMaxSeconds * 1000)
         << "\nTrace == <<\n" << trace << "\n>>\n====\n";
    std::printf("glide_walk: seed %llu, %d steps%s\n", static_cast<unsigned long long>(seed), steps,
        faulty ? ", faulty key-up" : "");
    return 0;
}
