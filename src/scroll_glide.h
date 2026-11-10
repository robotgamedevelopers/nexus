// The sprint board's smooth scrolling, as Safari scrolls on macOS, where Prax's HTML view jumps: its mouse wheel scrolls
// 40 px a line at once, and its keys a line, a page less a line, or to an end at once (html_view.cpp 1378, 614, 908).
//
// The page's scroll is a motion over time in CSS px, one of:
//   Coast   momentum toward a target: the speed is k times the distance left, so it decays by kCoastDecayPerMs each
//           millisecond and the page comes to rest on the target: y(t) = T + (y0 - T) e^(-kt). A key, a wheel's notch,
//           a key let go after a hold. A new target starts a coast from where the page shows.
//   Hold    Up or Down held: the target stands a line past where the key went down through kHoldTapSeconds, then
//           runs ahead at kHoldPixelsPerSecond, and the page follows it as a coast does: it starts at once toward the
//           line, reaches the hold's speed some 1/k after the target starts running, and when the key comes up coasts
//           on to rest at the same speed it had (a coast is a hold whose target stopped). Released within
//           kHoldTapSeconds, the key was a tap: the target never ran, and the page comes to rest exactly one line on.
//           (At 1000 px/s a target running from the press would carry the page past the line within a tap.)
//   Spring  past an end: a critically damped spring pulls the page back to the end, from how far past it is and how
//           fast it moves, without ever crossing back inside: e(t) = (e0 + (v0 + w e0) t) e^(-wt).
// A coast or a hold that carries the page past an end becomes a spring there. A key or a notch at an end aims
// kBumpPixels past it, so the page bounces as Safari's does. Past an end the page is drawn rubber-banded: Apple's
// curve, (1 - 1 / (x c / d + 1)) d, of how far past it is, x, over the view's height d.
//
// A trackpad's pixels and momentum come from macOS through the deltas and scroll at once; they take the page from
// wherever it shows. Rubber-banding a trackpad needs the gesture's phases, which Prax does not report.
//
// Pure: time is an argument, in seconds; nothing here reads a clock. GlideScroller acts on the page through GlidePage.

#pragma once

#include <functional>

