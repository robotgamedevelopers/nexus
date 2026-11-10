// The window's event filter, as a pure function of an event's fields: no OS, no clock, no UI context. The window runs
// every event through it before Prax's UI context sees it; tests/window/ checks it.
//
// Each thing it does works around something Prax's UI context does not offer:
// - Option-1..9 selects a tab. A key claim matches the logical key only, and an Option-digit arrives as key Unknown (its
//   key is the character the layout types), so only the event's code names the digit. The character macOS types for
//   the chord is dropped with it.
// - Caps Lock is taken off the modifiers. A claim compares modifiers exactly (ui.cpp claimOf), so with Caps Lock on no
//   binding (Enter, Command-Enter, the text edit's own keys) would match.

#pragma once

#include <cstddef>

#include "pr/app.h"

namespace nexus {

enum class Action : unsigned char {
    None,
    SelectTab,      // select tab `tab`
};

// What the event filter keeps between events.
struct InputState {
    bool dropText = false;      // the next Text event belongs to a key the filter took
};

// What the window does with one event.
struct Filtered {
    Action action = Action::None;
    std::size_t tab = 0;                // SelectTab: 0-based
    bool consume = false;               // true: the UI context never sees the event
    PrAppModifierFlags modifiers = 0;   // the modifiers the UI context sees, without Caps Lock
};

// Filters one event. `key`, `code` and `modifiers` are the event's (zero where its kind has none).
Filtered filterEvent(InputState& state, PrAppEventKind kind, PrAppKey key, PrAppKeyCode code,
    PrAppModifierFlags modifiers);

} // namespace nexus
