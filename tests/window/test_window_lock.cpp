// Unit tests of the window's side of the client lock (src/window_lock.h): what the app does when the lock locks, unlocks,
// asks, and takes input, over recording hooks and a fake authenticator. No window, no OS, no clock (time is an argument).
// Also the idle timeout's limits (window_app.h lockIdleSecondsFrom), and no sound while locked (View::chirp).

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "frame_host.h"
#include "window_app.h"
#include "window_lock.h"

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

constexpr double kIdle = 10;

struct FakeAuthenticator final : LockAuthenticator {
    std::vector<std::uint64_t>* requests;
    int* cancels;
    FakeAuthenticator(std::vector<std::uint64_t>* r, int* c) : requests(r), cancels(c) {}
    std::string method() const override { return "Touch ID or your password"; }
    void request(std::uint64_t attempt, const PrSurface&, LockPost) override { requests->push_back(attempt); }
    void cancel() override { ++*cancels; }
};

// The window as the hooks see it, and what was done to it.
struct Window {
    bool overlay;
    bool visible = false;
    std::optional<View::LockScreen> screen;
    std::vector<std::uint64_t> requests;
    int cancels = 0, hides = 0, reveals = 0, veils = 0, signInLocks = 0, signInShows = 0, frames = 0;
    std::vector<std::string> log;
    std::unique_ptr<WindowLock> lock;

    explicit Window(bool isOverlay) : overlay(isOverlay) {
        WindowLock::Hooks h;
        h.overlay = overlay;
        h.visible = [this] { return visible; };
        h.hide = [this] { ++hides; visible = false; };
        h.reveal = [this] { ++reveals; visible = true; };
        h.veil = [this] { ++veils; };
        h.screen = [this](std::optional<View::LockScreen> s) { screen = std::move(s); };
        h.signInLock = [this] { ++signInLocks; };
        h.signInShown = [this] { ++signInShows; };
        h.frame = [this] { ++frames; };
        h.log = [this](const std::string& line) { log.push_back(line); };
        lock = std::make_unique<WindowLock>(kIdle, std::make_unique<FakeAuthenticator>(&requests, &cancels), h);
    }

    void grant(double now) {
        lock->notices({LockNotice{LockNotice::Kind::Answer, lock->lock().attempt(), LockAnswer::Granted,
                                  LockReason::None, ""}}, now);
    }
    // Asked, granted, and shown.
    void unlock(double now) {
        lock->ask();
        grant(now);
        visible = true;
    }
};

PrAppEvent event(PrAppEventKind kind) {
    PrAppEvent e{};
    e.kind = kind;
    return e;
}

// Launch: the lock screen and nothing else; one prompt at a time, over it.
void launchShowsTheLockScreenAndAsksOnce() {
    Window w(true);
    CHECK(w.screen && w.screen->title == "nexus is locked");
    CHECK(!w.lock->drawsContent());
    w.lock->shown();
    CHECK(w.requests.size() == 1 && w.requests[0] == 1);
    CHECK(w.screen && w.screen->title == "nexus is waiting for you");
    w.lock->ask();
    w.lock->shown();
    CHECK(w.requests.size() == 1);
}

// After a refusal the lock screen waits for the person: showing does not ask, Return or a click (ask) does.
void refusalWaitsForThePerson() {
    Window w(true);
    w.lock->ask();
    w.lock->notices({LockNotice{LockNotice::Kind::Answer, 1, LockAnswer::Cancelled, LockReason::None, "Canceled."}}, 1);
    CHECK(w.lock->lock().state() == LockState::Locked && w.lock->lock().reason() == LockReason::Cancelled);
    CHECK(w.screen && w.screen->message.find("Canceled.") != std::string::npos);
    CHECK(!w.log.empty() && w.log.back().find("not unlocked") == 0);
    w.lock->shown();
    CHECK(w.requests.size() == 1);
    w.lock->ask();
    CHECK(w.requests.size() == 2);
}

// The grant unlocks: the screen goes, and the overlay the prompt hid shows again; a shown one allows a sign-in.
void grantUnlocksAndShows() {
    Window hidden(true);
    hidden.lock->ask();
    hidden.grant(1);
    CHECK(hidden.lock->drawsContent() && !hidden.screen);
    CHECK(hidden.reveals == 1 && hidden.signInShows == 0);
    Window shown(true);
    shown.visible = true;
    shown.lock->ask();
    shown.grant(1);
    CHECK(shown.reveals == 0 && shown.signInShows == 1);
    CHECK(shown.log.back() == "unlocked");
}

