// The window's system part on iOS and iPadOS (platform.h). An app there has no terminal and no overlay: its error lines
// stay on stderr (the device's console, and the simulator's spawn), and a scene's window fills the scene by the system's
// own sizing, so there is nothing to cover.
#include "idiom.h"
#include "platform.h"

#import <UIKit/UIKit.h>

namespace nexus {

std::string startErrorLog(const std::string&) {
    return {};
}

void showLaunchError(const std::string&) {}

bool coverScreen(const PrSurface& surface) {
    if (surface.kind != PrSurfaceKind_UiView || surface.handle == nullptr)
        return false;
    UIView* view = (__bridge UIView*)surface.handle;
    return view.window != nil;
}

bool regularWidth(const PrSurface& surface) {
    if (surface.kind != PrSurfaceKind_UiView || surface.handle == nullptr)
        return false;
    UIView* view = (__bridge UIView*)surface.handle;
    return view.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
}

void releaseFirstResponder(const PrSurface& surface) {
    if (surface.kind != PrSurfaceKind_UiView || surface.handle == nullptr)
        return;
    UIView* view = (__bridge UIView*)surface.handle;
    if (view.isFirstResponder)
        [view resignFirstResponder];
}

std::string deviceIdiom() {
    switch (UIDevice.currentDevice.userInterfaceIdiom) {
        case UIUserInterfaceIdiomPhone: return "phone";
        case UIUserInterfaceIdiomPad: return "pad";
        default: return "desktop";
    }
}

} // namespace nexus
