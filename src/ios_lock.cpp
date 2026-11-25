#include "ios_lock.h"

#include <utility>

namespace nexus {

bool promptsWhenActive(LockReason why) {
    switch (why) {
    case LockReason::Launch:
    case LockReason::Sleep:
    case LockReason::ScreenLock:
    case LockReason::SessionSwitch:
    case LockReason::Background:
        return true;
    case LockReason::None:
    case LockReason::Idle:
    case LockReason::Failed:
    case LockReason::Cancelled:
    case LockReason::Unavailable:
        return false;
    }
    return false;
}

void IosLock::setCover(bool on) {
    if (covered_ == on) return;
    covered_ = on;
    if (system_.cover) system_.cover(on);
}

void IosLock::relock(LockReason why) {
    const bool prompting = lock_.state() == LockState::Prompting;
    lock_.relock(why);
    step({IosLockStep::Op::Relock, 0, LockAnswer::Failed, why, 0, false});
    // The prompt that was up is overtaken: its answer, if one still comes, is stale (ClientLock::answer ignores it).
    if (prompting && system_.cancel) system_.cancel();
}

void IosLock::lifecycle(IosLifecycle now) {
    const IosLifecycle was = lifecycle_;
    lifecycle_ = now;
    switch (now) {
    case IosLifecycle::Background:
        // Covered before the system takes the app switcher's snapshot, which follows this event.
        setCover(true);
        relock(LockReason::Background);
        break;
    case IosLifecycle::Inactive:
        // The app's own prompt makes it inactive while it is up: that is the lock at work, not the person leaving.
        if (lock_.state() == LockState::Prompting) break;
        setCover(true);
        if (lock_.state() == LockState::Unlocked) relock(LockReason::SessionSwitch);
        break;
    case IosLifecycle::Active:
        setCover(false);
        if (was != IosLifecycle::Active && lock_.state() == LockState::Locked && promptsWhenActive(lock_.reason()))
            unlock();
        break;
    }
}

void IosLock::screenLocked() {
    relock(LockReason::ScreenLock);
}

void IosLock::unlock() {
    // Only an active app asks: a prompt asked for in the background is refused by the system, and would be a refusal
    // the person never saw.
    if (lifecycle_ != IosLifecycle::Active) return;
    const std::uint64_t attempt = lock_.prompt();
    step({IosLockStep::Op::Prompt, attempt, LockAnswer::Failed, LockReason::None, 0, false});
    if (attempt != 0 && system_.request) system_.request(attempt);
}

void IosLock::notices(std::deque<LockNotice> notices, double now) {
    for (LockNotice& n : notices) {
        if (n.kind == LockNotice::Kind::Relock) {
            relock(n.why);
            continue;
        }
        const bool counted = lock_.answer(n.attempt, n.answer, now);
        step({IosLockStep::Op::Answer, n.attempt, n.answer, LockReason::None, now, false});
        if (counted) detail_ = n.answer == LockAnswer::Granted ? std::string() : n.detail;
    }
}

void IosLock::tick(double now) {
    // Time only moves forward: a second call at the same moment (an input in the frame that ticked) is no step.
    if (now <= lastTick_) return;
    lastTick_ = now;
    lock_.tick(now);
    step({IosLockStep::Op::Tick, 0, LockAnswer::Failed, LockReason::None, now, false});
}

bool IosLock::input(double now) {
    // Time first, as its own step: an input after the idle timeout finds the window locked, and is dropped.
    tick(now);
    const bool delivered = lock_.input(now);
    step({IosLockStep::Op::Input, 0, LockAnswer::Failed, LockReason::None, now, delivered});
    return delivered;
}

bool IosLock::frame() {
    const bool draws = lock_.drawsContent();
    step({IosLockStep::Op::Frame, 0, LockAnswer::Failed, LockReason::None, 0, draws});
    return draws;
}

} // namespace nexus
