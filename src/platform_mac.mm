#include "platform_mac.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include <cstdio>

namespace nexus {

namespace {

// A borderless NSWindow cannot become key, so it would never take the keyboard. Prax makes a plain NSWindow, which
// nexus cannot subclass: changing the live window's class breaks AppKit's key-value observation of it (AppKit throws as
// the window is torn down). So NSWindow's own answers are wrapped, once, and differ only for the overlay window: it can
// become key and main. Every other window gets NSWindow's answers unchanged. The frame needs no wrapping: NSWindow does
// not constrain a borderless window, so it keeps the screen's whole frame, menu bar included (--print-window-config
// asks NSWindow's own constrainFrameRect:toScreen: and shows it).
//
// Why not our own NSPanel subclass holding Prax's view: Prax's host answers prAppIsActive from its own window
// (app_cocoa.mm isActive: NSApp.isActive && host.window.isKeyWindow). With the view moved into a panel, Prax's window
// is never key, so every Prax part that asks (the Studio terminal's focus among them) would see the window as inactive
// for good. Prax has no public way to give its host another window.
const void* overlayWindow = nullptr;       // unretained; compared, never messaged
IMP canBecomeKeyOriginal = nullptr;
IMP canBecomeMainOriginal = nullptr;
bool wrapped = false;                      // both answers wrapped, each checked after the swap

BOOL canBecomeKey(id self, SEL sel) {
    if ((__bridge const void*)self == overlayWindow)
        return YES;
    return reinterpret_cast<BOOL (*)(id, SEL)>(canBecomeKeyOriginal)(self, sel);
}

BOOL canBecomeMain(id self, SEL sel) {
    if ((__bridge const void*)self == overlayWindow)
        return YES;
    return reinterpret_cast<BOOL (*)(id, SEL)>(canBecomeMainOriginal)(self, sel);
}

// Wraps one of NSWindow's methods. The original is stored before the swap, so a call on another thread in between
// never finds it unset, and the swap is checked after. Says why on a failure.
bool wrap(SEL selector, IMP replacement, IMP* original) {
    Method method = class_getInstanceMethod(NSWindow.class, selector);
    if (method == nullptr) {
        std::fprintf(stderr, "nexus: NSWindow has no -%s on this macOS, so the overlay cannot change its answer; "
            "run with --windowed\n", sel_getName(selector));
        return false;
    }
    *original = method_getImplementation(method);
    method_setImplementation(method, replacement);
    if (method_getImplementation(method) != replacement) {
        std::fprintf(stderr, "nexus: wrapping NSWindow -%s did not take; run with --windowed\n", sel_getName(selector));
        return false;
    }
    return true;
}

bool wrapWindowAnswers() {
    if (canBecomeKeyOriginal == nullptr) {
        wrapped = wrap(@selector(canBecomeKeyWindow), reinterpret_cast<IMP>(canBecomeKey), &canBecomeKeyOriginal) &&
            wrap(@selector(canBecomeMainWindow), reinterpret_cast<IMP>(canBecomeMain), &canBecomeMainOriginal);
    }
    return wrapped;
}

// True if NSWindow's method is our wrapper now.
bool isWrapped(SEL selector, IMP replacement) {
    Method method = class_getInstanceMethod(NSWindow.class, selector);
    return method != nullptr && method_getImplementation(method) == replacement;
}

// The lowest level that floats above the menu bar (NSMainMenuWindowLevel) and so covers the whole screen.
constexpr NSWindowLevel kOverlayLevel = NSStatusWindowLevel;
constexpr UInt32 kHotKeySignature = 0x6e787573;   // "nxus"

struct Overlay {
    NSWindow* __weak window = nil;
    NSView* __weak view = nil;      // Prax's view, which takes the keyboard
    EventHotKeyRef hotKey = nullptr;
    EventHandlerRef handler = nullptr;
    bool hotKeyRegistered = false;
    OSStatus hotKeyStatus = noErr;
    OSStatus handlerStatus = noErr;
    id resignActive = nil;          // observers that put the overlay away (observeDeactivation)
    id resignKey = nil;
    OverlayConfig config{};
    void (*shown)(void*) = nullptr; // called each time the overlay is shown (setOverlayShown)
    void* shownContext = nullptr;
};

Overlay overlay;

// Takes the overlay off the screen without hiding the application. For when nexus has already lost the keyboard.
void putAway() {
    [overlay.window orderOut:nil];
}

// The owner's rule: Command-Tab (or a click on another app, or Mission Control) hides the overlay and leaves the OS
// to switch apps. So the overlay goes away when nexus stops being the active app or the overlay stops being the key
// window, and the next hotkey press shows it again.
void observeDeactivation(NSWindow* window) {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    if (overlay.resignActive == nil) {
        overlay.resignActive = [center addObserverForName:NSApplicationDidResignActiveNotification object:NSApp
            queue:nil usingBlock:^(NSNotification*) { putAway(); }];
    }
    if (overlay.resignKey == nil) {
        overlay.resignKey = [center addObserverForName:NSWindowDidResignKeyNotification object:window queue:nil
            usingBlock:^(NSNotification*) { putAway(); }];
    }
}

void stopObservingDeactivation() {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    if (overlay.resignActive != nil)
        [center removeObserver:overlay.resignActive];
    if (overlay.resignKey != nil)
        [center removeObserver:overlay.resignKey];
    overlay.resignActive = nil;
    overlay.resignKey = nil;
}

// The screen with the pointer on it, or the main screen.
NSScreen* overlayScreen() {
    NSPoint pointer = NSEvent.mouseLocation;
    for (NSScreen* screen in NSScreen.screens) {
        if (NSMouseInRect(pointer, screen.frame, NO))
            return screen;
    }
    return NSScreen.mainScreen;
}

void showOverlay() {
    NSWindow* window = overlay.window;
    if (window == nil)
        return;
    NSScreen* screen = overlayScreen();
    if (screen != nil)
        [window setFrame:screen.frame display:NO];
    if (NSApp.isHidden)
        [NSApp unhide:nil];
    // macOS 14 and later (nexus needs 26) activate cooperatively: -activate is the call, and the system may refuse it
    // from the background. A hotkey press is the person's own input, which the system honours. If it still refuses,
    // the overlay would stand over every app without the keyboard, so it is put away and the refusal said.
    [NSApp activate];
    [window makeKeyAndOrderFront:nil];
    if (overlay.view != nil)
        [window makeFirstResponder:overlay.view];
    if (overlay.shown != nullptr)
        overlay.shown(overlay.shownContext);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
        NSWindow* shown = overlay.window;
        if (shown != nil && shown.isVisible && (!NSApp.isActive || !shown.isKeyWindow)) {
            std::fprintf(stderr, "nexus: macOS did not make nexus the active app (active %s, overlay key %s), so the "
                "overlay is put away rather than cover other apps without the keyboard; press the hotkey again\n",
                NSApp.isActive ? "yes" : "no", shown.isKeyWindow ? "yes" : "no");
            putAway();
        }
    });
}

