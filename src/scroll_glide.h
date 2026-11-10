// The sprint board's smooth scrolling: a glide of the page's scroll from where it shows to where a mouse wheel's notch
// or a scrolling key sends it, as macOS animates those, instead of the jump Prax's HTML view makes (its wheel scrolls
// 40 px per line at once, and its keys a line, a page less a line, or to an end at once). A trackpad's pixel deltas,
// momentum included, already scroll by pixels as they come, and do not glide.
//
// The glide is a cubic from the position shown to the target over a duration: p(s) = a s + (3 - 2a) s^2 + (a - 2) s^3
// for s in [0, 1] of the duration, whose slope at the end is zero. `a` is its slope at the start, in units of the
// distance over the duration: 3 is ease-out (1 - (1 - s)^3), 0 is ease-in-out, and every a in [0, 3] moves one way
// only, so the glide never passes its target. A fresh key press eases out from where the page shows. A notch of the
// wheel or a held key's repeat during a glide keeps the speed it is moving at (a from the speed now), so a held key
// moves at an even speed with no pulse at each repeat.
//
// Pure: time is an argument, in seconds, and nothing here reads a clock or the page.

#pragma once

namespace nexus {

// How long a glide takes, in seconds: a wheel's notch, and a key (macOS animates a key's scroll a little longer).
constexpr double kGlideWheelSeconds = 0.12;
constexpr double kGlideKeySeconds = 0.18;

struct Glide {
    double from = 0;            // CSS px from the top
    double to = 0;
    double start = 0;           // s; unset (`pending`) until the first frame after it began
    double duration = 0;
    double slope = 3;           // a, the slope at the start, in [0, 3]
    bool active = false;
    bool pending = false;       // begun since the last frame: it starts at the next frame's time
};

// The cubic at s (clamped to [0, 1]) for slope a, and its derivative in s.
double glideCurve(double s, double a);
double glideCurveSlope(double s, double a);

// Where the glide shows at `now`, clamped to [0, max]; its speed then, in px/s. A pending glide is at its start, and
// moves at its starting speed.
double glidePosition(const Glide& g, double now, double max);
double glideSpeed(const Glide& g, double now);

// True once the glide has reached its target at `now`.
bool glideDone(const Glide& g, double now);

// How a new target is taken: a fresh key press eases out from the position shown; a wheel's notch or a held key's
// repeat keeps the speed the glide moves at.
enum class GlideKind {
    Press,
    Continue,
};

// A glide to `target` (clamped to [0, max]) from where `g` shows at `now`, or from `at` when `g` is not gliding.
// A glide that has not shown a frame yet keeps its start and its ease, with the new target. A glide that moves starts
// at `now` (the time of the frame last shown) from the position it shows then. The result is inactive when the target
// is where it starts.
Glide glideTo(const Glide& g, double now, double at, double target, double max, double duration, GlideKind kind);

// Sets a pending glide's start to the frame's time `now`.
Glide glideFrame(const Glide& g, double now);

} // namespace nexus