// Every relock from unlocked: the lock screen, the sign-in taken back, and the overlay hidden at once.
void relockHidesTheOverlayAndTakesBackSignIn() {
    for (const auto& e : {event(PrAppEventKind_Suspend), event(PrAppEventKind_ScreenLocked)}) {
        Window w(true);
        w.unlock(0);
        w.lock->relock(relockFor(e));
        CHECK(!w.lock->drawsContent() && w.screen);
        CHECK(w.hides == 1 && w.veils == 0);
        CHECK(w.signInLocks == 1);
        CHECK(w.log.back().find("locked: ") == 0);
    }
    for (const auto why : {LockReason::SessionSwitch, LockReason::Background}) {
        Window w(true);
        w.unlock(0);
        w.lock->relock(why);
        CHECK(w.hides == 1 && w.signInLocks == 1 && w.lock->lock().reason() == why);
    }
}

// A normal window is veiled at the relock, before anything else can draw or sleep, and is never hidden.
void relockVeilsANormalWindow() {
    Window w(false);
    w.unlock(0);
    w.lock->relock(LockReason::Sleep);
    CHECK(w.veils == 1 && w.hides == 0);
    CHECK(w.screen && w.screen->message.find("went to sleep") != std::string::npos);
    // Already locked: nothing more to take off the screen.
    w.lock->relock(LockReason::ScreenLock);
    CHECK(w.veils == 1);
    // The idle timeout too.
    Window idle(false);
    idle.unlock(0);
    idle.lock->tick(kIdle);
    CHECK(idle.veils == 1 && idle.lock->lock().reason() == LockReason::Idle);
}

// A relock that overtakes the system's prompt takes it away, and its late grant unlocks nothing.
void relockCancelsThePrompt() {
    Window w(true);
    w.lock->ask();
    w.lock->relock(LockReason::ScreenLock);
    CHECK(w.cancels == 1);
    w.lock->notices({LockNotice{LockNotice::Kind::Answer, 1, LockAnswer::Granted, LockReason::None, ""}}, 1);
    CHECK(!w.lock->drawsContent());
    w.lock->cancel();
    CHECK(w.cancels == 2);
}

// Prax's app events: Suspend is sleep, ScreenLocked a screen lock, the rest nothing; relock(None) is still a lock.
void relocksFromAppEvents() {
    CHECK(relockFor(event(PrAppEventKind_Suspend)) == LockReason::Sleep);
    CHECK(relockFor(event(PrAppEventKind_ScreenLocked)) == LockReason::ScreenLock);
    CHECK(relockFor(event(PrAppEventKind_Wake)) == LockReason::None);
    CHECK(relockFor(event(PrAppEventKind_Timer)) == LockReason::None);
    CHECK(relockFor(event(PrAppEventKind_KeyDown)) == LockReason::None);
    Window w(true);
    w.unlock(0);
    w.lock->relock(LockReason::None);
    CHECK(!w.lock->drawsContent() && w.lock->lock().reason() == LockReason::Launch && w.hides == 1);
}

