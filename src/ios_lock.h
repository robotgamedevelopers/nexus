// The client lock on iOS and iPadOS (client_lock.h): what the app's lifecycle, the device's screen lock, the person's
// input and time mean to nexus::ClientLock, and what the lock asks of the system in return (the authenticator's prompt
// and its cancel, and a cover over the window for the app switcher's snapshot). The rules are ClientLock's
// (models/ClientLock.tla); this file only maps the system's events onto its operations, so a behaviour of this glue is a
// sequence of ClientLock's operations, and ClientLock's trace check (tests/model/lock_walk.cpp, ClientLockTrace.tla)
// covers it. No UIKit, no clock: main_ios.cpp gives it Prax's events and their times, lock_ios.mm the system's side.
//
// The mapping:
//   the app sent to the background                    relock(Background), cancelling a prompt that is up; covered
//   the app inactive (app switcher, Control Centre,    relock(SessionSwitch), covered; except while the app's own
//     a call) while unlocked                             prompt is up, which is what makes the app inactive then
//   the device's screen locked (protected data goes)  relock(ScreenLock), cancelling a prompt that is up
//   the app active again                              if a relock locked it (or it just launched), the prompt is
//                                                       asked for at once; after a refusal or the idle timeout the
//                                                       lock screen waits for a tap; the cover stays until a frame
//                                                       drawn since then has been presented (frameBegins)
//   the lock screen tapped (View::unlock)             prompt()
//   the authenticator's answer (LockNotice)           answer(attempt, answer, now)
//   time (a timer each second)                        tick(now)
//   the person's input                                input(now): it reaches nexus only if that says so; dropped,
//                                                       never queued, otherwise
//   a frame                                           drawsContent(): nexus's content, or the lock screen alone

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "client_lock.h"
#include "pr/surface.h"

namespace nexus {

// The app's state with the person, as Prax's PrAppLifecycle says it.
enum class IosLifecycle { Active, Inactive, Background };

// Each of ClientLock's operations the glue made, as models/ClientLock.tla names them, for a trace
// (tests/model/ios_lock_walk.cpp): the operation, its attempt (prompt: the one made, 0 for none; answer: the one
// answered), its answer or reason, the time it was made at, and the code's answer (frame: drew the content; input:
// delivered).
struct IosLockStep {
    enum class Op { Prompt, Answer, Relock, Tick, Input, Frame } op = Op::Frame;
    std::uint64_t attempt = 0;
    LockAnswer answer = LockAnswer::Failed;
    LockReason why = LockReason::None;
    double now = 0;
    bool flag = false;
};

// What the glue asks of the system (lock_ios.mm in the app; recorders in the tests).
struct IosLockSystem {
    std::function<void(std::uint64_t attempt)> request;  // show the system's prompt for `attempt`
    std::function<void()> cancel;                         // take down a prompt that is up
    std::function<void(bool)> cover;                      // cover the window (true) or uncover it
    std::function<void(const IosLockStep&)> step;         // each ClientLock operation made; none in the app
};

class IosLock {
public:
    IosLock(ClientLock& lock, IosLockSystem system) : lock_(lock), system_(std::move(system)) {}

    // The app's lifecycle changed to `now`.
    void lifecycle(IosLifecycle now);

    // The device's screen locked (UIApplicationProtectedDataWillBecomeUnavailable).
    void screenLocked();

    // The lock screen was tapped: ask the system.
    void unlock();

    // The authenticator's answers and the system's notices, at `now`. The detail of the last refusal that counted is
    // kept for the lock screen.
    void notices(std::deque<LockNotice> notices, double now);

    // Time is `now`.
    void tick(double now);

    // The person's input at `now`: whether it reaches nexus. While locked it does not, and is dropped. Only
    // deliberate input (frame_host.h resetsIdle: a key, text, a press, a scroll, a paste, a drop) starts the idle time
    // again; other input (a pointer or a drag passing over, a gesture) only lets time pass, as on the desktop
    // (window_lock.cpp), so that nothing but the person's act keeps nexus unlocked.
    bool input(double now, bool deliberate = true);

    // A frame: whether it draws nexus's content (unlocked), or the lock screen alone.
    bool frame();

    // A frame begins (main_ios.cpp beginFrame, after the swap chain's wait, so the frame before it has been presented).
    // The cover put on when the app left is taken away only at the second frame to begin since the app became active
    // again: the window's layer still holds the last frame drawn before the relock (no frame is drawn in the
    // background), and uncovered at once it would show for a frame, under the system's prompt.
    void frameBegins();

    bool drawsContent() const { return lock_.drawsContent(); }
    bool covered() const { return covered_; }
    const std::string& detail() const { return detail_; }
    const ClientLock& lock() const { return lock_; }

private:
    void relock(LockReason why);
    void setCover(bool on);
    void step(IosLockStep s) const {
        if (system_.step) system_.step(s);
    }

    ClientLock& lock_;
    IosLockSystem system_;
    IosLifecycle lifecycle_ = IosLifecycle::Inactive;  // until the app says (main_ios.cpp open)
    bool covered_ = false;
    bool uncoverPending_ = false;        // active again while covered: uncovered once a frame since has been presented
    bool drawnSinceActive_ = false;      // a frame has begun since the app became active again
    double lastTick_ = 0;                // the time of the last tick: time starts after 0
    std::string detail_;
};

// Whether a lock with this reason asks the system as soon as the app is active: at launch and after a relock, yes; after
// a refusal (the person said no, or there is no way) or the idle timeout, no: the lock screen waits for a tap, so that a
// refusal is never answered with the same prompt in a loop.
bool promptsWhenActive(LockReason why);

// Covers the window behind `surface` (a UIView) with an opaque view above its drawing, or takes the cover away
// (lock_ios.mm). The app switcher's snapshot is of what shows once the app is in the background: covered, it shows
// nothing of nexus.
void iosCoverWindow(const PrSurface& surface, bool on);

// A URL opener that opens nothing while `lock` does not draw nexus's content, and logs that it did not: a page's link
// (the board's, a transcript's) that arrives while the app is locked is dropped, never kept to open later (as the
// desktop's window_lock.h openUnlessLocked).
std::function<void(const std::string&)> openWhileUnlocked(const ClientLock& lock,
                                                          std::function<void(const std::string&)> open,
                                                          std::function<void(const std::string&)> log);

} // namespace nexus