// The hotkey hides only an overlay that is up and has the keyboard; from any other state (hidden, put away, or up
// without the keyboard) it shows it.
void toggleOverlay() {
    NSWindow* window = overlay.window;
    if (window == nil)
        return;
    if (NSApp.isActive && !NSApp.isHidden && window.isVisible && window.isKeyWindow)
        hideOverlay();
    else
        showOverlay();
}

OSStatus onHotKey(EventHandlerCallRef, EventRef event, void*) {
    EventHotKeyID id{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof id, nullptr, &id) != noErr ||
            id.signature != kHotKeySignature)
        return eventNotHandledErr;
    toggleOverlay();
    return noErr;
}

// RegisterEventHotKey needs neither Accessibility nor Input Monitoring permission.
void registerHotKey(const OverlayConfig& config) {
    if (overlay.handler == nullptr) {
        EventTypeSpec spec{kEventClassKeyboard, kEventHotKeyPressed};
        overlay.handlerStatus = InstallEventHandler(GetApplicationEventTarget(), onHotKey, 1, &spec, nullptr,
            &overlay.handler);
        if (overlay.handlerStatus != noErr) {
            overlay.handler = nullptr;
            overlay.hotKeyRegistered = false;
            std::fprintf(stderr, "nexus: the hotkey's event handler did not install (OSStatus %d), so the hotkey "
                "cannot show the overlay; run with --windowed\n", int(overlay.handlerStatus));
            return;
        }
    }
    EventHotKeyID id{kHotKeySignature, 1};
    overlay.hotKeyStatus = RegisterEventHotKey(config.hotKeyCode, config.hotKeyModifiers, id,
        GetApplicationEventTarget(), 0, &overlay.hotKey);
    overlay.hotKeyRegistered = overlay.hotKeyStatus == noErr;
    if (!overlay.hotKeyRegistered) {
        overlay.hotKey = nullptr;
        std::fprintf(stderr, "nexus: the overlay hotkey (key code 0x%x, modifiers 0x%x) did not register (OSStatus %d); "
            "another app may hold it: change kOverlayHotKeyCode/kOverlayHotKeyModifiers in src/style.h\n",
            config.hotKeyCode, config.hotKeyModifiers, int(overlay.hotKeyStatus));
    }
}

