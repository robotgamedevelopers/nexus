// The window's gate for in-app sign-in when nexus-server runs on another machine (docs/secrets.md "Sign-in"): the
// browser's redirect after the person signs in comes back to this machine's loopback, where Claude Code's listener is
// not, so the window listens there for it, for one flow, once.
//
// A thread of its own binds the redirect's port on 127.0.0.1 and on ::1, since a browser may try `localhost` there
// first: both, or the gate fails with a plain line. The one exception is a machine with no IPv6 loopback at all (the
// family or the address not there), where no browser can reach ::1 either. It feeds each connection's bytes to
// sync::CallbackGate: any request that is not the one
// callback (its path, its state) is answered "not found" and changes nothing; the one is answered with a plain page
// ("signed in, you can close this tab"), its query kept for the frame thread to take, and the gate closes. Nothing
// it reads is logged or kept past that: the connection's bytes are wiped, and the query lives in the gate until taken.
//
// Prax's net (pinned) connects but does not listen, so the sockets are POSIX's, and Winsock's on Windows. The frame
// thread never waits on the gate: take() and failed() look under a mutex the thread holds only briefly, and closing
// the gate (its destructor) only asks the thread to stop; it does within kPollMs, on its own.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sync/secrets.h"
#include "sync/signin.h"

namespace nexus {

// This machine's host name, for sync::sameMachine; empty when it cannot be read.
std::string localHostName();
// A new claim token for a window (sync::isClaimToken): 32 letters and digits from the system's random source.
std::string newClaimToken();

class SignInGate {
public:
    // Starts the gate's thread for `target`; it binds at once, and failed() says if it could not.
    explicit SignInGate(sync::SignInTarget target);
    ~SignInGate();
    SignInGate(const SignInGate&) = delete;
    SignInGate& operator=(const SignInGate&) = delete;

    // The callback's query, once the gate took it: true the one time, the query moved out. Never waits.
    bool take(sync::SecretValue* query);
    // True, with why, when the gate could not listen (the port is in use, say) or stopped on an error. Never waits.
    bool failed(std::string* why);

    // How long the thread's poll waits before it looks whether it was asked to stop. Not measured: short enough that a
    // closed gate's port is free again well within a person's next try, long enough to cost nothing.
    static constexpr int kPollMs = 100;
    // How many connections it reads at once (a browser opens a few to one host, some of which never send), and how
    // long one is kept from its accept, whatever it sends: sync::GateSlots.
    static constexpr std::size_t kConnections = sync::GateSlots::kConnections;
    static constexpr std::uint64_t kConnectionMs = sync::GateSlots::kConnectionMs;

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace nexus
