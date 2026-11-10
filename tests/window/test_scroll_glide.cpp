// Unit tests of the sprint board's scrolling (src/scroll_glide.h): the curves (coast, hold, spring, rubber band) as
// pure functions of time, and the scroller over a fake page: a tap against a hold, ignored repeats, the release's
// coast, focus loss, other keys and presses, the ends (bounce and spring back), and a page that scrolls itself. Time is
// an argument: no clock, no OS, no window.

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

bool near(double a, double b, double within) {
    return std::fabs(a - b) <= within;
}

constexpr double kFrame = 1.0 / 120;
constexpr double kLine = 40;

// A page as Prax keeps one: its scroll within its range, in whole px (Prax's are 1/64 px).
struct FakePage final : GlidePage {
    double y = 0;
    double range = 2000;
    double view = 800;
    double top() override { return y; }
    void setTop(double v) override { y = std::round(std::clamp(v, 0.0, range)); }
    double max() override { return range; }
    double height() override { return view; }
    // Prax's view scrolling by `d`, as its keys and wheel do.
    auto by(double d) {
        return [this, d] { setTop(y + d); };
    }
};

// The scroller and its page, with the time of the frame last shown.
struct Rig {
    FakePage page;
    GlideScroller s;
    double now = 10;

    void frame(double dt = kFrame) {
        now += dt;
        s.frame(page, now);
    }
    void frames(double seconds) {
        for (double t = 0; t < seconds; t += kFrame)
            frame();
    }
    // Shown: the scroll and the rubber band past an end.
    double shown() const { return page.y + s.overscroll(); }
    bool down(int arrow, bool repeat = false) {
        return s.keyDown(page, now, arrow, arrow, repeat, page.by(arrow * kLine));
    }
    void up(int arrow) { s.keyUp(page, now, arrow); }
    void key(double d) { s.keyDown(page, now, 0, d > 0 ? 1 : -1, false, page.by(d)); }
    void notch(int lines) { s.wheel(page, now, lines > 0 ? 1 : -1, page.by(lines * kLine)); }
    // Frames until at rest, at most `seconds`.
    void settle(double seconds = 3) {
        for (double t = 0; t < seconds && s.busy(); t += kFrame)
            frame();
    }
};

void theCurves() {
    const double k = coastRate();
    CHECK(near(k, -std::log(0.99) * 1000, 1e-9));
    // A coast: from y0 at its start, on toward its target and never past it, its speed k times the distance left,
    // and that speed decays by kCoastDecayPerMs each millisecond.
    CHECK(coastPosition(100, 140, 0) == 100);
    double last = 100;
    for (double t = 0.001; t < 0.5; t += 0.001) {
        double y = coastPosition(100, 140, t);
        CHECK(y >= last && y <= 140);
        CHECK(near(coastSpeed(100, 140, t), k * (140 - y), 1e-9));
        CHECK(near(coastSpeed(100, 140, t) / coastSpeed(100, 140, t - 0.001), kCoastDecayPerMs, 1e-6));
        last = y;
    }
    CHECK(near(coastPosition(100, 140, 10), 140, 1e-9));
    // A hold: moving at once, its speed rising to the hold's and never past it.
    CHECK(holdPosition(0, 40, 500, 0) == 0);
    CHECK(holdSpeed(0, 40, 500, 0) > 300);
    double speed = 0;
    for (double t = 0; t < 1; t += 0.01) {
        double v = holdSpeed(0, 40, 500, t);
        CHECK(v >= speed - 1e-9 && v <= 500 + 1e-9);
        speed = v;
    }
    CHECK(holdSpeed(0, 40, 500, 3 / k) > 0.95 * 500);
    // A spring: critically damped, from past the end at speed outward, out further, then back, never crossing.
    CHECK(springOffset(5, 300, 0) == 5);
    double peak = 0;
    for (double t = 0; t < kSpringMaxSeconds; t += 0.001) {
        double e = springOffset(5, 300, t);
        CHECK(e >= 0);
        peak = std::max(peak, e);
    }
    CHECK(peak > 5 && springOffset(5, 300, kSpringMaxSeconds) < kSettlePixels);
    CHECK(near(springSpeed(5, 300, 0), 300, 1e-9));
    for (double t = 0; t < 1; t += 0.001)
        CHECK(springOffset(-8, -100, t) <= 0);
    // The rubber band: zero at the end, rising slower than the pull, never as far as the view's height.
    CHECK(rubberBand(0, 800) == 0);
    CHECK(near(rubberBand(1, 800), 0.55, 0.01));
    double band = 0;
    for (double x = 1; x < 100000; x *= 1.5) {
        double b = rubberBand(x, 800);
        CHECK(b > band && b < x && b < 800);
        CHECK(rubberBand(-x, 800) == -b);
        band = b;
    }
}

