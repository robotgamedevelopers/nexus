// The sprint board's held Up and Down keys: while one is down the page scrolls every frame, at a steady speed, from the
// frame after the press until the release. The key's down state drives the scroll, not the system's key repeats: there
// is no wait for the first repeat, and a repeat changes nothing. A tap scrolls as far as the key was down for.
//
// The state is which of the two keys is down. Up alone scrolls up, Down alone scrolls down, both or neither stand
// still; so a reversal follows the key left down once the other is let go. A change of direction starts a short
// ease-in, from half the speed to the whole over kHoldEaseSeconds, so the page does not jolt. The window losing the keyboard, or the
// sprint tab leaving the screen, lets go of both. models/scroll/ScrollHold.tla is this state machine, and
// tests/window/hold_walk.cpp binds this code to it by trace checking.
//
// Pure: time is an argument, in seconds, and nothing here reads a clock or the page. The page's position, its clamp to
// the scroll range and the speed's text scale are the board's (board.cpp stepHeld).

#pragma once

namespace nexus {

// The speed of a held key, in the window's px per second (the board divides it by the text scale, as it does a
// trackpad's pixels, so the page moves as fast on screen at every scale). Unmeasured: a first guess, to be tuned by
// eye.
constexpr double kHoldSpeed = 900;

// The ease-in at a change of direction: the speed starts at kHoldEaseStart of kHoldSpeed and reaches all of it after
// kHoldEaseSeconds, linearly. Unmeasured; under the 50 ms the design allows.
constexpr double kHoldEaseSeconds = 0.04;
constexpr double kHoldEaseStart = 0.5;

enum class HoldKey {
    Up,
    Down,
};

struct Hold {
    bool up = false;            // Up is down
    bool down = false;          // Down is down
    double since = 0;           // s: when the direction last changed, the ease's start
    double stepped = 0;         // s: the time the hold has moved the page to
};

// +1 while Down alone is down, -1 while Up alone is, 0 otherwise.
int holdDirection(const Hold& h);

// A key goes down (not a repeat) or up, at `now`: the time of the frame last shown, from which the next frame moves the
// page. A change of direction restarts the ease.
Hold holdPress(const Hold& h, HoldKey key, double now);
Hold holdRelease(const Hold& h, HoldKey key, double now);

// A key repeat: nothing changes. It is a function so that the board, the tests and the model's trace all take a repeat
// through the same place.
Hold holdRepeat(const Hold& h, HoldKey key);

// The window lost the keyboard, or the page left the screen: both keys let go.
Hold holdBlur(const Hold& h);

// How far a held key moves from the start of its direction, in px, `elapsed` seconds in, at `speed` px/s: the integral
// of the eased speed.
double holdDistance(double elapsed, double speed);

// The signed distance in px the hold moves the page from `stepped` to `now` (a frame's time), at `speed` px/s; the hold
// is then at `now`. Zero when no direction is held.
double holdStep(Hold* h, double now, double speed);

// The hold stands still until `now` without moving the page (a glide owns the scroll meanwhile): no distance is owed
// for the time skipped.
Hold holdSkip(const Hold& h, double now);

} // namespace nexus
