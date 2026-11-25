// Unit tests of the client lock's rules (src/client_lock.h): logic only, no OS, no clock (time is an argument), no
// thread but the one test of the mailbox's order. The model is models/ClientLock.tla; lock_walk binds the code to it.

#include "client_lock.h"

#include <cstdio>
#include <string>

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr double kIdle = 600;

// Launch: locked, nothing drawn, no input taken, and it says nothing more than that it is locked.
void startsLocked() {
    ClientLock lock(kIdle);
    CHECK(lock.state() == LockState::Locked);
    CHECK(lock.reason() == LockReason::Launch);
    CHECK(!lock.drawsContent());
    CHECK(!lock.input(0));
    lock.tick(1e9);
    CHECK(lock.state() == LockState::Locked);
}

// The current attempt's grant unlocks; the window then draws and takes input.
void grantUnlocks() {
    ClientLock lock(kIdle);
    const auto n = lock.prompt();
    CHECK(n == 1);
    CHECK(lock.state() == LockState::Prompting);
    CHECK(!lock.drawsContent());
    CHECK(!lock.input(1));
    CHECK(lock.answer(n, LockAnswer::Granted, 5));
    CHECK(lock.state() == LockState::Unlocked);
    CHECK(lock.reason() == LockReason::None);
    CHECK(lock.drawsContent());
    CHECK(lock.input(6));
}

// A second prompt while one is up, or while unlocked, starts nothing.
void noSecondPrompt() {
    ClientLock lock(kIdle);
    const auto n = lock.prompt();
    CHECK(lock.prompt() == 0);
    CHECK(lock.attempt() == n);
    CHECK(lock.answer(n, LockAnswer::Granted, 0));
    CHECK(lock.prompt() == 0);
    CHECK(lock.state() == LockState::Unlocked);
}

// Each refusal locks and says which.
void refusalsStayLockedAndSayWhich() {
    const struct {
        LockAnswer answer;
        LockReason why;
    } cases[] = {{LockAnswer::Failed, LockReason::Failed},
                 {LockAnswer::Cancelled, LockReason::Cancelled},
                 {LockAnswer::Unavailable, LockReason::Unavailable}};
    for (const auto& c : cases) {
        ClientLock lock(kIdle);
        const auto n = lock.prompt();
        CHECK(lock.answer(n, c.answer, 0));
        CHECK(lock.state() == LockState::Locked);
        CHECK(lock.reason() == c.why);
        CHECK(!lock.drawsContent());
        // and the person may try again
        CHECK(lock.prompt() == n + 1);
    }
}

// A relock overtakes a prompt that is up: its grant, arriving after, unlocks nothing.
void grantAfterRelockIsStale() {
    ClientLock lock(kIdle);
    const auto n = lock.prompt();
    lock.relock(LockReason::Sleep);
    CHECK(lock.state() == LockState::Locked);
    CHECK(lock.reason() == LockReason::Sleep);
    CHECK(!lock.answer(n, LockAnswer::Granted, 1));
    CHECK(lock.state() == LockState::Locked);
    // Nor once a new prompt is up: only that prompt's answer counts.
    const auto m = lock.prompt();
    CHECK(m == n + 1);
    CHECK(!lock.answer(n, LockAnswer::Granted, 2));
    CHECK(lock.state() == LockState::Prompting);
    CHECK(lock.answer(m, LockAnswer::Granted, 3));
    CHECK(lock.state() == LockState::Unlocked);
    // An answer that is never asked for (0, or a number not given yet) unlocks nothing.
    lock.relock(LockReason::ScreenLock);
    lock.prompt();
    CHECK(!lock.answer(0, LockAnswer::Granted, 4));
    CHECK(!lock.answer(lock.attempt() + 1, LockAnswer::Granted, 4));
    CHECK(lock.state() == LockState::Prompting);
}

// Every relock event locks an unlocked window and says why.
void relockEventsLock() {
    for (const auto why : {LockReason::Sleep, LockReason::ScreenLock, LockReason::SessionSwitch,
                           LockReason::Background}) {
        ClientLock lock(kIdle);
        CHECK(lock.answer(lock.prompt(), LockAnswer::Granted, 0));
        lock.relock(why);
        CHECK(lock.state() == LockState::Locked);
        CHECK(lock.reason() == why);
        CHECK(!lock.drawsContent());
        CHECK(!lock.input(1));
    }
}

// Idle: input keeps it unlocked; the timeout without input locks, at a tick or at the next input.
void idleTimeoutLocks() {
    ClientLock lock(kIdle);
    CHECK(lock.answer(lock.prompt(), LockAnswer::Granted, 100));
    lock.tick(100 + kIdle - 1);
    CHECK(lock.state() == LockState::Unlocked);
    CHECK(lock.input(100 + kIdle - 1));          // the idle time starts again
    lock.tick(100 + 2 * kIdle - 2);
    CHECK(lock.state() == LockState::Unlocked);
    lock.tick(100 + 2 * kIdle - 1);
    CHECK(lock.state() == LockState::Locked);
    CHECK(lock.reason() == LockReason::Idle);

    // Input that comes after the timeout with no tick between is refused, and locks.
    ClientLock late(kIdle);
    CHECK(late.answer(late.prompt(), LockAnswer::Granted, 0));
    CHECK(!late.input(kIdle));
    CHECK(late.state() == LockState::Locked);
    CHECK(late.reason() == LockReason::Idle);
}

// The timeout cannot be turned off: anything under a second is a second.
void timeoutHasAFloor() {
    ClientLock lock(0);
    CHECK(lock.idleSeconds() == 1);
    lock.setIdleSeconds(-5);
    CHECK(lock.idleSeconds() == 1);
    CHECK(lock.answer(lock.prompt(), LockAnswer::Granted, 0));
    lock.tick(1);
    CHECK(lock.state() == LockState::Locked);
}

