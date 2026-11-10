#include "platform_mac.h"

#import <AppKit/AppKit.h>

namespace nexus {

void enterNativeFullScreen(void* nsView) {
    NSWindow* window = [(__bridge NSView*)nsView window];
    if (window == nil)
        return;
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    if ((window.styleMask & NSWindowStyleMaskFullScreen) == 0)
        [window toggleFullScreen:nil];
}

bool mainDisplay(float* width, float* height, float* scale) {
    NSScreen* screen = [NSScreen mainScreen];
    if (screen == nil)
        return false;
    *width = float(screen.frame.size.width);
    *height = float(screen.frame.size.height);
    *scale = float(screen.backingScaleFactor);
    return true;
}

} // namespace nexus
