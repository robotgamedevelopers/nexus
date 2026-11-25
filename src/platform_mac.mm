#include "platform.h"
#include <algorithm>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface NexusForegroundActivity : NSObject
@property(nonatomic, strong) id activity;
@property(nonatomic, strong) id quitMonitor;
- (void)update:(NSNotification*)notification;
@end

@implementation NexusForegroundActivity
- (void)update:(NSNotification*)notification {
    NSWindow* window = notification.object;
    const bool foreground = window.isKeyWindow && window.isVisible && !window.isMiniaturized &&
        ![notification.name isEqualToString:NSWindowWillCloseNotification] &&
        ![notification.name isEqualToString:NSWindowDidResignKeyNotification];
    if (foreground && !self.activity) {
        self.activity = [NSProcessInfo.processInfo beginActivityWithOptions:
            (NSActivityUserInteractive & ~NSActivityIdleSystemSleepDisabled) reason:@"Interactive nexus window"];
    } else if (!foreground && self.activity) {
        [NSProcessInfo.processInfo endActivity:self.activity];
        self.activity = nil;
    }
}
- (void)dealloc {
    if (_quitMonitor) [NSEvent removeMonitor:_quitMonitor];
    if (_activity) [NSProcessInfo.processInfo endActivity:_activity];
}
@end

namespace nexus {

std::string startErrorLog(const std::string&) {
    return {};
}

void showLaunchError(const std::string&) {}

void prioritizeOverlayTransition() {
    NSProcessInfo* process = NSProcessInfo.processInfo;
    id token = [process beginActivityWithOptions:(NSActivityUserInteractive & ~NSActivityIdleSystemSleepDisabled)
                                         reason:@"Showing or hiding the nexus overlay"];
    // AppKit orders the window asynchronously. Keep the activity across the ordering animation,
    // including when the key window notification arrives before the compositor finishes.
    const double duration = std::max(0.35, NSAnimationContext.currentContext.duration);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(duration * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [process endActivity:token];
    });
}

void trackForegroundPriority(const PrSurface& surface, const std::function<void()>& hide) {
    if (surface.kind != PrSurfaceKind_NsView) return;
    NSWindow* window = ((__bridge NSView*)surface.handle).window;
    if (!window) return;
    static char key;
    if (objc_getAssociatedObject(window, &key)) return;
    NexusForegroundActivity* observer = [NexusForegroundActivity new];
    if (hide) {
        auto hideOverlay = hide;
        __weak NSWindow* overlayWindow = window;
        observer.quitMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
            handler:^NSEvent*(NSEvent* event) {
                const auto modifiers = event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                    NSEventModifierFlagControl | NSEventModifierFlagOption);
                if (overlayWindow.isKeyWindow && modifiers == NSEventModifierFlagCommand &&
                    [event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"q"]) {
                    if (!event.isARepeat) hideOverlay();
                    return nil;
                }
                return event;
            }];
    }
    objc_setAssociatedObject(window, &key, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    for (NSNotificationName name in @[NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
                                      NSWindowDidMiniaturizeNotification, NSWindowWillCloseNotification]) {
        [NSNotificationCenter.defaultCenter addObserver:observer selector:@selector(update:) name:name object:window];
    }
    [observer update:[NSNotification notificationWithName:NSWindowDidUpdateNotification object:window]];
}

bool coverScreen(const PrSurface& surface) {
    if (surface.kind != PrSurfaceKind_NsView) {
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