void aTapMovesExactlyOneLine() {
    Rig r;
    r.page.y = 400;
    CHECK(r.down(1));
    CHECK(r.page.y == 400);         // no jump at the press
    r.frame();
    r.frame();
    CHECK(r.page.y > 400);          // moving at once, at the first frames
    r.frames(0.06);
    r.up(1);
    r.settle();
    CHECK(r.page.y == 440 && !r.s.busy());
    // Two taps: two lines.
    r.down(1);
    r.frames(0.03);
    r.up(1);
    r.frames(0.03);
    r.down(1);
    r.frames(0.03);
    r.up(1);
    r.settle();
    CHECK(r.page.y == 520);
}

void aHoldScrollsWhileHeldAndCoastsToRest() {
    Rig r;
    r.page.y = 0;
    r.down(1);
    std::vector<double> ys;
    for (int i = 0; i < 120; ++i) {         // a second held
        r.frame();
        // The OS's repeats: ignored, and the page never sees them.
        if (i % 4 == 0)
            CHECK(!r.down(1, true));
        ys.push_back(r.page.y);
    }
    for (std::size_t i = 1; i < ys.size(); ++i)
        CHECK(ys[i] >= ys[i - 1]);
    // At the hold's speed after its ramp: 500 px/s, some 4 px a frame.
    const double late = ys[119] - ys[59];
    CHECK(near(late, 250, 15));
    // Held for 1 s: the line, then 500 px/s less the follow's lag.
    CHECK(ys.back() > 400 && ys.back() < 520);
    // Up: it coasts on, at the speed it had, to rest.
    const double atRelease = r.page.y;
    const double speedBefore = glideSpeed(r.s.glide(), r.now);
    r.up(1);
    CHECK(r.s.glide().kind == GlideKind::Coast);
    CHECK(near(glideSpeed(r.s.glide(), r.now), speedBefore, 1e-6));
    CHECK(r.page.y == atRelease);
    r.frames(0.5);
    const double coasted = r.page.y - atRelease;
    CHECK(coasted > 20 && coasted < 80);    // about the speed over k: 500 / 10 px
    r.settle();
    CHECK(!r.s.busy());
}

void focusLossOtherKeysAndPressesEndAHold() {
    // Losing focus: Prax sends the held key's release, which ends the hold as a key-up does.
    {
        Rig r;
        r.down(1);
        r.frames(0.5);
        r.up(1);
        CHECK(!r.s.held());
        r.settle();
        CHECK(!r.s.busy());
    }
    // A release of another key leaves the hold.
    {
        Rig r;
        r.down(1);
        r.frames(0.3);
        r.up(-1);
        CHECK(r.s.held());
    }
    // Another key ends the hold and takes the page on from where the hold was going.
    {
        Rig r;
        r.down(1);
        r.frames(0.5);
        r.key(r.page.view - kLine);
        CHECK(!r.s.held() && r.s.glide().kind == GlideKind::Coast);
        r.settle();
        CHECK(!r.s.busy());
    }
    // The window's own key (a tab's Command-digit): the board ends the hold.
    {
        Rig r;
        r.down(1);
        r.frames(0.5);
        r.s.release(r.now);
        CHECK(!r.s.held());
        r.settle();
        CHECK(!r.s.busy());
    }
    // A press stops it where the page shows.
    {
        Rig r;
        r.down(1);
        r.frames(0.5);
        const double at = r.page.y;
        r.s.interrupt(r.page, r.now);
        CHECK(!r.s.busy() && r.page.y == at);
        r.frames(0.2);
        CHECK(r.page.y == at);
    }
}

