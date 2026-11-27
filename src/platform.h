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

#include <functional>
#include <string>

#include "build_parts.h"
#include "pr/surface.h"
#include "pr/geom.h"

namespace nexus {

// Where the window's error lines (stderr) go when it has no terminal. On Windows nexus is a GUI program, whose stderr
// goes nowhere unless whoever started it redirected it: then stderr is reopened, appending, onto nexus.log in `home`,
// the window's home (%USERPROFILE%\.nexus\client, client_home.h), and that path is returned. With `home` empty, or on
// macOS, stderr stays as it is, and the result is empty. Called once, first thing in main().
std::string startErrorLog(const std::string& home);

// Shows `text` to the person, for the one failure that launch must explain because nothing else would (the hotkey that
// did not register). On Windows a message box, which also names the error log; on macOS nothing more than the line on
// stderr, which the caller has written already.
void showLaunchError(const std::string& text);

// Gives the window behind `surface` (prAppWindowSurface) the whole frame of the screen with the pointer on it, or of
// the main screen, menu bar or taskbar included. Returns false, changing nothing, if the surface is not this system's
// kind, the view has no window, or there is no screen. On iOS and iPadOS a scene's window is the system's to size and
// always fills the scene: true for a UIKit surface with a view in a window, else false.
#if NEXUS_MACOS
void prioritizeOverlayTransition(bool enabled);
void trackForegroundPriority(const PrSurface& surface, const std::function<void()>& hide, bool enabled);
void updateForegroundPriority(const PrSurface& surface, bool enabled);
// Display obstruction intersecting this view, in top-left logical view coordinates; empty on an unobscured display.
PrRect displayNotch(const PrSurface& surface);
#endif

// Desktop overlay only: block unsolicited focus changes where the OS supports it, or recover the keyboard.
// `restore` is true only after this showing has owned the keyboard. Hidden windows never acquire focus.
// Disable before an intentional hide, external browser handoff, or shutdown.
#if NEXUS_MACOS || defined(_WIN32)
void protectOverlayFocus(const PrSurface& surface, bool enabled, bool restore);
#endif

bool coverScreen(const PrSurface& surface);

#if !NEXUS_PROCESSES
// Whether the UIKit view behind `surface` is laid out in a regular horizontal size class (an iPad's full screen, or a
// wide part of it), as against a compact one (an iPhone, or a narrow iPad split). False for any other surface.
bool regularWidth(const PrSurface& surface);
// Lets the UIKit view behind `surface` go as the first responder, so that Prax, raising the keyboard at the end of this
// frame, makes it the first responder afresh and the system presents its keyboard whole (keyboard_start.h). Nothing
// for any other surface.
void releaseFirstResponder(const PrSurface& surface);
#endif

} // namespace nexus
