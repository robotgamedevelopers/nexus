// Unit tests of the client lock's iOS part (src/ios_lock.h) over the real nexus::ClientLock (src/client_lock.h): what
// the app's lifecycle, the device's screen lock, the lock screen's tap, the authenticator's answers, time and input do
// to the lock, and what the glue asks of the system (a prompt, its cancel, the cover). The system is a recorder; time
// is an argument. No UIKit, no clock, no sockets. Run on a simulator device like the other iOS tests.
//
// Each test was seen red with a fault planted in src/ios_lock.cpp (the pull request lists them):
//   own_prompt_does_not_relock               Inactive relocks and covers while the app's own prompt is up
//   inactive_while_unlocked_relocks           Inactive while unlocked does not relock
//   background_overtakes_the_prompt           Background does not cancel the prompt that is up
//   screen_lock_relocks                       screenLocked() does nothing
//   refusal_waits_for_a_tap                   promptsWhenActive(Failed) is true
//   input_reaches_nexus_only_unlocked         input() returns true while locked
//   idle_timeout_locks                        tick() does nothing
//   passing_input_keeps_nothing_unlocked      input(now, false) starts the idle time again
//   no_prompt_unless_active                   unlock() does not check that the app is active
//   active_uncovers                           Active does not take the cover away
//   launch_asks_once_active                   the first Active does not ask
//
// Usage: ios_lock_unit

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "client_lock.h"
#include "ios_lock.h"

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

constexpr double kIdle = 600;   // the shipped ten minutes

// What the glue asked of the system.
struct System {
    std::vector<std::uint64_t> requests;
    int cancels = 0;
    std::vector<bool> covers;

    IosLockSystem make() {
        IosLockSystem s;
        s.request = [this](std::uint64_t attempt) { requests.push_back(attempt); };
        s.cancel = [this] { ++cancels; };
        s.cover = [this](bool on) { covers.push_back(on); };
        return s;
    }
};

std::deque<LockNotice> answer(std::uint64_t attempt, LockAnswer a, std::string detail = {}) {
    std::deque<LockNotice> d;
    d.push_back({LockNotice::Kind::Answer, attempt, a, LockReason::None, std::move(detail)});
    return d;
}

// A lock unlocked by attempt 1's grant at `at`, the app active.
void unlockedAt(IosLock& glue, double at) {
    glue.lifecycle(IosLifecycle::Active);
    glue.notices(answer(1, LockAnswer::Granted), at);
}

void launch_asks_once_active() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    // Launched: locked, nothing drawn, nothing asked until the app is active.
    CHECK(!glue.drawsContent());
    CHECK(lock.state() == LockState::Locked && lock.reason() == LockReason::Launch);
    glue.lifecycle(IosLifecycle::Inactive);
    CHECK(sys.requests.empty());
    glue.lifecycle(IosLifecycle::Active);
    CHECK(sys.requests == std::vector<std::uint64_t>{1});
    CHECK(lock.state() == LockState::Prompting);
    CHECK(!glue.drawsContent());
    glue.notices(answer(1, LockAnswer::Granted), 10);
    CHECK(glue.drawsContent());
    CHECK(glue.input(11));
}

void own_prompt_does_not_relock() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    glue.lifecycle(IosLifecycle::Active);
    // The system's Face ID sheet makes the app inactive while it is up: not a relock, and nothing covered.
    glue.lifecycle(IosLifecycle::Inactive);
    CHECK(lock.state() == LockState::Prompting);
    CHECK(sys.cancels == 0);
    CHECK(!glue.covered());
    glue.notices(answer(1, LockAnswer::Granted), 5);
    CHECK(glue.drawsContent());
    glue.lifecycle(IosLifecycle::Active);
    CHECK(glue.drawsContent());
    CHECK(sys.requests.size() == 1);
}

void inactive_while_unlocked_relocks() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    unlockedAt(glue, 5);
    // The app switcher, Control Centre or a call: locked and covered before the system's snapshot.
    glue.lifecycle(IosLifecycle::Inactive);
    CHECK(!glue.drawsContent());
    CHECK(lock.reason() == LockReason::SessionSwitch);
    CHECK(glue.covered());
    // Back: uncovered, and asked again at once.
    glue.lifecycle(IosLifecycle::Active);
    CHECK(!glue.covered());
    CHECK(sys.requests == (std::vector<std::uint64_t>{1, 2}));
}

void background_overtakes_the_prompt() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    glue.lifecycle(IosLifecycle::Active);
    glue.lifecycle(IosLifecycle::Inactive);   // the prompt's sheet
    glue.lifecycle(IosLifecycle::Background); // the person went home with the prompt up
    CHECK(lock.state() == LockState::Locked && lock.reason() == LockReason::Background);
    CHECK(sys.cancels == 1);
    CHECK(glue.covered());
    // The overtaken attempt's grant, arriving late, unlocks nothing.
    glue.notices(answer(1, LockAnswer::Granted), 7);
    CHECK(!glue.drawsContent());
    glue.lifecycle(IosLifecycle::Active);
    CHECK(sys.requests == (std::vector<std::uint64_t>{1, 2}));
    glue.notices(answer(2, LockAnswer::Granted), 8);
    CHECK(glue.drawsContent());
}

void screen_lock_relocks() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    unlockedAt(glue, 5);
    glue.screenLocked();
    CHECK(!glue.drawsContent());
    CHECK(lock.reason() == LockReason::ScreenLock);
    // With a prompt up, the screen lock takes it down too.
    glue.unlock();
    CHECK(lock.state() == LockState::Prompting);
    glue.screenLocked();
    CHECK(lock.state() == LockState::Locked && sys.cancels == 1);
}

