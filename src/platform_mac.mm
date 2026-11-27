#include "platform.h"
#include <algorithm>
#include <cstdio>
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <pthread/qos.h>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <Carbon/Carbon.h>
#include "overlay.h"

@interface NexusForegroundActivity : NSObject
@property(nonatomic, strong) id activity;
@property(nonatomic) BOOL enabled;
@property(nonatomic) pthread_override_t qosOverride;
@property(nonatomic) task_role_t previousRole;
@property(nonatomic, strong) id quitMonitor;
@property(nonatomic) BOOL yieldFocus;
- (void)suspendFocus:(NSNotification*)notification;
- (void)update:(NSNotification*)notification;
@end

@implementation NexusForegroundActivity
- (void)update:(NSNotification*)notification {
    NSWindow* window = notification.object;
    if ([notification.name isEqualToString:NSWindowDidBecomeKeyNotification]) self.yieldFocus = NO;
    if ([notification.name isEqualToString:NSWindowDidResignKeyNotification]) {
        // Snapshot explicit switching before the next frame: Cmd-Tab can be released by then.
        const bool switching = (NSEvent.modifierFlags & NSEventModifierFlagCommand) &&
            CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, kVK_Tab);
        const bool outsideClick = NSEvent.pressedMouseButtons && !NSPointInRect(NSEvent.mouseLocation, window.frame);
        self.yieldFocus = self.yieldFocus || switching || outsideClick || NSApp.modalWindow || window.attachedSheet;
    }
    const bool foreground = window.isKeyWindow && window.isVisible && !window.isMiniaturized &&
        ![notification.name isEqualToString:NSWindowWillCloseNotification] &&
        ![notification.name isEqualToString:NSWindowDidResignKeyNotification];
    if (foreground && self.enabled && !self.activity) {
        task_category_policy_data_t category{};
        mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
        boolean_t defaults = FALSE;
        const auto read = task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
            reinterpret_cast<task_policy_t>(&category), &count, &defaults);
        self.previousRole = read == KERN_SUCCESS ? category.role : TASK_UNSPECIFIED;
        category.role = TASK_FOREGROUND_APPLICATION;
        const auto status = task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
            reinterpret_cast<task_policy_t>(&category), TASK_CATEGORY_POLICY_COUNT);
        if (status != KERN_SUCCESS)
            std::fprintf(stderr, "nexus: foreground scheduling could not be applied: %s\n", mach_error_string(status));
        self.qosOverride = pthread_override_qos_class_start_np(pthread_self(), QOS_CLASS_USER_INTERACTIVE, 0);
        if (!self.qosOverride)
            std::fprintf(stderr, "nexus: user-interactive thread priority could not be applied\n");
        self.activity = [NSProcessInfo.processInfo beginActivityWithOptions:
            (NSActivityUserInteractive & ~NSActivityIdleSystemSleepDisabled) reason:@"Interactive nexus window"];
    } else if ((!foreground || !self.enabled) && self.activity) {
        [NSProcessInfo.processInfo endActivity:self.activity];
        self.activity = nil;
        if (self.qosOverride) {
            pthread_override_qos_class_end_np(self.qosOverride);
            self.qosOverride = nullptr;
        }
        task_category_policy_data_t category{self.previousRole};
        const auto status = task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
            reinterpret_cast<task_policy_t>(&category), TASK_CATEGORY_POLICY_COUNT);
        if (status != KERN_SUCCESS)
            std::fprintf(stderr, "nexus: normal scheduling could not be restored: %s\n", mach_error_string(status));
    }
}
- (void)suspendFocus:(NSNotification*)notification {
    (void)notification;
    self.yieldFocus = YES;
}
- (void)dealloc {
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_quitMonitor) [NSEvent removeMonitor:_quitMonitor];
    if (_activity) [NSProcessInfo.processInfo endActivity:_activity];
    if (_qosOverride) pthread_override_qos_class_end_np(_qosOverride);
}
@end