namespace nexus {

// --- Tuning ---------------------------------------------------------------------------------------------------------

// The speed a coast keeps each millisecond. UIScrollView's fast rate; its normal rate, 0.998, is for a finger's fling,
// and coasts a key let go at 1000 px/s some 500 px on, over a second and a half.
constexpr double kCoastDecayPerMs = 0.99;
// A held Up or Down: a line of Prax's 40 px each 20 ms (the owner 2026-11-10: "Scroll on up/down is too slow. double
// it", 500 to 1000; then "still too slow", 1000 to 2000). The ramp and the ease to rest keep their times, as
// kCoastDecayPerMs sets them.
constexpr double kHoldPixelsPerSecond = 2000;
// A key down for less than this is a tap: exactly one line. Shorter than macOS's repeat delay so a hold starts at once.
constexpr double kHoldTapSeconds = 0.08;
// The spring back from an end: critically damped, at this angular frequency (1/s).
constexpr double kSpringOmega = 16;
// How far past an end a key or a notch there aims, to bounce.
constexpr double kBumpPixels = 24;
// Apple's rubber-band coefficient.
constexpr double kRubberBandCoefficient = 0.55;
// At rest: within kSettlePixels of where it is going, and slower than kSettleSpeed (px/s).
constexpr double kSettlePixels = 0.5;
constexpr double kSettleSpeed = 10;
// The longest a coast or a spring runs before it comes to rest where it is going.
constexpr double kCoastMaxSeconds = 2;
constexpr double kSpringMaxSeconds = 1;

// --- The curves -----------------------------------------------------------------------------------------------------

// k, the coast's rate per second: -ln(kCoastDecayPerMs) * 1000.
double coastRate();
// A coast from y0 toward `target`, `t` s after it began: where it is and how fast it moves (px/s).
double coastPosition(double y0, double target, double t);
double coastSpeed(double y0, double target, double t);
// A hold from y0 whose target stands at `base` (the first line) through kHoldTapSeconds, and then runs at `speed`
// px/s (signed): where the target is, where the page is and how fast it moves.
double holdGoal(double base, double speed, double t);
double holdPosition(double y0, double base, double speed, double t);
double holdSpeed(double y0, double base, double speed, double t);
// A critically damped spring from e0 past its rest at speed v0: how far past rest, and how fast.
double springOffset(double e0, double v0, double t);
double springSpeed(double e0, double v0, double t);
// Apple's rubber band: how far past an end the page is drawn when it is `x` (signed) past it, in a view `d` high.
double rubberBand(double x, double d);

// --- The motion -----------------------------------------------------------------------------------------------------

enum class GlideKind {
    Idle,
    Coast,
    Hold,
    Spring,
};

struct Glide {
    GlideKind kind = GlideKind::Idle;
    double y0 = 0;          // where it began, CSS px from the top; past an end while it is outside the range
    double target = 0;      // a coast's target; a hold's at its start; a spring's end (0 or the range's bottom)
    double speed = 0;       // a hold's speed (signed); a spring's speed at its start
    double start = 0;       // s; unset while pending
    bool pending = false;   // begun since the last frame: it starts at the next frame's time
    int dir = 0;            // which way a coast or a hold moves: -1 up, 1 down
};

// Where the motion has the page at `now`, and how fast it moves. A pending motion is at its start.
double glidePosition(const Glide& g, double now);
double glideSpeed(const Glide& g, double now);

// --- The scroller ---------------------------------------------------------------------------------------------------

// The page, as the scroller sees it: its scroll (which the page keeps within its range, to whole units of its own),
// how far it can scroll, and its view's height.
class GlidePage {
public:
    virtual ~GlidePage() = default;
    virtual double top() = 0;
    virtual void setTop(double y) = 0;
    virtual double max() = 0;
    virtual double height() = 0;
};

// The board's scrolling: which events move the page how, and the page's place each frame. Each call takes the time
// of the frame last shown (`now`), and an event's `apply` gives the event to Prax's view, which may move the page's
// scroll: the scroller reads where it went, puts it back where the page shows, and moves there over the next frames.
// Prax's view so keeps deciding what a key does (its listeners, a form control, a line, a page, an end).
class GlideScroller {
public:
    // A key's press. `arrow` is -1 or 1 for Up or Down with no Command, Control or Option, else 0; `dir` is which way
    // the key scrolls (0 for a key that does not). Returns false for an arrow's repeat, which the page must not see:
    // a held arrow scrolls by itself. Any other key ends a hold first.
    bool keyDown(GlidePage& page, double now, int arrow, int dir, bool repeat, const std::function<void()>& apply);
    // A key's release (Prax sends one for each key held when the window loses focus). The held arrow's ends its hold.
    void keyUp(GlidePage& page, double now, int arrow);
    // A wheel's notch, `dir` the way it scrolls.
    void wheel(GlidePage& page, double now, int dir, const std::function<void()>& apply);
    // A trackpad's scroll or a press: the motion and any hold stop where the page shows, before the event.
    void interrupt(GlidePage& page, double now);
    // The page leaves the screen: the motion ends where it was going, and any hold ends.
    void hide(GlidePage& page, double now);
    // Each frame: moves the page to where the motion has it. Returns true if it set the page's scroll.
    bool frame(GlidePage& page, double now);
    // A held arrow's hold ends as its key's release ends it: a tap comes to rest one line on, a hold coasts to rest.
    // Another key, a press, the window's own keys and the window losing focus all end it.
    void release(double now);

    const Glide& glide() const { return glide_; }
    bool held() const { return held_; }
    bool busy() const { return glide_.kind != GlideKind::Idle || held_; }
    // How far past an end the page is drawn, CSS px: positive past the bottom, negative past the top.
    double overscroll() const { return over_; }
    // The scroll the scroller set last.
    double applied() const { return applied_; }

protected:
    // Protected, not private: the trace walk's faulty scrollers (tests/model/glide_walk.cpp) break these on purpose.
    bool stale(GlidePage& page) const;
    void drop();
    void show(GlidePage& page, double y);
    // Gives the event to the page from where the motion is going (or from where it shows, for an event the other way)
    // and starts the coast to where it went; or, at an end, a bump past it. Returns true if the page's scroll moved,
    // with where it went in `*landed`.
    bool through(GlidePage& page, double now, int dir, const std::function<void()>& apply, double* landed);
    void coastTo(double now, double y, double target, int dir);

    Glide glide_;
    double applied_ = 0;
    double over_ = 0;
    bool held_ = false;         // an arrow is down and its hold has not ended (it may be parked at an end)
    int heldDir_ = 0;
};

} // namespace nexus
