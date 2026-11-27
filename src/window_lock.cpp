#include "window_lock.h"

#include <utility>

#include "frame_host.h"

namespace nexus {

WindowLock::WindowLock(double idleSeconds, std::unique_ptr<LockAuthenticator> authenticator, Hooks hooks)
    : lock_(idleSeconds), authenticator_(std::move(authenticator)), hooks_(std::move(hooks)) {
    if (authenticator_) method_ = authenticator_->method();
    showScreen();
}

void WindowLock::showScreen() {
    if (!hooks_.screen) return;
    if (lock_.drawsContent()) {
        hooks_.screen(std::nullopt);
        return;
    }
    hooks_.screen(View::LockScreen{lockTitle(lock_.state()), lockMessage(lock_.state(), lock_.reason(), method_,
                                                                          detail_)});
}

void WindowLock::ask() {
    if (!authenticator_) return;
    const std::uint64_t attempt = lock_.prompt();
    if (attempt == 0) return;
    const PrSurface surface = hooks_.surface ? hooks_.surface() : PrSurface{};
    authenticator_->request(attempt, surface, hooks_.post ? hooks_.post : LockPost([](LockNotice) {}));
    showScreen();
    if (hooks_.frame) hooks_.frame();
}

bool WindowLock::requestReveal(double now) {
    tick(now);
    if (drawsContent()) return true;
    ask();
    return false;
}

void WindowLock::shown() {
    if (lock_.state() == LockState::Locked && asksOnShow(lock_.reason())) ask();
}

void WindowLock::notices(std::deque<LockNotice> notices, double now) {
    const LockState before = lock_.state();
    if (applyLockNotices(lock_, std::move(notices), now, &detail_)) changed(before);
}

void WindowLock::relock(LockReason why) {
    const LockState before = lock_.state();
    lock_.relock(why);
    changed(before);
}

void WindowLock::tick(double now) {
    const LockState before = lock_.state();
    lock_.tick(now);
    if (lock_.state() != before) changed(before);
}

void WindowLock::input(const PrAppEvent& e, double now) {
    if (!lock_.drawsContent() || !isInputEvent(e)) return;
    const LockState before = lock_.state();
    if (resetsIdle(e)) lock_.input(now);
    else lock_.tick(now);
    if (lock_.state() != before) changed(before);
}

void WindowLock::cancel() {
    if (authenticator_) authenticator_->cancel();
}

void WindowLock::changed(LockState before) {
    const LockState now = lock_.state();
    const auto log = [this](const std::string& line) {
        if (hooks_.log) hooks_.log(line);
    };
    // A relock that overtakes the system's prompt takes the prompt away (its answer, if one still comes, is stale).
    if (before == LockState::Prompting && now == LockState::Locked && authenticator_) authenticator_->cancel();
    // The lock screen first, so that whatever draws next draws it.
    showScreen();
    if (before == LockState::Unlocked && now != LockState::Unlocked) {
        log(std::string("locked: ") + lockReasonName(lock_.reason()));
        if (hooks_.signInLock) hooks_.signInLock();
        if (hooks_.overlay) {
            if (hooks_.visible && hooks_.visible() && hooks_.hide) hooks_.hide();
        } else if (hooks_.veil) {
            hooks_.veil();
        }
    } else if (before != LockState::Unlocked && now == LockState::Locked && before != now) {
        log(std::string("not unlocked: ") + lockReasonName(lock_.reason()) + (detail_.empty() ? "" : ": ") + detail_);
    }
    if (before != LockState::Unlocked && now == LockState::Unlocked) {
        log("unlocked");
        if (hooks_.overlay && hooks_.visible && !hooks_.visible()) {
            if (hooks_.reveal) hooks_.reveal();
        } else if (hooks_.signInShown) {
            hooks_.signInShown();
        }
    }
    if (hooks_.frame) hooks_.frame();
}

LockReason relockFor(const PrAppEvent& e) {
    switch (e.kind) {
    case PrAppEventKind_Suspend: return LockReason::Sleep;
    case PrAppEventKind_ScreenLocked: return LockReason::ScreenLock;
    default: return LockReason::None;
    }
}

bool asksOnShow(LockReason why) {
    return why != LockReason::Failed && why != LockReason::Cancelled && why != LockReason::Unavailable;
}

const char* lockReasonName(LockReason why) {
    switch (why) {
    case LockReason::None: return "unlocked";
    case LockReason::Launch: return "launch";
    case LockReason::Sleep: return "sleep";
    case LockReason::ScreenLock: return "the screen locked";
    case LockReason::SessionSwitch: return "a session switch";
    case LockReason::Background: return "the background";
    case LockReason::Idle: return "the idle timeout";
    case LockReason::Failed: return "authentication failed";
    case LockReason::Cancelled: return "the prompt was cancelled";
    case LockReason::Unavailable: return "no authentication available";
    }
    return "?";
}

std::function<void(const std::string&)> openUnlessLocked(std::function<void(const std::string&)> open,
                                                         std::function<bool()> locked,
                                                         std::function<void(const std::string&)> log) {
    return [open = std::move(open), locked = std::move(locked), log = std::move(log)](const std::string& url) {
        if (locked && locked()) {
            if (log) log("a link was not opened: the window is locked");
            return;
        }
        if (open) open(url);
    };
}

} // namespace nexus
