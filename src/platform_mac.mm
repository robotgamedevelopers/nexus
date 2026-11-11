#include "platform.h"

#import <AppKit/AppKit.h>

namespace nexus {

std::string startErrorLog() {
    return {};
}

void showLaunchError(const std::string&) {}

bool coverScreen(const PrAppSurface& surface) {
    if (surface.kind != PrAppSurfaceKind_NsView) {
        return false;
    }
    NSWindow* window = ((__bridge NSView*)surface.handle).window;
    if (window == nil)
        return false;
    NSScreen* screen = NSScreen.mainScreen;
    for (NSScreen* each in NSScreen.screens) {
        if (NSMouseInRect(NSEvent.mouseLocation, each.frame, NO)) {
            screen = each;
            break;
        }
    }
    if (screen == nil)
        return false;
    // AppKit does not hold a borderless window to the work area, so the frame stays the screen's whole frame.
    [window setFrame:screen.frame display:NO];
    return true;
}

} // namespace nexus
