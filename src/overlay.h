// When the overlay shows and hides: the rules alone, apart from the window that carries them out. main.cpp asks these
// rules at each input and does what they answer through Prax's lib/app (prAppWindowSetVisible, prAppWindowFocus). What
// the window reports, whether it is visible and whether it has the keyboard (prAppWindowIsActive), comes in as
// arguments, so the rules run without a window. No OS, no time.
//
// The owner's rules: the hotkey hides an overlay that is up with the keyboard, and shows it from any other state (so
// after Command-Tab the next press shows it, not merely re-activates it). Escape hides it. Once a shown overlay has had
// the keyboard, losing it (Command-Tab, a click on another app) hides it. One that never took the keyboard is hidden
// when the check after its show finds it without, rather than stand over other apps.
//
// Model: models/Overlay.tla. tests/model/overlay_walk.cpp drives these rules and writes traces that
// models/OverlayTrace.tla checks against the model.

#pragma once

namespace nexus {

// What the window does next.
enum class OverlayAct {
    None,
    Show,       // cover the screen, show the window and give it the keyboard, and start the key check
    Hide,       // hide the window
};

class OverlayRules {
public:
    // The hotkey: Hide if the overlay is visible and has the keyboard, Show otherwise.
    OverlayAct hotkey(bool visible, bool active);

    // Escape: Hide if the overlay is visible.
    OverlayAct escape(bool visible);

    // At each frame (a change of either input asks for one): a visible overlay with the keyboard is noted as having had
    // it; one that has had it and lost it is hidden.
    OverlayAct observe(bool visible, bool active);

    // The check after a show: a visible overlay without the keyboard is hidden. One with it is noted as having had it.
    OverlayAct keyCheck(bool visible, bool active);

    // True once the overlay has had the keyboard since it was last shown.
    bool keyed() const { return keyed_; }

private:
    OverlayAct act(OverlayAct a);

    bool keyed_ = false;
};

// Whether the overlay's window closing means it failed. Prax closes a window whose swap chain cannot be made before
// the window's open, and tells a program that created the window itself only through its close, which comes whether
// or not it opened (Prax 2908525 studio_window.cpp open 377-393, studio_window.h 91). nexus never closes the overlay,
// and the run closes it only after shutdown: so a close before open, outside shutdown, is Prax refusing the window.
// `opened`: main.cpp open() ran; `stopping`: the run's shutdown has begun.
bool overlayCloseFailed(bool opened, bool stopping);

} // namespace nexus
