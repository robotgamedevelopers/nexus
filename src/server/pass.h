// One pass of nexus-server's serving loop (server/server.cpp Server::run, through take_pass_step below),
// models/LoopPass.tla step for step:
//
//   Tick     at the top of the pass, every link ticks (ServerSession::tick): the state set at the end of the pass
//            before leaves on the wire, and mosh's acknowledgement goes out only for actions whose batch's flush has
//            already returned
//   Wait     poll for the control socket, the links, the drivers and any background config push
//   Hold     the pass's batch begins (DurableStore::beginBatch) and the drivers are held (Drivers::hold): commits
//            append to the journal in memory without syncing, and commands told to a driver wait until Release
//   Apply    the batch's steps (Drivers::serve, deadlines, and each readable link's receive, take and Hub::apply); a
//            store failure ends the pass Failed before Flush, Release, State or another Tick
//   Flush    the batch's one sync (DurableStore::flush); a failed flush ends the pass Failed with neither Release nor
//            State, and the loop stops so no later Tick acknowledges the batch
//   Release  Drivers::release carries out what the batch told the drivers, in order
//   State    when the pass changed anything, ServerSession::setState stages the new state on every link for the next
//            pass's Tick
//
// Fault builds a broken pass for the trace binding's negative checks (tests/server/pass_walk.cpp) and unit checks
// (tests/server/pass_test.cpp), each of which TLC must reject; the server never sets one:
//   ReleaseBeforeFlush  Drivers::release before DurableStore::flush (PromptAtMostOnce, DriverOnlyDurable)
//   StateBeforeFlush    setState before DurableStore::flush (LinkStateDurable, NoOutBeforeFlush)
//   TickBeforeFlush     the links' ticks after Apply and before DurableStore::flush (AckedOnlyDurable)
//   NoHold              the batch applied without Drivers::hold (PromptAtMostOnce, DriverOnlyDurable)
//   ReleaseOnFail       Drivers::release and setState run even after DurableStore::flush failed (DriverOnlyDurable)

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace nexus::server {

class PassOrder {
public:
    enum class Step { Tick, Wait, Hold, HoldUnheld, Apply, Flush, Release, State, Done, Stopped, Failed };
    enum class Outcome { Ok, Changed, Unchanged, Stopped, Failed };
    enum class Fault { None, ReleaseBeforeFlush, StateBeforeFlush, TickBeforeFlush, NoHold, ReleaseOnFail };

    explicit PassOrder(Fault fault = Fault::None) : fault_(fault) {
        switch (fault) {
        case Fault::ReleaseBeforeFlush:
            order_ = {Step::Tick, Step::Wait, Step::Hold, Step::Apply, Step::Release, Step::Flush, Step::State};
            break;
        case Fault::StateBeforeFlush:
            order_ = {Step::Tick, Step::Wait, Step::Hold, Step::Apply, Step::State, Step::Flush, Step::Release};
            break;
        case Fault::TickBeforeFlush:
            order_ = {Step::Tick, Step::Wait, Step::Hold, Step::Apply, Step::Tick, Step::Flush, Step::Release,
                      Step::State};
            break;
        case Fault::NoHold:
            order_ = {Step::Tick, Step::Wait, Step::HoldUnheld, Step::Apply, Step::Flush, Step::Release, Step::State};
            break;
        case Fault::None:
        case Fault::ReleaseOnFail:
            order_ = {Step::Tick, Step::Wait, Step::Hold, Step::Apply, Step::Flush, Step::Release, Step::State};
            break;
        }
    }

    Step next() const {
        if (end_ != Step::Done) {
            return end_;
        }
        return at_ < order_.size() ? order_[at_] : Step::Done;
    }

    bool changed() const { return changed_; }

    void done(Outcome outcome) {
        const Step step = next();
        if (step == Step::Done || step == Step::Stopped || step == Step::Failed) {
            return;
        }
        if (step == Step::Wait) {
            if (outcome == Outcome::Stopped) {
                end_ = Step::Stopped;
            } else if (outcome != Outcome::Ok) {
                end_ = Step::Failed;
            } else {
                ++at_;
            }
            return;
        }
        if (step == Step::Hold || step == Step::HoldUnheld) {
            if (outcome != Outcome::Ok) {
                end_ = Step::Failed;
            } else {
                changed_ = false;
                ++at_;
            }
            return;
        }
        if (step == Step::Apply) {
            if (outcome == Outcome::Changed) {
                changed_ = true;
                ++at_;
            } else if (outcome == Outcome::Unchanged || outcome == Outcome::Ok) {
                ++at_;
            } else {
                end_ = Step::Failed;
            }
            return;
        }
        if (step == Step::Flush) {
            if (outcome == Outcome::Ok || fault_ == Fault::ReleaseOnFail) {
                ++at_;
            } else {
                end_ = Step::Failed;
            }
            return;
        }
        if (outcome == Outcome::Ok) {
            ++at_;
            return;
        }
        end_ = Step::Failed;
    }

private:
    Fault fault_;
    std::vector<Step> order_;
    std::size_t at_ = 0;
    bool changed_ = false;
    Step end_ = Step::Done;
};

// What each step of a pass does, for take_pass_step: the server's (Server::run) and the trace walk's
// (tests/server/pass_walk.cpp) own, so that one dispatch runs in both.
class PassSteps {
public:
    virtual ~PassSteps() = default;
    virtual void tick() = 0;
    virtual PassOrder::Outcome wait(std::string* why) = 0;                      // Ok or Stopped
    virtual PassOrder::Outcome hold(bool hold_drivers, std::string* why) = 0;   // Ok or Failed
    virtual PassOrder::Outcome apply(std::string* why) = 0;                     // Changed, Unchanged or Failed
    virtual PassOrder::Outcome flush(std::string* why) = 0;                     // Ok or Failed
    virtual void release() = 0;
    virtual void state() = 0;
};

inline PassOrder::Step take_pass_step(PassOrder& order, PassSteps& steps, std::string* why) {
    const PassOrder::Step step = order.next();
    PassOrder::Outcome outcome = PassOrder::Outcome::Failed;
    switch (step) {
    case PassOrder::Step::Tick:
        steps.tick();
        outcome = PassOrder::Outcome::Ok;
        break;
    case PassOrder::Step::Wait:
        outcome = steps.wait(why);
        break;
    case PassOrder::Step::Hold:
        outcome = steps.hold(true, why);
        break;
    case PassOrder::Step::HoldUnheld:
        outcome = steps.hold(false, why);
        break;
    case PassOrder::Step::Apply:
        outcome = steps.apply(why);
        break;
    case PassOrder::Step::Flush:
        outcome = steps.flush(why);
        break;
    case PassOrder::Step::Release:
        steps.release();
        outcome = PassOrder::Outcome::Ok;
        break;
    case PassOrder::Step::State:
        if (order.changed()) {
            steps.state();
        }
        outcome = PassOrder::Outcome::Ok;
        break;
    case PassOrder::Step::Done:
    case PassOrder::Step::Stopped:
    case PassOrder::Step::Failed:
        return step;
    }
    order.done(outcome);
    return step;
}

struct PassEnd {
    std::function<bool(std::string*)> flush;    // the batch's one flush; false, with why, when it failed
    std::function<void()> release;              // carry out what the batch told the drivers
    std::function<void()> publish;              // set the state on every link
};

// False, with why, when the flush failed: nothing was released or published.
inline bool endPass(const PassEnd& end, bool changed, std::string* why) {
    if (!end.flush(why)) {
        return false;
    }
    end.release();
    if (changed) {
        end.publish();
    }
    return true;
}

} // namespace nexus::server
