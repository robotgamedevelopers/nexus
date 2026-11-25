// The client lock's system side on macOS (client_lock_platform.h): LocalAuthentication's device-owner policy, and the
// AppKit notices Prax does not report.

#include "client_lock_platform.h"

#import <AppKit/AppKit.h>
#import <LocalAuthentication/LocalAuthentication.h>

#include <mutex>
#include <utility>

// The codes lockAnswerForLAError (client_lock.h) names, as this SDK defines them.
static_assert(nexus::kLAErrorAuthenticationFailed == LAErrorAuthenticationFailed, "LAError moved");
static_assert(nexus::kLAErrorUserCancel == LAErrorUserCancel, "LAError moved");
static_assert(nexus::kLAErrorUserFallback == LAErrorUserFallback, "LAError moved");
static_assert(nexus::kLAErrorSystemCancel == LAErrorSystemCancel, "LAError moved");
static_assert(nexus::kLAErrorPasscodeNotSet == LAErrorPasscodeNotSet, "LAError moved");
static_assert(nexus::kLAErrorAppCancel == LAErrorAppCancel, "LAError moved");
static_assert(nexus::kLAErrorInvalidContext == LAErrorInvalidContext, "LAError moved");
static_assert(nexus::kLAErrorNotInteractive == LAErrorNotInteractive, "LAError moved");

@interface NexusLockObserver : NSObject
@property(nonatomic) nexus::LockPost post;
- (void)screensSlept:(NSNotification*)notification;
- (void)sessionResigned:(NSNotification*)notification;
@end

@implementation NexusLockObserver
- (void)screensSlept:(NSNotification*)notification {
    (void)notification;
    if (self.post) self.post({nexus::LockNotice::Kind::Relock, 0, nexus::LockAnswer::Failed, nexus::LockReason::Sleep, {}});
}
- (void)sessionResigned:(NSNotification*)notification {
    (void)notification;
    if (self.post)
        self.post({nexus::LockNotice::Kind::Relock, 0, nexus::LockAnswer::Failed, nexus::LockReason::SessionSwitch, {}});
}
- (void)dealloc {
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
}
@end

namespace nexus {

namespace {

class MacAuthenticator final : public LockAuthenticator {
public:
    std::string method() const override { return "Touch ID or your password"; }

    void request(std::uint64_t attempt, const PrSurface&, LockPost post) override {
        LAContext* context = [LAContext new];
        NSError* why = nil;
        // The device-owner policy: Touch ID, a paired Apple Watch, or the account's login password. It fails only where
        // the account has none of them.
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
                localizedReason:@"unlock nexus"
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

class MacEvents final : public LockEvents {
public:
    explicit MacEvents(LockPost post) {
        observer_ = [NexusLockObserver new];
        observer_.post = std::move(post);
        NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
        [center addObserver:observer_ selector:@selector(screensSlept:)
                       name:NSWorkspaceScreensDidSleepNotification object:nil];
        [center addObserver:observer_ selector:@selector(sessionResigned:)
                       name:NSWorkspaceSessionDidResignActiveNotification object:nil];
    }
    ~MacEvents() override {
        [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer_];
        observer_.post = nullptr;
    }

private:
    NexusLockObserver* observer_ = nil;
};

} // namespace

#ifndef NEXUS_TEST_AUTHENTICATOR
std::unique_ptr<LockAuthenticator> systemLockAuthenticator() {
    return std::make_unique<MacAuthenticator>();
}
#endif

std::unique_ptr<LockEvents> watchLockEvents(LockPost post) {
    return std::make_unique<MacEvents>(std::move(post));
}

} // namespace nexus
