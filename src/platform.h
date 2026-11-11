// The one thing the overlay needs from the system that Prax's lib/app does not give: covering the whole screen, the
// menu bar or the taskbar included. Everything else about the overlay (a borderless panel above other apps, on every
// Space and beside fullscreen apps, see-through, no Dock icon, the global hotkey, the keyboard) is Prax's (main.cpp).
// Each system's code is beside this file: platform_mac.mm (AppKit) and platform_win.cpp (Win32).
//
// A request for Prax's author: Prax holds an overlay's size and position to the screen's work area, below the menu bar
// and above the Dock or the taskbar. On macOS (visibleFrame): at create (lib/app/backends/cocoa/src/app_cocoa.mm:
// 2767-2770), in prAppWindowSetExtent (:1912-1917, :1926-1927) and in prAppWindowSetPosition (:1105-1116). On Windows
// (rcWork): lib/app/backends/win32/src/app_win32.cpp placeNewWindow and its placement (:3373). An overlay placement
// that fills a display's whole frame would make these files go.

#pragma once

#include "pr/app_surface.h"

namespace nexus {

// Gives the window behind `surface` (prAppWindowSurface) the whole frame of the screen with the pointer on it, or of
// the main screen, menu bar or taskbar included. Returns false, changing nothing, if the surface is not this system's
// kind, the view has no window, or there is no screen.
bool coverScreen(const PrAppSurface& surface);

} // namespace nexus
