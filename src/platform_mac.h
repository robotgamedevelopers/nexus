// The one thing the overlay needs from macOS that Prax's lib/app does not give: covering the whole screen, menu bar
// included. Everything else about the overlay (a borderless panel above other apps, on every Space and beside
// fullscreen apps, see-through, no Dock icon, the global hotkey, the keyboard) is Prax's (main.cpp).
//
// A request for Prax's author: Prax holds an overlay's size and position to the screen's work area (visibleFrame),
// below the menu bar and above the Dock: at create (lib/app/backends/cocoa/src/app_cocoa.mm:2767-2770),
// in prAppWindowSetExtent (:1912-1917, :1926-1927) and in prAppWindowSetPosition (:1105-1116). An overlay placement
// that fills a display's whole frame would make this file go.

#pragma once

namespace nexus {

// Gives the window behind `nsView` (PrAppSurface::handle) the whole frame of the screen with the pointer on it, or of
// the main screen, menu bar included. Returns false, changing nothing, if the view has no window or there is no screen.
bool coverScreen(void* nsView);

} // namespace nexus
