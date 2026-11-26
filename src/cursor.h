// The state cursor of an AI's tab: its own line under the last text of the transcript, showing what the AI of
// the session is doing, from the server's state only (AGENTS.md "The state cursor"; sync/client.h cursorOf). Its
// colours are OKLCH, and the busy one breathes: its lightness rises and falls once every kBreathSeconds. Pure: the
// time is an argument, so the breathe is tested without a clock; view.cpp turns the colour into the display's.

#pragma once

#include <cstdint>

namespace nexus {

enum class CursorState : std::uint8_t {
    None,   // no cursor: the window shows no server (no --server), or the tab is not an AI's
    Busy,   // a turn is open, starting or stopping: pulsing light blue
    Working, // the harness is waiting for one or more tools
    Idle,   // waiting for the person: steady dark blue
    Down,   // the AI is down (its driver crashed, keeps failing to start, or has no login): steady red
    Lost,   // the window has lost the server: dim grey, and never blue, since what it holds is stale
};

// One breath of the busy cursor, in seconds: about 1.5 s (the owner, 2026-11-11).
constexpr double kBreathSeconds = 1.5;

// How many steps of lightness a breath is drawn in: the window keeps one style record per step, so the breathe needs a
// bounded set of records. At 32 the steps are 0.008 apart in OKLCH lightness, below what the eye tells apart.
constexpr int kBreathLevels = 32;

// Where in its breath the busy cursor is at `seconds` (any fixed start): 0 at its dimmest, kBreathLevels - 1 at its
// brightest, rising and falling as a cosine, once every kBreathSeconds. A time before 0 counts as 0.
int breathLevel(double seconds);

// An OKLCH colour: lightness in [0, 1], chroma, hue in degrees.
struct Oklch {
    float l = 0;
    float c = 0;
    float h = 0;
};

// The cursor's colour at breath level `level` (breathLevel; the steady states ignore it). None has no colour: black.
Oklch cursorColor(CursorState state, int level);

} // namespace nexus