// Only deliberate input starts the idle time again: a key, text, a press, a scroll, a paste, a drop. A pointer or a
// drag passing over, a gesture or a trackpad touch does not, and after the timeout it locks before it is delivered.
void onlyDeliberateInputKeepsItUnlocked() {
    const PrAppEventKind deliberate[] = {PrAppEventKind_KeyDown, PrAppEventKind_Text, PrAppEventKind_PointerDown,
                                         PrAppEventKind_Scroll, PrAppEventKind_Paste, PrAppEventKind_Drop};
    for (const PrAppEventKind kind : deliberate) {
        Window w(true);
        w.unlock(0);
        w.lock->input(event(kind), kIdle - 1);
        w.lock->tick(kIdle);
        CHECK(w.lock->drawsContent());
        CHECK(resetsIdle(event(kind)));
    }
    const PrAppEventKind passing[] = {PrAppEventKind_PointerMove, PrAppEventKind_PointerEnter,
                                      PrAppEventKind_PointerLeave, PrAppEventKind_DragEnter, PrAppEventKind_DragOver,
                                      PrAppEventKind_Gesture, PrAppEventKind_TrackpadTouches, PrAppEventKind_KeyUp,
                                      PrAppEventKind_PointerUp};
    for (const PrAppEventKind kind : passing) {
        Window w(true);
        w.unlock(0);
        w.lock->input(event(kind), kIdle - 1);
        w.lock->tick(kIdle);
        CHECK(!w.lock->drawsContent());
        CHECK(!resetsIdle(event(kind)));
        Window late(true);
        late.unlock(0);
        late.lock->input(event(kind), kIdle);
        CHECK(!late.lock->drawsContent() && late.lock->lock().reason() == LockReason::Idle && late.hides == 1);
    }
    // Deliberate input after the timeout locks too.
    Window late(true);
    late.unlock(0);
    late.lock->input(event(PrAppEventKind_KeyDown), kIdle + 1);
    CHECK(!late.lock->drawsContent());
    // The app's own events are no input and change nothing.
    Window w(true);
    w.unlock(0);
    w.lock->input(event(PrAppEventKind_Timer), kIdle + 5);
    CHECK(w.lock->drawsContent());
}

// A link that arrives while locked is dropped, said in the log, and never opened later.
void linksWhileLockedAreNotOpened() {
    bool locked = true;
    std::vector<std::string> opened, log;
    const auto open = openUnlessLocked([&](const std::string& url) { opened.push_back(url); },
                                       [&] { return locked; }, [&](const std::string& line) { log.push_back(line); });
    open("https://example.invalid/a");
    CHECK(opened.empty() && log.size() == 1);
    locked = false;
    open("https://example.invalid/b");
    CHECK(opened.size() == 1 && opened[0] == "https://example.invalid/b");
}

// No sound while locked: no cue says what an AI did.
void silentWhileLocked() {
    View view;
    int played = 0;
    view.playChirp = [&](const Chirp&) { ++played; };
    view.lockScreen = View::LockScreen{"nexus is locked", ""};
    view.chirp(Chirp{});
    CHECK(played == 0);
    view.lockScreen.reset();
    view.chirp(Chirp{});
    CHECK(played == 1);
}

// The idle timeout: a whole number of minutes from 1 to 240; anything else, the default, with a reason; it cannot be
// turned off.
void idleTimeoutLimits() {
    std::string why;
    CHECK(lockIdleSecondsFrom(R"({"idle_minutes": 1})", why) == 60 && why.empty());
    CHECK(lockIdleSecondsFrom(R"({"idle_minutes": 240})", why) == 240 * 60 && why.empty());
    CHECK(lockIdleSecondsFrom(R"({"idle_minutes": 10})", why) == 600 && why.empty());
    CHECK(lockIdleSecondsFrom("", why) == kLockIdleMinutesDefault * 60 && why.empty());
    CHECK(lockIdleSecondsFrom("null", why) == kLockIdleMinutesDefault * 60 && why.empty());
    for (const char* off : {R"({"idle_minutes": 0})", R"({"idle_minutes": 241})", R"({"idle_minutes": -1})",
                            R"({"idle_minutes": 1.5})", R"({"idle_minutes": false})", R"({"idle_minutes": null})",
                            R"({"idle_minutes": 1e9})", R"({})", "[", R"("off")"}) {
        std::string reason;
        CHECK(lockIdleSecondsFrom(off, reason) == kLockIdleMinutesDefault * 60);
        CHECK(reason.find("cannot be turned off") != std::string::npos);
    }
}

} // namespace

int main() {
    launchShowsTheLockScreenAndAsksOnce();
    refusalWaitsForThePerson();
    grantUnlocksAndShows();
    relockHidesTheOverlayAndTakesBackSignIn();
    relockVeilsANormalWindow();
    relockCancelsThePrompt();
    relocksFromAppEvents();
    onlyDeliberateInputKeepsItUnlocked();
    linksWhileLockedAreNotOpened();
    silentWhileLocked();
    idleTimeoutLimits();
    if (failures != 0) {
        std::fprintf(stderr, "window_lock_unit: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("window_lock_unit: all passed\n");
    return 0;
}
