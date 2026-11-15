// The order nexus-server starts in (server/server.cpp Server::run, through take_step below), models/store/StoreLock.tla
// step for step:
//
//   Probe    another server answers on the control socket: this one is refused before it touches the store (a server
//            from before the store's lock holds none, so the lock alone would not keep it out)
//   Open     the store (store::Store::open): its lock first, exclusive, on the store's directory, before anything in the
//            store is read; then the replay. Held when a writer holds the lock; Failed when only readers held it for
//            all of the lock's wait (store/fs.h), or the store cannot be read
//   Wait     the lock is a writer's, and no server answered: a server starting (its replay) or stopping (its drivers
//            ending, after its socket closed). Wait kWaitRetryMs, ask the socket again: a server answers, refused;
//            else Open again, until `held_wait_ms` has passed since the lock was first found held: Stuck
//   Listen   the control socket: refused when a server answers on it; else bound
//   Restore  the sessions settled against the accounts and the friends' drivers restored, which may commit to the
//            journal and start a driver for a queued prompt: only now, with the lock and the socket both held
//   Serve    the loop
//
// How a start ends, and its exit (a service manager restarts any exit but 0: launchd's KeepAlive with SuccessfulExit
// false, systemd's Restart=on-failure):
//   Refused  a live server answers on the socket: one line, exit kRefusedExit 0, not restarted, as nothing is wrong
//   Stuck    a writer held the lock all of `held_wait_ms` and no server answered: exit kStuckExit 3, its own code, so a
//            service (or a person) can tell it from a failure: a holder that is neither serving nor ending
//   Failed   anything else, readers that did not let go among them: exit kFailedExit 1
// Nothing writes to the journal or starts a driver before Restore.
//
// Fault builds a broken order for the trace binding's negative checks (tests/store/store_lock_walk.cpp), which TLC must
// reject; the server never sets one:
//   Unlocked      the order before the lock: the store opened with no lock, the drivers restored, the socket last
//   RestoreFirst  the lock kept, but the drivers restored before the socket is held
//   HeldRefused   a lock held by a writer taken as another server, refused at once with exit 0, as the first cut of
//                 this order did: a server stopping, its socket closed, would leave nothing serving

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nexus::server {

constexpr int kRefusedExit = 0;
constexpr int kFailedExit = 1;
constexpr int kStuckExit = 3;

// The wait for a lock held by a writer that does not answer: past a server's stop (its drivers' grace periods,
// endGrace and termGrace in server/supervisor.h, 4 s together, then its last flush), with room to spare. A judgement
// from those constants, not measured.
constexpr std::uint64_t kHeldWaitMs = 10000;
constexpr std::uint64_t kWaitRetryMs = 10;

class StartOrder {
public:
    enum class Step { Probe, Open, OpenUnlocked, Wait, Listen, Restore, Serve, Refused, Stuck, Failed };
    // Ok: the step was taken (for Probe and Wait: no server answered). Answered: a live server answers on the socket.
    // Held: a writer holds the store's lock. Failed: anything else.
    enum class Outcome { Ok, Answered, Held, Failed };
    enum class Fault { None, Unlocked, RestoreFirst, HeldRefused };

    explicit StartOrder(Fault fault = Fault::None, std::uint64_t held_wait_ms = kHeldWaitMs)
        : fault_(fault), held_wait_ms_(held_wait_ms) {
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
        if (waiting_) {
            return Step::Wait;
        }
        return at_ < order_.size() ? order_[at_] : Step::Serve;
    }

    // The outcome of the step next() gave, at `now_ms` (any clock; only differences count). Nothing changes once the
    // start has ended.
    void done(Outcome outcome, std::uint64_t now_ms) {
        const Step step = next();
        if (step == Step::Serve || step == Step::Refused || step == Step::Stuck || step == Step::Failed) {
            return;
        }
        if (step == Step::Wait) {
            if (outcome == Outcome::Answered) {
                end_ = Step::Refused;
            } else if (outcome != Outcome::Ok) {
                end_ = Step::Failed;
            } else if (now_ms - held_since_ >= held_wait_ms_) {
                end_ = Step::Stuck;
            } else {
                waiting_ = false;   // Open again
            }
            return;
        }
        if (outcome == Outcome::Ok) {
            ++at_;
            return;
        }
        if (outcome == Outcome::Answered && (step == Step::Probe || step == Step::Listen)) {
            end_ = Step::Refused;
            return;
        }
        if (outcome == Outcome::Held && (step == Step::Open || step == Step::OpenUnlocked)) {
            if (fault_ == Fault::HeldRefused) {
                end_ = Step::Refused;
                return;
            }
            if (!held_) {
                held_ = true;
                held_since_ = now_ms;
            }
            waiting_ = true;
            return;
        }
        end_ = Step::Failed;
    }

private:
    Fault fault_;
    std::uint64_t held_wait_ms_;
    std::vector<Step> order_;
    std::size_t at_ = 0;
    bool waiting_ = false;          // the lock found held: Wait next
    bool held_ = false;             // the lock was found held at least once, first at held_since_
    std::uint64_t held_since_ = 0;
    Step end_ = Step::Serve;        // Serve while the start runs; Refused, Stuck or Failed once it ended so
};

// What each step does, for take_step: the server's (Server::run) and the trace walk's (tests/store/store_lock_walk.cpp)
// own, so that one dispatch, the one below, runs in both.
class StartSteps {
public:
    virtual ~StartSteps() = default;
    virtual StartOrder::Outcome probe(std::string* why) = 0;             // Ok or Answered
    virtual StartOrder::Outcome open(bool unlocked, std::string* why) = 0;   // Ok, Held or Failed
    virtual StartOrder::Outcome listen(std::string* why) = 0;            // Ok, Answered or Failed
    virtual StartOrder::Outcome restore(std::string* why) = 0;           // Ok or Failed
    virtual void pause(std::uint64_t ms) = 0;                            // the wait between a Wait's tries
    virtual std::uint64_t now_ms() = 0;
};

// One step of a start: the one `order` gives, taken by `steps`, and its outcome given to `order`. Returns the step
// taken; an end (Serve, Refused, Stuck, Failed) is returned as it is and takes nothing.
inline StartOrder::Step take_step(StartOrder& order, StartSteps& steps, std::string* why) {
    const StartOrder::Step step = order.next();
    StartOrder::Outcome outcome = StartOrder::Outcome::Failed;
    switch (step) {
    case StartOrder::Step::Probe:
        outcome = steps.probe(why);
        break;
    case StartOrder::Step::Open:
        outcome = steps.open(false, why);
        break;
    case StartOrder::Step::OpenUnlocked:
        outcome = steps.open(true, why);
        break;
    case StartOrder::Step::Wait:
        steps.pause(kWaitRetryMs);
        outcome = steps.probe(why);
        break;
    case StartOrder::Step::Listen:
        outcome = steps.listen(why);
        break;
    case StartOrder::Step::Restore:
        outcome = steps.restore(why);
        break;
    case StartOrder::Step::Serve:
    case StartOrder::Step::Refused:
    case StartOrder::Step::Stuck:
    case StartOrder::Step::Failed:
        return step;
    }
    order.done(outcome, steps.now_ms());
    return step;
}

} // namespace nexus::server