namespace nexus {
namespace { char priorityObserverKey; }

std::string startErrorLog(const std::string&) {
    return {};
}

void showLaunchError(const std::string&) {}

void prioritizeOverlayTransition(bool enabled) {
    if (!enabled) return;
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

void trackForegroundPriority(const PrSurface& surface, const std::function<void()>& hide, bool enabled) {
    if (surface.kind != PrSurfaceKind_NsView) return;
    NSWindow* window = ((__bridge NSView*)surface.handle).window;
    if (!window) return;
    if (objc_getAssociatedObject(window, &priorityObserverKey)) return;
    NexusForegroundActivity* observer = [NexusForegroundActivity new];
    observer.enabled = enabled;
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
    objc_setAssociatedObject(window, &priorityObserverKey, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    for (NSNotificationName name in @[NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
                                      NSWindowDidMiniaturizeNotification, NSWindowWillCloseNotification]) {
        [NSNotificationCenter.defaultCenter addObserver:observer selector:@selector(update:) name:name object:window];
    }
    for (NSNotificationName name in @[NSWorkspaceWillSleepNotification, NSWorkspaceScreensDidSleepNotification,
                                      NSWorkspaceSessionDidResignActiveNotification]) {
        [NSWorkspace.sharedWorkspace.notificationCenter addObserver:observer selector:@selector(suspendFocus:)
            name:name object:nil];
    }
    [observer update:[NSNotification notificationWithName:NSWindowDidUpdateNotification object:window]];
}

void updateForegroundPriority(const PrSurface& surface, bool enabled) {
    if (surface.kind != PrSurfaceKind_NsView) return;
    NSWindow* window = ((__bridge NSView*)surface.handle).window;
    NexusForegroundActivity* observer = objc_getAssociatedObject(window, &priorityObserverKey);
    if (!observer || observer.enabled == enabled) return;
    observer.enabled = enabled;
    [observer update:[NSNotification notificationWithName:NSWindowDidUpdateNotification object:window]];
}

void protectOverlayFocus(const PrSurface& surface, bool enabled, bool restore) {
    if (surface.kind != PrSurfaceKind_NsView || !surface.handle) return;
    NSWindow* window = ((__bridge NSView*)surface.handle).window;
    NexusForegroundActivity* observer = objc_getAssociatedObject(window, &priorityObserverKey);
    if (!observer || !window) return;
    if (!enabled) { observer.yieldFocus = YES; return; }
    if (window.isKeyWindow) { observer.yieldFocus = NO; return; }
    const bool visible = window.isVisible && !window.isMiniaturized && window.alphaValue > 0;
    const bool yielded = observer.yieldFocus || NSApp.modalWindow || window.attachedSheet;
    if (!overlayMayRestoreFocus(enabled, visible, restore, yielded)) return;
    const pid_t other = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    [window makeKeyAndOrderFront:nil];
    std::fprintf(stderr, "nexus: focus recovery time=%.3f other_pid=%d restored=%d\n",
        NSDate.date.timeIntervalSince1970, int(other), int(window.isKeyWindow));
}

PrRect displayNotch(const PrSurface& surface) {
    if (surface.kind != PrSurfaceKind_NsView || !surface.handle) return {};
    NSView* view = (__bridge NSView*)surface.handle;
    NSWindow* window = view.window;
    NSScreen* screen = window.screen;
    if (!window || !screen) return {};
    if (@available(macOS 12.0, *)) {
        const CGFloat top = screen.safeAreaInsets.top;
        if (top <= 0) return {};
        const NSRect left = screen.auxiliaryTopLeftArea, right = screen.auxiliaryTopRightArea;
        // If the OS supplies no side areas, reserve the whole obstructed top band.
        const CGFloat x0 = NSIsEmptyRect(left) ? NSMinX(screen.frame) : NSMaxX(left);
        const CGFloat x1 = NSIsEmptyRect(right) ? NSMaxX(screen.frame) : NSMinX(right);
        if (x1 <= x0) return {};
        const NSRect global = NSMakeRect(x0, NSMaxY(screen.frame) - top, x1 - x0, top);
        const NSRect local = [view convertRect:[window convertRectFromScreen:global] fromView:nil];
        const NSRect clipped = NSIntersectionRect(local, view.bounds);
        if (NSIsEmptyRect(clipped)) return {};
        const CGFloat y0 = view.isFlipped ? NSMinY(clipped) - NSMinY(view.bounds) : NSMaxY(view.bounds) - NSMaxY(clipped);
        return {float(NSMinX(clipped) - NSMinX(view.bounds)), float(y0),
                float(NSMaxX(clipped) - NSMinX(view.bounds)), float(y0 + NSHeight(clipped))};
    }
    return {};
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
