// The window's side of the client lock (client_lock.h): what the app does around the lock's rules when the system
// answers, an event relocks, the idle time runs out, the person's input arrives, or the window shows. main.cpp gives it
// the window through Hooks; tests/window/test_window_lock.cpp gives it recorders. No OS, no clock (time is an
// argument), no window.
//
// Locking from unlocked takes the content off the screen at once: the overlay hides; a normal window (--windowed) is
// veiled (its opacity to 0, main.cpp) until the lock screen's frame is drawn, so that no old frame stays in a capture,
// the Dock's thumbnail or Mission Control. It takes back the sign-in a show allowed (frame_host.h LinkSignIn::lock), and
// a system prompt that is still up. Unlocking reveals a hidden overlay; on macOS it stays hidden until this grant.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "pr/app.h"

#include "client_lock.h"
#include "client_lock_platform.h"
#include "view.h"

namespace nexus {

class WindowLock {
public:
    struct Hooks {
        bool overlay = true;                             // the overlay, or a normal window (--windowed)
        std::function<bool()> visible;                   // the window is on screen
        std::function<void()> hide;                      // the overlay hides at once
        std::function<void()> reveal;                    // the overlay shows again (the hotkey's rules)
        std::function<void()> veil;                      // a normal window's content off the screen until the next frame
        std::function<void(std::optional<View::LockScreen>)> screen;   // the view's lock screen, or none
        std::function<void()> signInLock;                // LinkSignIn::lock
        std::function<void()> signInShown;               // LinkSignIn::shown
        std::function<void()> frame;                     // ask for a frame
        std::function<PrSurface()> surface;              // the window's surface, for the system's prompt
        LockPost post;                                   // the system's answers, from any thread, into the app's inbox
        std::function<void(const std::string&)> log;     // one line for the log
    };

    WindowLock(double idleSeconds, std::unique_ptr<LockAuthenticator> authenticator, Hooks hooks);

    const ClientLock& lock() const { return lock_; }
    bool drawsContent() const { return lock_.drawsContent(); }
    const std::string& method() const { return method_; }
    void setIdleSeconds(double seconds) { lock_.setIdleSeconds(seconds); }

    // A deliberate show request: check idle first, then authenticate while hidden if needed.
    // True only when content may be revealed; a current grant invokes the reveal hook.
    bool requestReveal(double now);
    // Asks the system: only from locked, so never twice at once.
    void ask();
    // The window finished showing (the overlay's reveal): asks, unless a refusal waits for the person to read it.
    void shown();
    // The notices other threads posted (the system's answers, its events).
    void notices(std::deque<LockNotice> notices, double now);
    // A relock event (sleep, screen lock, session switch, the background).
    void relock(LockReason why);
    // The idle check (a timer, a frame, and before the overlay shows).
    void tick(double now);
    // The person's input, before it is delivered: deliberate input (frame_host.h resetsIdle) starts the idle time
    // again; any input after the timeout locks first, so that it reaches only the lock screen.
    void input(const PrAppEvent& e, double now);
    // Shutdown: the system's prompt is taken away.
    void cancel();
    // The view's lock screen from the lock's state now.
    void showScreen();

private:
    void changed(LockState before);

    ClientLock lock_;
    std::unique_ptr<LockAuthenticator> authenticator_;
    Hooks hooks_;
    std::string method_;
    std::string detail_;
};

// The relock that Prax's own app events mean: Suspend is sleep, ScreenLocked is a screen lock; None for the rest.
LockReason relockFor(const PrAppEvent& e);

// The relock reasons that ask by themselves when the window shows; after a refusal the lock screen waits for Return or a
// click, so that its words can be read.
bool asksOnShow(LockReason why);

const char* lockReasonName(LockReason why);

// A URL opener that opens nothing while `locked` says so, and logs that it did not: a page's link, or a sign-in's, that
// arrives while the window is locked is dropped, never kept to open later.
std::function<void(const std::string&)> openUnlessLocked(std::function<void(const std::string&)> open,
                                                         std::function<bool()> locked,
                                                         std::function<void(const std::string&)> log);

} // namespace nexus
