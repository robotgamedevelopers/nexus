// What the window needs from macOS that Prax's lib/app does not offer: turning its window into the overlay (a
// borderless, see-through window over the whole screen, shown and hidden by a global hotkey, like iTerm2's hotkey
// window), and the size of the main display without a window.
//
// Prax's window and its Metal layer are left as Prax made them except for what the overlay sets on them through the
// NSView handle (PrAppSurface::handle): the window's style, frame, level, Spaces behaviour and opacity, and the layer's
// opaque flag and background. Prax clears each frame to the canvas colour with its alpha, so a non-opaque layer shows
// what is behind the window wherever the canvas is not fully opaque.

#pragma once

#include <string>

namespace nexus {

struct OverlayConfig {
    unsigned hotKeyCode;        // Carbon virtual key code
    unsigned hotKeyModifiers;   // Carbon modifier mask
    bool blur;                  // an NSVisualEffectView behind the Metal view
    bool accessory;             // accessory activation policy: no Dock icon, not in the app switcher
};

// Turns the window that holds `nsView` into the overlay and registers the global hotkey that shows and hides it. Shows
// it (and takes keyboard focus) if `show`. Call once. Returns false if the view has no window. The overlay puts itself
// away when nexus stops being the active app or the overlay stops being the key window (Command-Tab, a click on
// another app, Mission Control); the next hotkey press shows it again.
bool openOverlay(void* nsView, const OverlayConfig& config, bool show);

// Hides the overlay: the hotkey's hide and Escape's. The window and everything in it are kept, and the keyboard goes
// back to the app that had it.
void hideOverlay();

// True while the overlay is on screen: its window is visible and the app not hidden. True with no overlay (a normal
// window, or none). Call on the main thread.
bool overlayVisible();

// Calls `shown` with `context` each time the overlay is shown (the hotkey), after it takes the keyboard: the window
// asks for a frame there, so that what waited while it was hidden catches up at once.
void setOverlayShown(void (*shown)(void* context), void* context);

// Unregisters the hotkey.
void closeOverlay();

// The overlay window's configuration as it stands, one "name: value" per line, for --print-window-config and the log.
std::string describeOverlay();

// For --print-window-config: makes a window as Prax's Cocoa backend makes one (an NSWindow over a layer-backed view
// with a CAMetalLayer), never ordered on screen, turns it into the overlay without showing it, and returns its
// description. Returns an empty string if there is no screen.
std::string describeHiddenOverlay(const OverlayConfig& config);

// Writes the main display's size in logical points and its backing scale. Returns false if there is no display.
bool mainDisplay(float* width, float* height, float* scale);

} // namespace nexus
