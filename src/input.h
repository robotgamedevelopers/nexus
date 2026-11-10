// The window's event filter, as a pure function of an event's modifiers: no OS, no clock, no UI context. The window runs
// every event through it before Prax's UI context sees it; tests/window/ checks it.
//
// It works around something Prax's UI context does not offer: Caps Lock is taken off the modifiers. A claim compares
// modifiers exactly (ui.cpp claimOf), so with Caps Lock on no binding (Enter, Command-Enter, Command-1 to Command-0, the text
// edit's own keys) would match.

#pragma once

#include "pr/app.h"

namespace nexus {

// The modifiers the UI context sees: the event's, without Caps Lock.
PrAppModifierFlags withoutCapsLock(PrAppModifierFlags modifiers);

} // namespace nexus
