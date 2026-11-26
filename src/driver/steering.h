// A driver's side of steering a turn that runs (docs/driver-protocol.md, "Steering a turn"): the turn the driver has
// open, the steers and follow-ups it took and has not yet given its harness, and when each goes in. Pure: no OS, no
// time, no harness. A driver feeds it the server's send, stop, steer and follow-up lines and its harness's moments (a
// tool-call boundary, the end of a model run, the turn's end), and writes the lines it returns. The fake driver
// (tests/driver/fake_driver.cpp) plays it from a script; tests/driver/steering_walk.cpp binds it, with the server's
// supervisor, to models/Steering.tla (Read, Boundary, RunEnd, Settle, Finish).
//
// The rules (the model's): a steer or follow-up is taken only for the open turn, before it settles, from a server that
// keeps the order (each id above the last, no turn the driver has not opened); one for a turn not open, or a settled
// one, is ignored, since it crossed the turn's end on the way (the server gives it back at the done); every change of
// the queue is reported whole; steers go in at the next tool-call boundary, or at the end of a model run; a follow-up
// goes in only at the end of a model run with no steer pending, one per run; settled is written only with nothing
// pending, or at a stop (aborted), and then the turn's done.

#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "driver/protocol.h"

namespace nexus::driver {

class Steering {
public:
    // A message the harness takes in: its id, whether it is a follow-up (else a steer), and its text.
    struct Item {
        std::uint64_t id = 0;
        bool followUp = false;
        std::string text;
    };

    // What a line from the server comes to: taken (with the queue to report, for a steer or follow-up), ignored (it
    // crossed the turn's end), or refused as the server's bug (with why: the driver writes one protocol error and
    // ends, rule 4).
    struct Took {
        enum class What { Taken, Ignored, Refused } what = What::Ignored;
        std::optional<Queue> report;
        std::optional<Settled> settled;     // a stop of the open turn: settled, aborted (to a driver that names it)
        std::string why;
    };

    // What goes into the harness at a moment of the turn, and the queue to report when it changed.
    struct Given {
        std::vector<Item> items;
        std::optional<Queue> report;
    };

    enum class Phase { Idle, Running, Settling };

    // The features the driver's own hello named.
    Steering() = default;
    explicit Steering(const std::vector<std::string>& features) { name(features); }

    void name(const std::vector<std::string>& features) {
        auto has = [&features](std::string_view f) {
            return std::find(features.begin(), features.end(), f) != features.end();
        };
        steers_ = has(kFeatureSteer);
        followsUp_ = has(kFeatureFollowUp);
        settles_ = has(kFeatureSettled);
    }

    bool steers() const { return steers_; }
    bool followsUp() const { return followsUp_; }
    bool settles() const { return settles_; }

    Took send(const Send& m) {
        if (phase_ != Phase::Idle) {
            return refused("a send of turn " + std::to_string(m.turn) + " arrived while turn " + std::to_string(turn_) +
                " is open; the server writes the next send only after the open turn's done");
        }
        if (m.turn <= last_) {
            return refused("a send of turn " + std::to_string(m.turn) + " does not follow turn " +
                std::to_string(last_) + "; turns go up");
        }
        turn_ = m.turn;
        last_ = m.turn;
        phase_ = Phase::Running;
        aborted_ = false;
        steer_.clear();
        follow_.clear();
        Took t;
        t.what = Took::What::Taken;
        return t;
    }

    Took take(const Steer& m) { return take(m.turn, m.id, m.text, false); }
    Took take(const FollowUp& m) { return take(m.turn, m.id, m.text, true); }

    // A stop: of the open turn while it runs, it settles now, aborted, and what is queued never goes in; any other is
    // ignored.
    Took stop(const Stop& m) {
        Took t;
        if (phase_ != Phase::Running || m.turn != turn_) {
            return t;
        }
        t.what = Took::What::Taken;
        t.settled = settleNow(true);
        return t;
    }

    // The tool calls are done, before the next model call: every steer pending goes in.
    Given boundary() {
        Given g;
        if (phase_ != Phase::Running || steer_.empty()) {
            return g;
        }
        g.items.assign(steer_.begin(), steer_.end());
        steer_.clear();
        g.report = report();
        return g;
    }

