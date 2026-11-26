#include "client_lock.h"

#include <utility>

namespace nexus {

ClientLock::ClientLock(double idleSeconds) : idleSeconds_(idleSeconds < 1 ? 1 : idleSeconds) {}

std::uint64_t ClientLock::prompt() {
    if (state_ != LockState::Locked) {
        return 0;
    }
    state_ = LockState::Prompting;
    return ++attempt_;
}

bool ClientLock::answer(std::uint64_t attempt, LockAnswer a, double now) {
    if (state_ != LockState::Prompting || attempt != attempt_) {
        return false;
    }
    switch (a) {
    case LockAnswer::Granted:
        state_ = LockState::Unlocked;
        reason_ = LockReason::None;
        lastInput_ = now;
        break;
    case LockAnswer::Failed: lock(LockReason::Failed); break;
    case LockAnswer::Cancelled: lock(LockReason::Cancelled); break;
    case LockAnswer::Unavailable: lock(LockReason::Unavailable); break;
    }
    return true;
}

void ClientLock::relock(LockReason why) {
    lock(why == LockReason::None ? LockReason::Launch : why);
}

void ClientLock::tick(double now) {
    if (state_ == LockState::Unlocked && now - lastInput_ >= idleSeconds_) {
        lock(LockReason::Idle);
    }
}

bool ClientLock::input(double now) {
    tick(now);
    if (state_ != LockState::Unlocked) {
        return false;
    }
    lastInput_ = now;
    return true;
}

void ClientLock::lock(LockReason why) {
    state_ = LockState::Locked;
    reason_ = why;
    lastInput_ = 0;
}

std::string lockTitle(LockState state) {
    return state == LockState::Prompting ? "nexus is waiting for you" : "nexus is locked";
}

std::string lockMessage(LockState state, LockReason why, const std::string& method, const std::string& detail,
                        LockInput input) {
    if (state == LockState::Prompting) {
        return "Confirm it is you with " + method + ".";
    }
    const bool tap = input == LockInput::Tap;
    const std::string again =
        (tap ? "Tap to unlock with " : "Press Return or click here to unlock with ") + method + ".";
    switch (why) {
    case LockReason::None:
    case LockReason::Launch: return again;
    case LockReason::Sleep: return "Locked when this computer went to sleep. " + again;
    case LockReason::ScreenLock: return "Locked when the screen locked. " + again;
    case LockReason::SessionSwitch: return "Locked when another user session took the screen. " + again;
    case LockReason::Background: return "Locked when nexus went to the background. " + again;
    case LockReason::Idle: return "Locked after a while without use. " + again;
    case LockReason::Failed:
        return "Not unlocked: " + method + " did not confirm you" + (detail.empty() ? "" : " (" + detail + ")") +
            ". " + again;
    case LockReason::Cancelled:
        return "Not unlocked: the prompt was cancelled" + (detail.empty() ? "" : " (" + detail + ")") + ". " + again;
    case LockReason::Unavailable:
        return "Not unlocked: " + method + " is not available" + (detail.empty() ? "" : " (" + detail + ")") +
            ". Set it up in the system's settings, then " + (tap ? "tap here." : "press Return or click here.");
    }
    return again;
}

void LockMailbox::post(LockNotice notice) {
    std::lock_guard<std::mutex> hold(mutex_);
    notices_.push_back(std::move(notice));
}

std::deque<LockNotice> LockMailbox::take() {
    std::lock_guard<std::mutex> hold(mutex_);
    return std::exchange(notices_, {});
}

bool applyLockNotices(ClientLock& lock, std::deque<LockNotice> notices, double now, std::string* detail) {
    bool counted = false;
    for (auto& n : notices) {
        if (n.kind == LockNotice::Kind::Relock) {
            lock.relock(n.why);
            counted = true;
        } else if (lock.answer(n.attempt, n.answer, now)) {
            counted = true;
            if (detail != nullptr) *detail = n.answer == LockAnswer::Granted ? std::string() : std::move(n.detail);
        }
    }
    return counted;
}

} // namespace nexus

namespace nexus {

LockAnswer lockAnswerForLAError(long code) {
    switch (code) {
    case kLocalAuthErrorUserCancel:
    case kLocalAuthErrorUserFallback:
    case kLocalAuthErrorSystemCancel:
    case kLocalAuthErrorAppCancel:
    case kLocalAuthErrorInvalidContext:
        return LockAnswer::Cancelled;
    case kLocalAuthErrorPasscodeNotSet:
    case kLocalAuthErrorNotInteractive:
        return LockAnswer::Unavailable;
    case kLocalAuthErrorAuthenticationFailed:
    default:
        return LockAnswer::Failed;
    }
}

LockAnswer lockAnswerForConsent(int result) {
    switch (result) {
    case kConsentVerified: return LockAnswer::Granted;
    case kConsentDeviceNotPresent:
    case kConsentNotConfiguredForUser:
    case kConsentDisabledByPolicy:
        return LockAnswer::Unavailable;
    case kConsentCanceled: return LockAnswer::Cancelled;
    case kConsentDeviceBusy:
    case kConsentRetriesExhausted:
    default:
        return LockAnswer::Failed;
    }
}

LockReason lockReasonForSessionChange(unsigned long event) {
    switch (event) {
    case kWtsConsoleDisconnect:
    case kWtsRemoteDisconnect:
    case kWtsSessionLogoff:
    case kWtsSessionRemoteControl:
        return LockReason::SessionSwitch;
    case kWtsSessionLock: return LockReason::ScreenLock;
    default: return LockReason::None;
    }
}

} // namespace nexus