// Puts an NSVisualEffectView behind `view` by moving both into a new content view.
void addBlur(NSWindow* window, NSView* view) {
    NSView* container = [[NSView alloc] initWithFrame:view.frame];
    NSVisualEffectView* blur = [[NSVisualEffectView alloc] initWithFrame:container.bounds];
    blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    blur.material = NSVisualEffectMaterialHUDWindow;
    blur.state = NSVisualEffectStateActive;
    blur.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = container;
    [container addSubview:blur];
    view.frame = container.bounds;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [container addSubview:view];
    [window makeFirstResponder:view];
}

void configure(NSWindow* window, NSView* view, const OverlayConfig& config) {
    overlay.config = config;
    overlay.window = window;
    overlay.view = view;
    if (wrapWindowAnswers())
        overlayWindow = (__bridge const void*)window;
    observeDeactivation(window);
    window.styleMask = NSWindowStyleMaskBorderless;
    window.level = kOverlayLevel;
    // Without FullScreenPrimary the window never takes a fullscreen Space of its own.
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.opaque = NO;
    window.backgroundColor = NSColor.clearColor;
    window.hasShadow = NO;
    window.hidesOnDeactivate = NO;
    // Prax gives the layer a black background, which would show under every see-through pixel.
    view.layer.opaque = NO;
    view.layer.backgroundColor = NSColor.clearColor.CGColor;
    NSScreen* screen = overlayScreen();
    if (screen != nil)
        [window setFrame:screen.frame display:NO];
    if (config.blur)
        addBlur(window, view);
    if (config.accessory)
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    registerHotKey(config);
}

const char* levelName(NSWindowLevel level) {
    if (level == NSNormalWindowLevel)
        return "normal";
    if (level == NSFloatingWindowLevel)
        return "floating";
    if (level == NSMainMenuWindowLevel)
        return "main menu";
    if (level == NSStatusWindowLevel)
        return "status";
    return "other";
}