    // A model run ended with no tool calls: the steers pending go in, and the run goes on; else the oldest follow-up
    // goes in, and the run goes on; else nothing, and the turn may settle (or retry, or compact).
    Given runEnd() {
        Given g = boundary();
        if (!g.items.empty() || phase_ != Phase::Running || follow_.empty()) {
            return g;
        }
        g.items.push_back(follow_.front());
        follow_.pop_front();
        g.report = report();
        return g;
    }

    bool pending() const { return !steer_.empty() || !follow_.empty(); }

    // The turn has nothing automatic left: settled, to a driver that names it. Only with nothing pending (a driver
    // drains runEnd() first); else nothing, and the turn goes on.
    std::optional<Settled> settle() {
        if (phase_ != Phase::Running || pending()) {
            return std::nullopt;
        }
        return settleNow(false);
    }
    bool settling() const { return phase_ == Phase::Settling; }
    bool aborted() const { return aborted_; }

    // The turn's done is written: the turn is closed, and the queue with it.
    void done() {
        phase_ = Phase::Idle;
        turn_ = 0;
        steer_.clear();
        follow_.clear();
    }

    Phase phase() const { return phase_; }
    std::uint64_t turn() const { return turn_; }        // the open turn, 0 for none
    std::uint64_t lastTurn() const { return last_; }
    std::uint64_t lastId() const { return lastId_; }
    std::vector<std::uint64_t> steerIds() const { return ids(steer_); }
    std::vector<std::uint64_t> followUpIds() const { return ids(follow_); }

private:
    static std::vector<std::uint64_t> ids(const std::deque<Item>& q) {
        std::vector<std::uint64_t> out;
        for (const Item& i : q) {
            out.push_back(i.id);
        }
        return out;
    }

    Queue report() const { return Queue{turn_, ids(steer_), ids(follow_)}; }

    static Took refused(std::string why) {
        Took t;
        t.what = Took::What::Refused;
        t.why = std::move(why);
        return t;
    }

    std::optional<Settled> settleNow(bool aborted) {
        phase_ = Phase::Settling;
        aborted_ = aborted;
        if (!settles_) {
            return std::nullopt;
        }
        return Settled{turn_, aborted};
    }

    Took take(std::uint64_t turn, std::uint64_t id, const std::string& text, bool followUp) {
        const char* kind = followUp ? "follow-up" : "steer";
        if (!(followUp ? followsUp_ : steers_)) {
            return refused(std::string("the server wrote a ") + kind + ", which this driver's hello did not name; a "
                "server writes it only to a driver that names \"" + kind + "\"");
        }
        if (turn > last_) {
            return refused(std::string("a ") + kind + " for turn " + std::to_string(turn) + ", which the server has not "
                "opened (the last send was turn " + std::to_string(last_) + ")");
        }
        if (id <= lastId_) {
            return refused(std::string("a ") + kind + " numbered " + std::to_string(id) + ", not above the last, " +
                std::to_string(lastId_) + "; the server numbers what it writes into turns in order");
        }
        lastId_ = id;
        if (turn != turn_ || phase_ != Phase::Running) {
            return Took{};      // it crossed the turn's end: the server gives it back at the done
        }
        std::deque<Item>& q = followUp ? follow_ : steer_;
        if (q.size() == kMaxQueued) {
            return refused(std::string("a ") + kind + " past the " + std::to_string(kMaxQueued) + " a turn holds "
                "pending; the server writes no more into one turn");
        }
        q.push_back(Item{id, followUp, text});
        Took t;
        t.what = Took::What::Taken;
        t.report = report();
        return t;
    }

    bool steers_ = false;
    bool followsUp_ = false;
    bool settles_ = false;
    Phase phase_ = Phase::Idle;
    std::uint64_t turn_ = 0;
    std::uint64_t last_ = 0;
    std::uint64_t lastId_ = 0;
    bool aborted_ = false;
    std::deque<Item> steer_;
    std::deque<Item> follow_;
};

} // namespace nexus::driver