void refusal_waits_for_a_tap() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    glue.lifecycle(IosLifecycle::Active);
    glue.lifecycle(IosLifecycle::Inactive);
    glue.notices(answer(1, LockAnswer::Failed, "Face not recognised"), 3);
    CHECK(lock.state() == LockState::Locked && lock.reason() == LockReason::Failed);
    CHECK(glue.detail() == "Face not recognised");
    // The sheet goes and the app is active again: the lock screen says why and waits, never prompting in a loop.
    glue.lifecycle(IosLifecycle::Active);
    CHECK(sys.requests.size() == 1);
    glue.unlock();
    CHECK(sys.requests == (std::vector<std::uint64_t>{1, 2}));
    glue.notices(answer(2, LockAnswer::Cancelled), 4);
    CHECK(lock.reason() == LockReason::Cancelled);
    glue.lifecycle(IosLifecycle::Inactive);
    glue.lifecycle(IosLifecycle::Active);
    CHECK(sys.requests.size() == 2);
    // A grant clears the refusal's words.
    glue.unlock();
    glue.notices(answer(3, LockAnswer::Granted), 5);
    CHECK(glue.drawsContent() && glue.detail().empty());
}

void input_reaches_nexus_only_unlocked() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    // Locked, and while the prompt is up: dropped, each one.
    CHECK(!glue.input(1));
    glue.lifecycle(IosLifecycle::Active);
    CHECK(!glue.input(2));
    CHECK(!glue.input(3));
    glue.notices(answer(1, LockAnswer::Granted), 4);
    // Unlocked: the next input reaches nexus; none of those dropped comes after it (there is nothing held to replay).
    CHECK(glue.input(5));
    glue.screenLocked();
    CHECK(!glue.input(6));
}

void idle_timeout_locks() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    unlockedAt(glue, 100);
    glue.tick(100 + kIdle - 1);
    CHECK(glue.drawsContent());
    CHECK(glue.input(100 + kIdle - 1));      // input starts the idle time again
    glue.tick(100 + 2 * kIdle - 2);
    CHECK(glue.drawsContent());
    glue.tick(100 + 2 * kIdle - 1);
    CHECK(!glue.drawsContent());
    CHECK(lock.reason() == LockReason::Idle);
    // Input past the timeout, with no tick between, is dropped and finds it locked.
    ClientLock lock2(kIdle);
    System sys2;
    IosLock glue2(lock2, sys2.make());
    unlockedAt(glue2, 0);
    CHECK(!glue2.input(kIdle));
    CHECK(lock2.reason() == LockReason::Idle);
    // The idle lock waits for a tap: the app was active all along.
    CHECK(sys2.requests.size() == 1);
}

// A pointer passing over the window (an iPad's trackpad), a drag or a gesture is input that is not deliberate: it reaches
// nexus while unlocked, but never starts the idle time again, so it never keeps nexus unlocked (as window_lock.cpp).
void passing_input_keeps_nothing_unlocked() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    unlockedAt(glue, 100);
    CHECK(glue.input(100 + kIdle - 1, false));   // delivered, but the idle time runs on from 100
    CHECK(glue.drawsContent());
    CHECK(!glue.input(100 + kIdle, false));      // the timeout: dropped, and locked
    CHECK(!glue.drawsContent());
    CHECK(lock.reason() == LockReason::Idle);
}

void no_prompt_unless_active() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    glue.lifecycle(IosLifecycle::Background);
    glue.unlock();
    CHECK(sys.requests.empty());
    CHECK(lock.state() == LockState::Locked);
}

void active_uncovers() {
    ClientLock lock(kIdle);
    System sys;
    IosLock glue(lock, sys.make());
    glue.lifecycle(IosLifecycle::Background);
    CHECK(glue.covered());
    CHECK(sys.covers == std::vector<bool>{true});
    glue.lifecycle(IosLifecycle::Active);
    CHECK(!glue.covered());
    CHECK(sys.covers == (std::vector<bool>{true, false}));
}

void reasons_that_prompt() {
    CHECK(promptsWhenActive(LockReason::Launch));
    CHECK(promptsWhenActive(LockReason::Background));
    CHECK(promptsWhenActive(LockReason::SessionSwitch));
    CHECK(promptsWhenActive(LockReason::ScreenLock));
    CHECK(promptsWhenActive(LockReason::Sleep));
    CHECK(!promptsWhenActive(LockReason::Idle));
    CHECK(!promptsWhenActive(LockReason::Failed));
    CHECK(!promptsWhenActive(LockReason::Cancelled));
    CHECK(!promptsWhenActive(LockReason::Unavailable));
    CHECK(!promptsWhenActive(LockReason::None));
}

} // namespace

int main() {
    launch_asks_once_active();
    own_prompt_does_not_relock();
    inactive_while_unlocked_relocks();
    background_overtakes_the_prompt();
    screen_lock_relocks();
    refusal_waits_for_a_tap();
    input_reaches_nexus_only_unlocked();
    idle_timeout_locks();
    passing_input_keeps_nothing_unlocked();
    no_prompt_unless_active();
    active_uncovers();
    reasons_that_prompt();
    if (failures != 0) {
        std::fprintf(stderr, "ios_lock_unit: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ios_lock_unit: all passed\n");
    return 0;
}
