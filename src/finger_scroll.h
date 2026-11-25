// A list a finger scrolls, as UIScrollView does (the iPhone's menu of AIs, docs/voice.md): the content follows the
// finger, past an end rubber-banded; let go moving, it flings on and slows (UIScrollView's normal deceleration); a fling
// that reaches an end, or a release past one, springs back to that end. A press that does not move past kSlopPixels is
// a tap, for the row under it; one that does is a drag, and no row takes it.
//
// The motion, one of:
//   Rest    still, within the range [0, max].
//   Track   a finger is down and has not yet moved kSlopPixels: maybe a tap.
//   Drag    the finger moves the content, offset = where it began - how far the finger went, rubber-banded past an end.
//   Fling   let go at speed v0 inside the range: v(t) = v0 d^(1000 t), so the content goes v0 (d^(1000 t) - 1) / ln(d^1000)
//           further, and comes to rest when slower than kSettleSpeed. Reaching an end, it springs there.
//   Spring  past an end: scroll_glide.h's critically damped spring back to the end, never crossing back inside.
// A press stops a fling or a spring where it shows (as a finger on a moving UIScrollView catches it).
//
// Pure: time is an argument, the events' own (seconds); nothing here reads a clock. The model is
// models/FingerScroll.tla, checked against this code's traces (tests/voice/finger_walk.cpp).
#pragma once

#include <array>
#include <cstddef>

namespace nexus {

// A press that moves less than this is a tap (UIKit's touch slop is some 10 pt).
constexpr double kSlopPixels = 10;
// UIScrollView's normal deceleration rate: the speed kept each millisecond.
constexpr double kFlingDecayPerMs = 0.998;
// Slower than this at release does not fling; slower than this in a fling is at rest (px/s).
constexpr double kFlingMinSpeed = 50;
// The finger's speed at release is measured over its last moves within this window (s).
constexpr double kVelocityWindow = 0.1;

enum class FingerPhase { Rest, Track, Drag, Fling, Spring };

const char* fingerPhaseName(FingerPhase phase);

class FingerScroll {
public:
    // The range: how far the content can scroll (its height less the view's, at least 0), and the view's height (for
    // the rubber band). At rest a shorter range pulls the offset in at once.
    void setRange(double max, double height);
    // The finger: down at `y` (view px, down the screen), moved, lifted or cancelled. `t` is the event's own time (its
    // moves' times give its speed), `now` the frame's (the motion's clock, as frame() takes it). up() returns true when
    // the press was a tap (never moved kSlopPixels): the row under it may take it.
    void down(double y, double t, double now);
    void move(double y, double t);
    bool up(double t, double now);
    void cancel(double now);
    // Each frame: moves a fling or a spring to `now`.
    void frame(double now);

    FingerPhase phase() const { return phase_; }
    // How far the content is scrolled, as drawn: past an end while dragging (rubber-banded) or springing back.
    double offset() const { return offset_; }
    double max() const { return max_; }
    bool moving() const { return phase_ == FingerPhase::Fling || phase_ == FingerPhase::Spring; }
    // A finger is down and moved past the slop: what it is over must not take the press.
    bool dragging() const { return phase_ == FingerPhase::Drag; }

private:
    double raw(double y) const;     // the offset the finger asks for, before the rubber band
    double banded(double r) const;  // the offset drawn for a raw offset
    void release(double v, double t);
    void springFrom(double end, double v, double t);
    double speedAt(double t) const;

    FingerPhase phase_ = FingerPhase::Rest;
    double offset_ = 0;
    double max_ = 0;
    double height_ = 1;
    // Track and Drag: where the finger and the offset were when the drag began (raw, unbanded).
    double y0_ = 0, from_ = 0;
    // The finger's last moves, for its speed at release: a ring of (t, y).
    std::array<double, 8> times_{}, ys_{};
    std::size_t samples_ = 0;
    // Fling and Spring: where and when the motion began, its speed then, and a spring's end.
    double start_ = 0, p0_ = 0, v0_ = 0, end_ = 0;
};

} // namespace nexus
