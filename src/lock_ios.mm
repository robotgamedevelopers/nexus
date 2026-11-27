// The client lock's system side on iOS and iPadOS (client_lock_platform.h, ios_lock.h): LocalAuthentication's
// device-owner policy (Face ID or Touch ID, falling back to the device's passcode), and the cover over the window
// for the app switcher's snapshot. The OS's events that lock are Prax's own on iOS (the scene's lifecycle, and the
// screen lock as protected data going away), which main_ios.cpp takes from it, so watchLockEvents watches nothing
// more here.

#include "client_lock_platform.h"
#include "ios_lock.h"

#import <LocalAuthentication/LocalAuthentication.h>
#import <UIKit/UIKit.h>

#include <mutex>
#include <utility>

// The codes lockAnswerForLAError (client_lock.h) names, as this SDK defines them.
static_assert(nexus::kLocalAuthErrorAuthenticationFailed == LAErrorAuthenticationFailed, "LAError moved");
static_assert(nexus::kLocalAuthErrorUserCancel == LAErrorUserCancel, "LAError moved");
static_assert(nexus::kLocalAuthErrorUserFallback == LAErrorUserFallback, "LAError moved");
static_assert(nexus::kLocalAuthErrorSystemCancel == LAErrorSystemCancel, "LAError moved");
static_assert(nexus::kLocalAuthErrorPasscodeNotSet == LAErrorPasscodeNotSet, "LAError moved");
static_assert(nexus::kLocalAuthErrorAppCancel == LAErrorAppCancel, "LAError moved");
static_assert(nexus::kLocalAuthErrorInvalidContext == LAErrorInvalidContext, "LAError moved");
static_assert(nexus::kLocalAuthErrorNotInteractive == LAErrorNotInteractive, "LAError moved");

namespace nexus {

namespace {

class IosAuthenticator final : public LockAuthenticator {
public:
    // Named by what the device has: Face ID or Touch ID, each with the passcode behind it, or the passcode alone.
    std::string method() const override {
        LAContext* context = [LAContext new];
        [context canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:nil];
        switch (context.biometryType) {
        case LABiometryTypeFaceID: return "Face ID or your passcode";
        case LABiometryTypeTouchID: return "Touch ID or your passcode";
        default: return "your passcode";
        }
    }

    void request(std::uint64_t attempt, const PrSurface&, LockPost post) override {
        LAContext* context = [LAContext new];
        NSError* why = nil;
        // The device-owner policy: Face ID or Touch ID, and the passcode when biometry fails or is not set up. It fails
        // only on a device with no passcode.
        if (![context canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication error:&why]) {
            post({LockNotice::Kind::Answer, attempt, lockAnswerForLAError(long(why.code)), LockReason::None,
                  why.localizedDescription ? why.localizedDescription.UTF8String : "no way to authenticate"});
            return;
        }
        {
            std::lock_guard<std::mutex> hold(mutex_);
            if (context_) [context_ invalidate];
            context_ = context;
        }
        [context evaluatePolicy:LAPolicyDeviceOwnerAuthentication
                localizedReason:@"Unlock nexus"
                          reply:^(BOOL success, NSError* error) {
            if (success) {
                post({LockNotice::Kind::Answer, attempt, LockAnswer::Granted, LockReason::None, {}});
            } else {
                post({LockNotice::Kind::Answer, attempt, lockAnswerForLAError(long(error.code)), LockReason::None,
                      error.localizedDescription ? error.localizedDescription.UTF8String : ""});
            }
        }];
    }

    void cancel() override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (context_) [context_ invalidate];
        context_ = nil;
    }

private:
    std::mutex mutex_;
    LAContext* context_ = nil;
};

class IosEvents final : public LockEvents {};

// The cover's tag among the window's subviews.
constexpr NSInteger kCoverTag = 0x6e6c636b;  // "nlck"

} // namespace

#ifndef NEXUS_TEST_AUTHENTICATOR
std::unique_ptr<LockAuthenticator> systemLockAuthenticator() {
    return std::make_unique<IosAuthenticator>();
}
#endif

std::unique_ptr<LockEvents> watchLockEvents(LockPost) {
    return std::make_unique<IosEvents>();
}

void iosCoverWindow(const PrSurface& surface, bool on) {
    if (surface.kind != PrSurfaceKind_UiView || surface.handle == nullptr) return;
    UIView* view = (__bridge UIView*)surface.handle;
    UIView* host = view.window != nil ? view.window : view;
    UIView* cover = [host viewWithTag:kCoverTag];
    if (!on) {
        [cover removeFromSuperview];
        return;
    }
    if (cover != nil) return;
    // Opaque black, above the drawing: the app switcher's snapshot, taken once the app has gone to the background,
    // shows nothing of nexus whatever its last frame drew.
    cover = [[UIView alloc] initWithFrame:host.bounds];
    cover.tag = kCoverTag;
    cover.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    cover.backgroundColor = UIColor.blackColor;
    cover.userInteractionEnabled = NO;
    [host addSubview:cover];
}

} // namespace nexus
