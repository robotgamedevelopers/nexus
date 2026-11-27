// Exercise the real macOS scheduling calls without displaying or activating a window.
#include "platform.h"
#include <cstdio>
#include <cmath>
#include <mach/mach.h>
#include <mach/task_policy.h>
#import <AppKit/AppKit.h>

@interface NotchTestScreen : NSObject
@property(nonatomic) NSRect frame;
@property(nonatomic) NSEdgeInsets safeAreaInsets;
@property(nonatomic) NSRect auxiliaryTopLeftArea;
@property(nonatomic) NSRect auxiliaryTopRightArea;
@end
@implementation NotchTestScreen
@end
@interface FlippedTestView : NSView
@end
@implementation FlippedTestView
- (BOOL)isFlipped { return YES; }
@end
@interface PriorityTestWindow : NSWindow
@property(nonatomic) BOOL pretendForeground;
@property(nonatomic) BOOL pretendVisible;
@property(nonatomic) int focusRequests;
@property(nonatomic, strong) NSScreen* testScreen;
@end
@implementation PriorityTestWindow
- (NSScreen*)screen { return self.testScreen ? self.testScreen : [super screen]; }
- (BOOL)isKeyWindow { return self.pretendForeground; }
- (BOOL)isVisible { return self.pretendForeground || self.pretendVisible; }
- (void)makeKeyAndOrderFront:(id)sender { (void)sender; ++self.focusRequests; self.pretendForeground = YES; }
- (BOOL)isMiniaturized { return NO; }
@end

static task_role_t role() {
    task_category_policy_data_t category{};
    mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
    boolean_t defaults = FALSE;
    if (task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
            reinterpret_cast<task_policy_t>(&category), &count, &defaults) != KERN_SUCCESS) return TASK_RENICED;
    return category.role;
}

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        auto* window = [[PriorityTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 100, 100)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:YES];
        window.releasedWhenClosed = NO;
        window.pretendForeground = YES;
        const auto before = role();
        const auto boosted = before == TASK_FOREGROUND_APPLICATION || before == TASK_BACKGROUND_APPLICATION ||
            before == TASK_DEFAULT_APPLICATION ? TASK_FOREGROUND_APPLICATION : before;
        const PrSurface surface{.kind = PrSurfaceKind_NsView, .handle = (__bridge void*)window.contentView};
        nexus::trackForegroundPriority(surface, {}, true);
        if (role() != boosted) return 1;
        nexus::updateForegroundPriority(surface, false);
        if (role() != before) return 2;
        nexus::updateForegroundPriority(surface, true);
        if (role() != boosted) return 3;
        window.pretendForeground = NO;
        [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification object:window];
        if (role() != before) return 4;
        // Also exercise a role the kernel permits us to restore, regardless of the test launcher.
        task_category_policy_data_t background{TASK_BACKGROUND_APPLICATION};
        if (task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
                reinterpret_cast<task_policy_t>(&background), TASK_CATEGORY_POLICY_COUNT) != KERN_SUCCESS) return 12;
        window.pretendForeground = YES;
        [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidBecomeKeyNotification object:window];
        if (role() != TASK_FOREGROUND_APPLICATION) return 13;
        window.pretendForeground = NO;
        [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification object:window];
        if (role() != TASK_BACKGROUND_APPLICATION) return 14;
        // Restore only an already-focused visible overlay. Never activate a real desktop window in this test.
        nexus::protectOverlayFocus(surface, true, true);
        if (window.focusRequests != 0) return 8;
        window.pretendVisible = YES;
        nexus::protectOverlayFocus(surface, true, false);
        if (window.focusRequests != 0) return 9;
        nexus::protectOverlayFocus(surface, true, true);
        if (window.focusRequests != 1 || !window.isKeyWindow) return 10;
        nexus::protectOverlayFocus(surface, false, true);
        window.pretendForeground = NO;
        nexus::protectOverlayFocus(surface, true, true);
        if (window.focusRequests != 1) return 11;
        window.pretendForeground = YES;
        nexus::protectOverlayFocus(surface, true, true);
        [NSWorkspace.sharedWorkspace.notificationCenter postNotificationName:NSWorkspaceSessionDidResignActiveNotification object:nil];
        window.pretendForeground = NO;
        nexus::protectOverlayFocus(surface, true, true);
        if (window.focusRequests != 1) return 12;
        window.pretendVisible = NO;
        // Screen coordinates can be negative on a secondary display. The same notch must map correctly in both
        // AppKit view orientations, and disappear when the content is entirely below the camera housing.
        auto* screen = [NotchTestScreen new];
        screen.frame = NSMakeRect(-1000, -800, 1000, 800);
        screen.safeAreaInsets = NSEdgeInsetsMake(38, 0, 0, 0);
        screen.auxiliaryTopLeftArea = NSMakeRect(-1000, -38, 440, 38);
        screen.auxiliaryTopRightArea = NSMakeRect(-440, -38, 440, 38);
        [window setFrame:screen.frame display:NO];
        for (bool flipped : {false, true}) {
            window.contentView = flipped ? [[FlippedTestView alloc] initWithFrame:NSMakeRect(0, 0, 1000, 800)] :
                [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1000, 800)];
            const PrSurface test{.kind = PrSurfaceKind_NsView, .handle = (__bridge void*)window.contentView};
            window.testScreen = (NSScreen*)screen;
            const auto notch = nexus::displayNotch(test);
            window.testScreen = nil;
            if (std::abs(notch.left - 440) > 0.1 || std::abs(notch.right - 560) > 0.1 ||
                std::abs(notch.top) > 0.1 || std::abs(notch.bottom - 38) > 0.1) return 5;
            [window setFrame:NSMakeRect(-1000, -800, 1000, 760) display:NO];
            window.testScreen = (NSScreen*)screen;
            const auto below = nexus::displayNotch(test);
            window.testScreen = nil;
            if (below.bottom != 0) return 6;
            [window setFrame:screen.frame display:NO];
        }
        screen.safeAreaInsets = NSEdgeInsetsMake(0, 0, 0, 0);
        const PrSurface test{.kind = PrSurfaceKind_NsView, .handle = (__bridge void*)window.contentView};
        window.testScreen = (NSScreen*)screen;
        const auto unobstructed = nexus::displayNotch(test);
        window.testScreen = nil;
        if (unobstructed.bottom != 0 || nexus::displayNotch({}).bottom != 0) return 7;
        [window close];
    }
    std::puts("foreground scheduling and notch coordinate conversion passed");
}
