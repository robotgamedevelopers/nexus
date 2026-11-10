// Unit tests of the sprint board's scroll glide (src/scroll_glide.h): the ease, a glide from where the page shows,
// a new target in the middle of a glide, the ends of the page, and a held key's repeats. Time is an argument: no clock,
// no OS, no window.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "scroll_glide.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

bool near(double a, double b, double within = 1e-9) {
    return std::fabs(a - b) <= within;
}

constexpr double kMax = 5000;       // how far the page scrolls, px
constexpr double kFrame = 1.0 / 120;

void theEaseIsEaseOutAndNeverPassesItsTarget() {
    for (double s = 0; s <= 1; s += 1.0 / 64)
        CHECK(near(glideCurve(s, 3), 1 - std::pow(1 - s, 3)));
    for (double a : {0.0, 0.5, 1.5, 3.0}) {
        CHECK(near(glideCurve(0, a), 0) && near(glideCurve(1, a), 1));
        CHECK(near(glideCurve(-1, a), 0) && near(glideCurve(2, a), 1));
        CHECK(near(glideCurveSlope(0, a), a));
        CHECK(near(glideCurveSlope(1 - 1e-9, a), 0, 1e-6));
        double last = 0;
        for (double s = 0; s <= 1; s += 1.0 / 256) {
            double p = glideCurve(s, a);
            CHECK(p >= last - 1e-12 && p <= 1 + 1e-12);
            last = p;
        }
    }
    // Ease-out: most of the way early. A quarter of the time covers more than half the distance.
    CHECK(glideCurve(0.25, 3) > 0.5);
}

void aPressGlidesFromWhereThePageShows() {
    Glide g = glideTo(Glide{}, 10, 200, 240, kMax, kGlideKeySeconds, GlideKind::Press);
    CHECK(g.active && g.pending && g.from == 200 && g.to == 240);
    // Until the next frame it stays where it shows.
    CHECK(glidePosition(g, 10, kMax) == 200);
    g = glideFrame(g, 10.016);
    CHECK(!g.pending && g.start == 10.016);
    CHECK(glidePosition(g, 10.016, kMax) == 200);
    double half = glidePosition(g, 10.016 + kGlideKeySeconds / 2, kMax);
    CHECK(near(half, 200 + 40 * (1 - std::pow(0.5, 3))));
    CHECK(!glideDone(g, 10.016 + kGlideKeySeconds / 2));
    CHECK(glidePosition(g, 10.016 + kGlideKeySeconds, kMax) == 240);
    CHECK(glideDone(g, 10.016 + kGlideKeySeconds + 1e-9));
    CHECK(glidePosition(g, 99, kMax) == 240);
    // Its frames move one way, by less each frame.
    double last = 200;
    double step = 1e9;
    for (double t = 10.016 + kFrame; t <= 10.016 + kGlideKeySeconds; t += kFrame) {
        double y = glidePosition(g, t, kMax);
        CHECK(y >= last && y - last <= step + 1e-9);
        step = y - last;
        last = y;
    }
    // Nowhere to go: no glide.
    CHECK(!glideTo(Glide{}, 10, 200, 200, kMax, kGlideKeySeconds, GlideKind::Press).active);
}

void aNewTargetGlidesOnFromWhereThePageShowsNow() {
    Glide g = glideFrame(glideTo(Glide{}, 0, 0, 400, kMax, kGlideKeySeconds, GlideKind::Press), 0);
    const double now = kGlideKeySeconds / 3;
    const double shown = glidePosition(g, now, kMax);
    CHECK(shown > 0 && shown < 400);
    // Down again: from where it shows, never back to where the first began, and on to the new target.
    Glide pressed = glideTo(g, now, 0, 800, kMax, kGlideKeySeconds, GlideKind::Press);
    CHECK(!pressed.pending && pressed.start == now);
    CHECK(near(glidePosition(pressed, now, kMax), shown));
    CHECK(glidePosition(pressed, now + kFrame, kMax) > shown);
    CHECK(glidePosition(pressed, now + kGlideKeySeconds, kMax) == 800);
    // A fresh press eases out: at least the speed it moved at.
    CHECK(glideSpeed(pressed, now) >= glideSpeed(g, now));
    // A repeat keeps the speed it moves at.
    Glide repeated = glideTo(g, now, 0, 800, kMax, kGlideKeySeconds, GlideKind::Continue);
    CHECK(near(glidePosition(repeated, now, kMax), shown));
    CHECK(near(glideSpeed(repeated, now), glideSpeed(g, now), 1e-6));
    // Up instead: it turns where it shows and glides back, without passing the new target.
    Glide back = glideTo(g, now, 0, 0, kMax, kGlideKeySeconds, GlideKind::Continue);
    CHECK(near(glidePosition(back, now, kMax), shown));
    double last = shown;
    for (double t = now; t <= now + kGlideKeySeconds; t += kFrame) {
        double y = glidePosition(back, t, kMax);
        CHECK(y <= last + 1e-9 && y >= 0);
        last = y;
    }
    CHECK(glidePosition(back, now + kGlideKeySeconds, kMax) == 0);
    // Two notches before a frame shows: one glide from where the page shows, to the second target.
    Glide two = glideTo(Glide{}, 5, 100, 140, kMax, kGlideWheelSeconds, GlideKind::Continue);
    two = glideTo(two, 5, 140, 180, kMax, kGlideWheelSeconds, GlideKind::Continue);
    CHECK(two.pending && two.from == 100 && two.to == 180 && two.slope == 3);
}

