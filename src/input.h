// The window's key bindings, as pure functions of an event's fields: no OS, no clock, no UI context. The window calls
// them; tests/window/ checks them.
//
// Two places take keys. The event filter sees every event before Prax's UI context does, and takes what the context
// cannot: Option-1..9 (whose key is the character the layout types, so only its code names the digit), the character
// macOS types for that chord, Enter while an input method composes, and the Caps Lock flag, which the context's exact
// modifier match would otherwise let break every binding. The prompt's keys are captured on the prompt in the build,
// in the order the context routes them, so a send always takes the text typed before it.

#pragma once

#include <cstddef>

#include "pr/app.h"

namespace nexus {

enum class Action : unsigned char {
    None,
    SelectTab,      // select tab `tab`
    Send,           // the prompt's text goes into the transcript
    Queue,          // the same, marked to be sent later
    Newline,        // a line feed at the caret
};

// What the event filter keeps between events.
struct InputState {
    bool composing = false;     // an input method has marked text
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

// A key the prompt captures, and what it does.
struct PromptKey {
    PrAppKey key;
    PrAppModifierFlags modifiers;
    Action action;
};

// The prompt's keys: Enter sends, Shift-Enter inserts a line feed, Command-Enter queues. Writes the count.
const PromptKey* promptKeys(std::size_t* count);

// Returns the prompt's action for a key and its modifiers (Caps Lock ignored), or None.
Action promptAction(PrAppKey key, PrAppModifierFlags modifiers);

} // namespace nexus
