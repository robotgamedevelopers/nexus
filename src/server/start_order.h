// The order nexus-server starts in (server/server.cpp Server::run), models/store/StoreLock.tla step for step:
//
//   Probe    another server answers on the control socket: this one is refused before it touches the store (a server
//            from before the store's lock holds none, so the lock alone would not keep it out)
//   Open     the store (store::Store::open): its lock first, exclusive and never waiting on a writer, before anything
//            in the store is read; then the replay. Refused when another process holds the lock
//   Listen   the control socket: refused when a server answers on it; else bound
//   Restore  the sessions settled against the accounts and the friends' drivers restored, which may commit to the
//            journal and start a driver for a queued prompt: only now, with the lock and the socket both held
//   Serve    the loop
//
// A refused start (Refused) is another server's store or socket: the process says so in one line and exits
// kRefusedExit, 0, which a service manager does not restart (launchd's KeepAlive with SuccessfulExit false, systemd's
// Restart=on-failure); a failed one (Failed) exits 1. Nothing writes to the journal or starts a driver before Restore.
//
// Fault builds a broken order for the trace binding's negative checks (tests/store/store_lock_walk.cpp), which TLC must
// reject; the server never sets one:
//   Unlocked      the order before the lock: the store opened with no lock, the drivers restored, the socket last
//   RestoreFirst  the lock kept, but the drivers restored before the socket is held

#pragma once

#include <cstddef>
#include <vector>

namespace nexus::server {

constexpr int kRefusedExit = 0;
constexpr int kFailedExit = 1;

class StartOrder {
public:
    enum class Step { Probe, Open, OpenUnlocked, Listen, Restore, Serve, Refused, Failed };
    // Ok: the step was taken. Held: another server holds what it needed (the lock, the socket). Failed: anything else.
    enum class Outcome { Ok, Held, Failed };
    enum class Fault { None, Unlocked, RestoreFirst };

    explicit StartOrder(Fault fault = Fault::None) {
        if (fault == Fault::Unlocked) {
            order_ = {Step::OpenUnlocked, Step::Restore, Step::Listen};
        } else if (fault == Fault::RestoreFirst) {
            order_ = {Step::Probe, Step::Open, Step::Restore, Step::Listen};
        } else {
            order_ = {Step::Probe, Step::Open, Step::Listen, Step::Restore};
        }
    }

    Step next() const {
        if (end_ != Step::Serve) {
            return end_;
        }
        return at_ < order_.size() ? order_[at_] : Step::Serve;
    }

    // The outcome of the step next() gave. Held is a refusal only where another server can hold what the step takes
    // (Probe, Open, Listen); from Restore it is a failure. Nothing changes once the start has ended.
    void done(Outcome outcome) {
        const Step step = next();
        if (step == Step::Serve || step == Step::Refused || step == Step::Failed) {
            return;
        }
        if (outcome == Outcome::Ok) {
            ++at_;
            return;
        }
        const bool holdable = step == Step::Probe || step == Step::Open || step == Step::OpenUnlocked ||
            step == Step::Listen;
        end_ = outcome == Outcome::Held && holdable ? Step::Refused : Step::Failed;
    }

private:
    std::vector<Step> order_;
    std::size_t at_ = 0;
    Step end_ = Step::Serve;    // Serve while the start runs; Refused or Failed once it ended so
};

} // namespace nexus::server