void keysAndTheWheelCoastAndAddUp() {
    Rig r;
    r.page.y = 0;
    r.key(r.page.view - kLine);
    r.frame();
    r.frame();
    CHECK(r.page.y > 0);
    // Page Down again mid-coast: on from where the first was going.
    r.key(r.page.view - kLine);
    r.settle();
    CHECK(r.page.y == 2 * (r.page.view - kLine));
    // Three notches: three lines each, added up.
    const double from = r.page.y;
    r.notch(3);
    r.frame();
    r.notch(3);
    r.frame();
    r.notch(3);
    r.settle();
    CHECK(r.page.y == from + 9 * kLine);
    // The other way mid-coast: from where it shows, not from where it was going.
    r.notch(3);
    r.frames(0.05);
    const double shown = r.page.y;
    r.notch(-3);
    r.settle();
    CHECK(r.page.y == shown - 3 * kLine);
}

void theEndsBounceAndSpringBack() {
    // A key at the bottom: the page bounces past it and comes back, the band always past the end.
    {
        Rig r;
        r.page.y = r.page.range;
        r.down(1);
        CHECK(!r.s.held());
        double peak = 0;
        for (int i = 0; i < 240 && r.s.busy(); ++i) {
            r.frame();
            CHECK(r.page.y == r.page.range && r.s.overscroll() >= 0);
            peak = std::max(peak, r.s.overscroll());
        }
        CHECK(peak > 2 && peak < kBumpPixels);
        CHECK(!r.s.busy() && r.s.overscroll() == 0 && r.page.y == r.page.range);
    }
    // At the top, the other way.
    {
        Rig r;
        r.notch(-3);
        double peak = 0;
        for (int i = 0; i < 240 && r.s.busy(); ++i) {
            r.frame();
            CHECK(r.page.y == 0 && r.s.overscroll() <= 0);
            peak = std::min(peak, r.s.overscroll());
        }
        CHECK(peak < -2);
        CHECK(!r.s.busy() && r.s.overscroll() == 0);
    }
    // Held into the bottom: it runs past the end, springs back, and stays parked there while the key is down.
    {
        Rig r;
        r.page.y = r.page.range - 200;
        r.down(1);
        bool sprang = false;
        double lastShown = r.shown();
        for (int i = 0; i < 240; ++i) {
            r.frame();
            CHECK(r.page.y <= r.page.range);
            if (r.s.glide().kind == GlideKind::Spring)
                sprang = true;
            if (!sprang)
                CHECK(r.shown() >= lastShown);
            lastShown = r.shown();
        }
        CHECK(sprang && r.s.held() && r.page.y == r.page.range && r.s.overscroll() == 0);
        r.up(1);
        CHECK(!r.s.held() && !r.s.busy());
    }
    // Let go just before the end: the coast carries it past, and it springs back.
    {
        Rig r;
        r.page.y = r.page.range - 300;
        r.down(1);
        r.frames(0.55);
        CHECK(r.page.y < r.page.range);
        r.up(1);
        r.settle();
        CHECK(r.page.y == r.page.range && r.s.overscroll() == 0);
    }
}

void aPageThatScrollsItselfKeepsItsPlace() {
    Rig r;
    r.down(1);
    r.frames(0.2);
    r.page.y = 1500;                // the page's script
    r.frame();
    CHECK(!r.s.busy() && r.page.y == 1500);
    // And a key before the next frame starts from where the page is.
    Rig q;
    q.notch(3);
    q.frames(0.05);
    q.page.y = 900;
    q.notch(3);
    q.settle();
    CHECK(q.page.y == 1020);
}

void leavingTheScreenEndsWhereItWasGoing() {
    Rig r;
    r.key(r.page.view - kLine);
    r.frame();
    r.s.hide(r.page, r.now);
    CHECK(!r.s.busy() && r.page.y == r.page.view - kLine);
}

} // namespace

int main() {
    theCurves();
    aTapMovesExactlyOneLine();
    aHoldScrollsWhileHeldAndCoastsToRest();
    focusLossOtherKeysAndPressesEndAHold();
    keysAndTheWheelCoastAndAddUp();
    theEndsBounceAndSpringBack();
    aPageThatScrollsItselfKeepsItsPlace();
    leavingTheScreenEndsWhereItWasGoing();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("scroll glide tests passed\n");
    return 0;
}