std::string describe(NSWindow* window, NSView* view) {
    if (window == nil)
        return "window: none\n";
    NSWindowCollectionBehavior behavior = window.collectionBehavior;
    NSRect frame = window.frame;
    NSScreen* screen = window.screen != nil ? window.screen : overlayScreen();
    // NSWindow's own constraint, unwrapped: what AppKit does to a screen-sized frame as it orders the window in.
    NSRect own = [window constrainFrameRect:screen.frame toScreen:screen];
    bool keepsScreenFrame = NSEqualRects(own, screen.frame) && NSEqualRects(frame, screen.frame);
    bool keyWrapped = isWrapped(@selector(canBecomeKeyWindow), reinterpret_cast<IMP>(canBecomeKey));
    bool mainWrapped = isWrapped(@selector(canBecomeMainWindow), reinterpret_cast<IMP>(canBecomeMain));
    char text[3072];
    std::snprintf(text, sizeof text,
        "window class: %s\n"
        "can become key: %s\n"
        "style mask: 0x%llx (%s)\n"
        "level: %ld (%s)\n"
        "collection behavior: 0x%llx (CanJoinAllSpaces %s, FullScreenAuxiliary %s, FullScreenPrimary %s)\n"
        "frame: %.0f,%.0f %.0fx%.0f; screen frame %.0f,%.0f %.0fx%.0f (menu bar included); kept unconstrained: %s\n"
        "NSWindow's own constrainFrameRect:toScreen: (not wrapped) gives the screen frame as: %.0f,%.0f %.0fx%.0f\n"
        "wrapped NSWindow methods (checked in the method table): canBecomeKeyWindow %s, canBecomeMainWindow %s; "
        "originals stored before the swap: %s\n"
        "hides when nexus stops being active: %s; hides when the overlay stops being key: %s\n"
        "hotkey handler: %s (OSStatus %d)\n"
        "window opaque: %s; window background alpha: %.2f; window alphaValue: %.2f; shadow: %s\n"
        "layer class: %s; layer opaque: %s; layer background alpha: %.2f\n"
        "blur view: %s\n"
        "activation policy: %s\n"
        "visible: %s\n"
        "hotkey: key code 0x%x, modifiers 0x%x, registered: %s (OSStatus %d)\n",
        object_getClassName(window),
        [window canBecomeKeyWindow] ? "yes" : "no",
        (unsigned long long)window.styleMask, window.styleMask == NSWindowStyleMaskBorderless ? "borderless" : "titled",
        long(window.level), levelName(window.level),
        (unsigned long long)behavior,
        (behavior & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0 ? "yes" : "no",
        (behavior & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0 ? "yes" : "no",
        (behavior & NSWindowCollectionBehaviorFullScreenPrimary) != 0 ? "yes" : "no",
        frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
        screen.frame.origin.x, screen.frame.origin.y, screen.frame.size.width, screen.frame.size.height,
        keepsScreenFrame ? "yes" : "no",
        own.origin.x, own.origin.y, own.size.width, own.size.height,
        keyWrapped ? "yes" : "no", mainWrapped ? "yes" : "no",
        canBecomeKeyOriginal != nullptr && canBecomeMainOriginal != nullptr ? "yes" : "no",
        overlay.resignActive != nil ? "yes" : "no", overlay.resignKey != nil ? "yes" : "no",
        overlay.handler != nullptr ? "installed" : "not installed", int(overlay.handlerStatus),
        window.opaque ? "yes" : "no", double(window.backgroundColor.alphaComponent), double(window.alphaValue),
        window.hasShadow ? "yes" : "no",
        view.layer != nil ? object_getClassName(view.layer) : "none", view.layer.opaque ? "yes" : "no",
        view.layer.backgroundColor != nullptr ? double(CGColorGetAlpha(view.layer.backgroundColor)) : 0.0,
        view.superview != nil && view.superview == window.contentView ? "yes" : "no",
        NSApp.activationPolicy == NSApplicationActivationPolicyAccessory ? "accessory" : "regular",
        window.isVisible ? "yes" : "no",
        overlay.config.hotKeyCode, overlay.config.hotKeyModifiers, overlay.hotKeyRegistered ? "yes" : "no",
        int(overlay.hotKeyStatus));
    return text;
}

} // namespace

bool openOverlay(void* nsView, const OverlayConfig& config, bool show) {
    NSView* view = (__bridge NSView*)nsView;
    NSWindow* window = view.window;
    if (window == nil)
        return false;
    configure(window, view, config);
    if (show)
        showOverlay();
    return true;
}

// Hiding the application keeps the window and everything in it, and gives the keyboard back to the app that had it.
void hideOverlay() {
    [NSApp hide:nil];
}

bool overlayVisible() {
    NSWindow* window = overlay.window;
    return window == nil || (window.isVisible && !NSApp.isHidden);
}

void setOverlayShown(void (*shown)(void* context), void* context) {
    overlay.shown = shown;
    overlay.shownContext = context;
}

void closeOverlay() {
    if (overlay.hotKey != nullptr)
        UnregisterEventHotKey(overlay.hotKey);
    overlay.hotKey = nullptr;
    overlay.hotKeyRegistered = false;
    if (overlay.handler != nullptr)
        RemoveEventHandler(overlay.handler);
    overlay.handler = nullptr;
    stopObservingDeactivation();
    overlayWindow = nullptr;
}

std::string describeOverlay() {
    return describe(overlay.window, overlay.view);
}

std::string describeHiddenOverlay(const OverlayConfig& config) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if (NSScreen.mainScreen == nil)
            return {};
        // As Prax's Cocoa backend makes its window (lib/app/backends/cocoa/src/app_cocoa.mm), never ordered on screen.
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
            NSWindowStyleMaskMiniaturizable;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1000, 780) styleMask:style
            backing:NSBackingStoreBuffered defer:YES];
        window.releasedWhenClosed = NO;
        NSView* view = [[NSView alloc] initWithFrame:window.contentView.bounds];
        view.layer = [CAMetalLayer layer];
        view.wantsLayer = YES;
        view.layer.backgroundColor = NSColor.blackColor.CGColor;
        window.contentView = view;
        configure(window, view, config);
        std::string text = describe(window, view);
        closeOverlay();
        // Tears the window down as a run's end does, so a class change that AppKit cannot live with shows here.
        [window close];
        return text;
    }
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
