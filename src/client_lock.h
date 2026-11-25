// The client lock: a window shows nothing of nexus (no Dashboard, no AI tabs or their names, no terminals, no tab strip,
// no sound of what an AI did) and takes no input until the device's owner has passed the system's own authentication:
// Touch ID or the login password on macOS (LocalAuthentication), Windows Hello on Windows, Face ID on iOS. Asked at
// launch; asked again after the machine sleeps, the screen locks, the user session switches, the app goes to the
// background on a phone, or the window goes unused for the idle timeout (client policy `lock_idle_minutes`, shipped as
// 10). Nothing turns it off. docs/security.md says how it and the link's ssh authentication fit together.
//
// The rules alone, apart from the system: the platform's authenticator (client_lock_platform.h) and the OS's events
// come in as calls, time as an argument. No OS, no clock, no thread.
//
// Model: models/ClientLock.tla. tests/model/lock_walk.cpp drives these rules and writes traces that
// models/ClientLockTrace.tla checks against the model.

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace nexus {

enum class LockState {
    Locked,
    Prompting,      // the system's prompt is up for the current attempt
    Unlocked,
};

// Why the window is locked, which the lock screen says.
enum class LockReason {
    None,           // unlocked
    Launch,
    Sleep,          // the machine went to sleep
    ScreenLock,     // the screen locked
    SessionSwitch,  // another user session took the console
    Background,     // a phone's app went to the background
    Idle,           // the idle timeout
    Failed,         // the system refused the owner
    Cancelled,      // the prompt was cancelled
    Unavailable,    // the system has no way to authenticate the owner (no Windows Hello, no passcode)
};

// The authenticator's answer to an attempt.
enum class LockAnswer { Granted, Failed, Cancelled, Unavailable };

class ClientLock {
public:
    // `idleSeconds`: how long an unlocked window may go without input before it locks; at least 1.
    explicit ClientLock(double idleSeconds);

    // The lock screen asks the system: from Locked, a new attempt, whose number is returned for the authenticator to
    // answer with. Otherwise (a prompt is up, or unlocked) nothing, and 0.
    std::uint64_t prompt();

    // The authenticator's answer to attempt `attempt`, at `now`. Only the current attempt's, while it is asked, counts:
    // Granted unlocks (and the idle time starts at `now`); a refusal locks and says which. An answer to an older attempt,
    // or one a relock overtook, changes nothing. Returns whether it counted.
    bool answer(std::uint64_t attempt, LockAnswer a, double now);

    // An event of the OS that locks, from any state, a prompt that is up included.
    void relock(LockReason why);

    // Time is `now`: an unlocked window idle for the timeout locks.
    void tick(double now);

    // The person's input at `now`: true, and the idle time starts again, only while unlocked (after tick(now)).
    bool input(double now);

    // Whether a frame draws nexus's content; otherwise it draws the lock screen alone.
    bool drawsContent() const { return state_ == LockState::Unlocked; }

    LockState state() const { return state_; }
    LockReason reason() const { return reason_; }
    std::uint64_t attempt() const { return attempt_; }
    double idleSeconds() const { return idleSeconds_; }
    // A new timeout (the policy changed), at least 1 second; it applies from the next tick.
    void setIdleSeconds(double seconds) { idleSeconds_ = seconds < 1 ? 1 : seconds; }
    // Seconds since the last input, while unlocked; 0 otherwise.
    double idleFor(double now) const { return state_ == LockState::Unlocked ? now - lastInput_ : 0; }

private:
    void lock(LockReason why);

    double idleSeconds_;
    LockState state_ = LockState::Locked;
    LockReason reason_ = LockReason::Launch;
    std::uint64_t attempt_ = 0;
    double lastInput_ = 0;
};

// How the person asks to unlock: a key or a click on the Mac and Windows, a tap on a phone. Chosen at build time
// (kLockInput); the tests pass each one.
enum class LockInput { KeyOrClick, Tap };
#if defined(__APPLE__) && TARGET_OS_IPHONE
inline constexpr LockInput kLockInput = LockInput::Tap;
#else
inline constexpr LockInput kLockInput = LockInput::KeyOrClick;
#endif

// What the lock screen says: one line for the reason, and what to do. `method` names the system's authentication
// ("Touch ID or your password", "Windows Hello"). Nothing of nexus's content.
std::string lockTitle(LockState state);
std::string lockMessage(LockState state, LockReason why, const std::string& method, const std::string& detail,
                        LockInput input = kLockInput);

// The systems' codes, mapped to the lock's terms. Each platform file checks the numbers against its SDK
// (static_assert), so a change there fails the build rather than the lock.
//
// LocalAuthentication's LAError (lock_mac.mm): a failed check is Failed; the person, the system or nexus cancelling is
// Cancelled; no password or no way to show the prompt is Unavailable. Anything else is Failed: try again.
inline constexpr long kLocalAuthErrorAuthenticationFailed = -1;
inline constexpr long kLocalAuthErrorUserCancel = -2;
inline constexpr long kLocalAuthErrorUserFallback = -3;
inline constexpr long kLocalAuthErrorSystemCancel = -4;
inline constexpr long kLocalAuthErrorPasscodeNotSet = -5;
inline constexpr long kLocalAuthErrorAppCancel = -9;
inline constexpr long kLocalAuthErrorInvalidContext = -10;
inline constexpr long kLocalAuthErrorNotInteractive = -1004;
LockAnswer lockAnswerForLAError(long code);

// Windows' UserConsentVerificationResult (lock_win.cpp): Verified is Granted; no device, not set up for this user or
// turned off by policy is Unavailable; Canceled is Cancelled; busy, retries exhausted and anything else are Failed.
inline constexpr int kConsentVerified = 0;
inline constexpr int kConsentDeviceNotPresent = 1;
inline constexpr int kConsentNotConfiguredForUser = 2;
inline constexpr int kConsentDisabledByPolicy = 3;
inline constexpr int kConsentDeviceBusy = 4;
inline constexpr int kConsentRetriesExhausted = 5;
inline constexpr int kConsentCanceled = 6;
LockAnswer lockAnswerForConsent(int result);

// WM_WTSSESSION_CHANGE's event (lock_win.cpp): the console or a remote session disconnected or logged off is a session
// switch; the session locked is a screen lock; the rest (connect, logon, unlock, remote control) lock nothing: None.
inline constexpr unsigned long kWtsConsoleDisconnect = 0x2;
inline constexpr unsigned long kWtsRemoteDisconnect = 0x4;
inline constexpr unsigned long kWtsSessionLogoff = 0x6;
inline constexpr unsigned long kWtsSessionLock = 0x7;
LockReason lockReasonForSessionChange(unsigned long event);

// What arrives from other threads (the authenticator's reply, the OS's notices) for the window's thread, which takes it
// at its next wake. Every call is thread-safe.
struct LockNotice {
    enum class Kind { Answer, Relock } kind = Kind::Relock;
    std::uint64_t attempt = 0;          // with Answer
    LockAnswer answer = LockAnswer::Failed;
    LockReason why = LockReason::None;  // with Relock
    std::string detail;                 // with Answer: the system's own words for a refusal, for the lock screen
};

class LockMailbox {
public:
    void post(LockNotice notice);
    // Everything posted since the last take, in order.
    std::deque<LockNotice> take();

private:
    std::mutex mutex_;
    std::deque<LockNotice> notices_;
};

// Applies `notices` to `lock` at `now`, in order. The detail of the last refusal that counted is put in `detail`;
// a grant clears it. Returns whether anything counted.
bool applyLockNotices(ClientLock& lock, std::deque<LockNotice> notices, double now, std::string* detail);

} // namespace nexus