void itStopsAtBothEnds() {
    Glide down = glideTo(Glide{}, 0, kMax - 10, kMax + 400, kMax, kGlideKeySeconds, GlideKind::Press);
    CHECK(down.to == kMax);
    down = glideFrame(down, 0);
    for (double t = 0; t <= 1; t += kFrame)
        CHECK(glidePosition(down, t, kMax) <= kMax);
    Glide up = glideTo(Glide{}, 0, 10, -400, kMax, kGlideKeySeconds, GlideKind::Press);
    CHECK(up.to == 0);
    up = glideFrame(up, 0);
    for (double t = 0; t <= 1; t += kFrame)
        CHECK(glidePosition(up, t, kMax) >= 0);
    // At an end already: no glide.
    CHECK(!glideTo(Glide{}, 0, kMax, kMax + 40, kMax, kGlideKeySeconds, GlideKind::Press).active);
    CHECK(!glideTo(Glide{}, 0, 0, -40, kMax, kGlideKeySeconds, GlideKind::Press).active);
    // The page grew shorter mid-glide: the glide shows within it.
    Glide g = glideFrame(glideTo(Glide{}, 0, 0, 1000, kMax, kGlideKeySeconds, GlideKind::Press), 0);
    CHECK(glidePosition(g, kGlideKeySeconds, 600) == 600);
    CHECK(glidePosition(g, kGlideKeySeconds, -5) == 0);
}

// A held key repeating every `repeat` s, a line (40 px) each, at 120 frames a second: after the first half second, the
// page moves at an even speed, every frame's step within 20% of every other's (the repeats land 9 or 10 frames apart,
// which is most of what remains).
double heldKeyUnevenness(double repeat, double duration) {
    Glide g;
    double next = 0;
    double target = 0;
    double last = 0;
    std::vector<double> steps;
    for (int f = 0; f < 240; ++f) {
        double now = f * kFrame;
        bool first = true;
        while (next <= now) {
            target += 40;
            // The event comes between frames: the glide's time is the frame last shown.
            double shownAt = f == 0 ? 0 : (f - 1) * kFrame;
            g = glideTo(g, shownAt, 0, target, kMax, duration, first && !g.active ? GlideKind::Press : GlideKind::Continue);
            first = false;
            next += repeat;
        }
        g = glideFrame(g, now);
        double y = glidePosition(g, now, kMax);
        if (f >= 60)
            steps.push_back(y - last);
        last = y;
    }
    auto [lo, hi] = std::minmax_element(steps.begin(), steps.end());
    return *lo > 0 ? *hi / *lo : 1e9;
}

void aHeldKeyMovesEvenly() {
    for (double repeat : {0.03, 0.05, 1.0 / 12}) {
        double uneven = heldKeyUnevenness(repeat, kGlideKeySeconds);
        if (uneven > 1.2)
            std::fprintf(stderr, "repeat %.3f s: steps vary by %.3fx\n", repeat, uneven);
        CHECK(uneven <= 1.2);
    }
    // A wheel spun steadily, a notch every 30 ms, glides as evenly.
    CHECK(heldKeyUnevenness(0.03, kGlideWheelSeconds) <= 1.2);
    // Eased out afresh at each repeat instead, the page would pulse: at macOS's default repeat (83 ms), fast then
    // nearly still between repeats.
    {
        Glide g;
        double prev = 0;
        double lo = 1e9, hi = 0;
        double target = 0;
        for (int f = 0; f < 240; ++f) {
            double now = f * kFrame;
            if (f % 10 == 0) {
                target += 40;
                g = glideTo(g, f == 0 ? 0 : now - kFrame, 0, target, kMax, kGlideKeySeconds, GlideKind::Press);
            }
            g = glideFrame(g, now);
            double y = glidePosition(g, now, kMax);
            if (f >= 60) {
                lo = std::min(lo, y - prev);
                hi = std::max(hi, y - prev);
            }
            prev = y;
        }
        CHECK(hi / lo > 2);
    }
}

} // namespace

int main() {
    theEaseIsEaseOutAndNeverPassesItsTarget();
    aPressGlidesFromWhereThePageShows();
    aNewTargetGlidesOnFromWhereThePageShowsNow();
    itStopsAtBothEnds();
    aHeldKeyMovesEvenly();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("scroll glide tests passed\n");
    return 0;
}