// The lock screen's words: why, and what to do; a refusal carries the system's words.
void messagesSayWhyAndWhatToDo() {
    const std::string method = "Touch ID or your password";
    CHECK(lockTitle(LockState::Locked) == "nexus is locked");
    CHECK(lockMessage(LockState::Prompting, LockReason::Launch, method, "") ==
          "Confirm it is you with Touch ID or your password.");
    CHECK(lockMessage(LockState::Locked, LockReason::Sleep, method, "").find("went to sleep") != std::string::npos);
    const auto failed = lockMessage(LockState::Locked, LockReason::Failed, method, "Application retry limit exceeded.");
    CHECK(failed.find("did not confirm you") != std::string::npos);
    CHECK(failed.find("retry limit") != std::string::npos);
    CHECK(failed.find("Press Return or click here") != std::string::npos);
    CHECK(lockMessage(LockState::Locked, LockReason::Unavailable, "Windows Hello", "").find("not available") !=
          std::string::npos);
}

// Notices from other threads apply in order: a relock then the stale grant leaves it locked; the detail is the last
// counted refusal's, and a grant clears it.
void mailboxAppliesInOrder() {
    ClientLock lock(kIdle);
    LockMailbox box;
    const auto n = lock.prompt();
    box.post({LockNotice::Kind::Relock, 0, LockAnswer::Failed, LockReason::ScreenLock, ""});
    box.post({LockNotice::Kind::Answer, n, LockAnswer::Granted, LockReason::None, ""});
    std::string detail = "old";
    CHECK(applyLockNotices(lock, box.take(), 1, &detail));
    CHECK(lock.state() == LockState::Locked);
    CHECK(detail == "old");
    CHECK(box.take().empty());

    const auto m = lock.prompt();
    box.post({LockNotice::Kind::Answer, m, LockAnswer::Failed, LockReason::None, "Biometry is locked out."});
    CHECK(applyLockNotices(lock, box.take(), 2, &detail));
    CHECK(lock.reason() == LockReason::Failed);
    CHECK(detail == "Biometry is locked out.");

    const auto k = lock.prompt();
    box.post({LockNotice::Kind::Answer, k, LockAnswer::Granted, LockReason::None, ""});
    CHECK(applyLockNotices(lock, box.take(), 3, &detail));
    CHECK(lock.state() == LockState::Unlocked);
    CHECK(detail.empty());
    CHECK(!applyLockNotices(lock, box.take(), 4, &detail));
}

// The systems' codes: only success grants; cancelling is Cancelled; no way to authenticate is Unavailable; the rest,
// unknown codes included, fail. Only the session leaving or locking relocks.
void systemCodesMap() {
    CHECK(lockAnswerForLAError(kLocalAuthErrorAuthenticationFailed) == LockAnswer::Failed);
    CHECK(lockAnswerForLAError(kLocalAuthErrorUserCancel) == LockAnswer::Cancelled);
    CHECK(lockAnswerForLAError(kLocalAuthErrorSystemCancel) == LockAnswer::Cancelled);
    CHECK(lockAnswerForLAError(kLocalAuthErrorAppCancel) == LockAnswer::Cancelled);
    CHECK(lockAnswerForLAError(kLocalAuthErrorPasscodeNotSet) == LockAnswer::Unavailable);
    CHECK(lockAnswerForLAError(kLocalAuthErrorNotInteractive) == LockAnswer::Unavailable);
    CHECK(lockAnswerForLAError(0) == LockAnswer::Failed);
    CHECK(lockAnswerForLAError(-8) == LockAnswer::Failed);
    CHECK(lockAnswerForConsent(kConsentVerified) == LockAnswer::Granted);
    CHECK(lockAnswerForConsent(kConsentNotConfiguredForUser) == LockAnswer::Unavailable);
    CHECK(lockAnswerForConsent(kConsentDeviceNotPresent) == LockAnswer::Unavailable);
    CHECK(lockAnswerForConsent(kConsentDisabledByPolicy) == LockAnswer::Unavailable);
    CHECK(lockAnswerForConsent(kConsentCanceled) == LockAnswer::Cancelled);
    CHECK(lockAnswerForConsent(kConsentRetriesExhausted) == LockAnswer::Failed);
    CHECK(lockAnswerForConsent(kConsentDeviceBusy) == LockAnswer::Failed);
    CHECK(lockAnswerForConsent(99) == LockAnswer::Failed);
    CHECK(lockReasonForSessionChange(kWtsConsoleDisconnect) == LockReason::SessionSwitch);
    CHECK(lockReasonForSessionChange(kWtsRemoteDisconnect) == LockReason::SessionSwitch);
    CHECK(lockReasonForSessionChange(kWtsSessionLogoff) == LockReason::SessionSwitch);
    CHECK(lockReasonForSessionChange(kWtsSessionLock) == LockReason::ScreenLock);
    CHECK(lockReasonForSessionChange(0x1) == LockReason::None);      // console connect
    CHECK(lockReasonForSessionChange(0x8) == LockReason::None);      // unlock
}

} // namespace

int main() {
    systemCodesMap();
    startsLocked();
    grantUnlocks();
    noSecondPrompt();
    refusalsStayLockedAndSayWhich();
    grantAfterRelockIsStale();
    relockEventsLock();
    idleTimeoutLocks();
    timeoutHasAFloor();
    messagesSayWhyAndWhatToDo();
    mailboxAppliesInOrder();
    if (failures != 0) {
        std::fprintf(stderr, "client_lock_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("client_lock_test: all passed\n");
    return 0;
}
