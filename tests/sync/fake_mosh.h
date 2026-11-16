// What mosh's Transport does with the states it carries, without its sockets, clock or crypto: a test-only copy of the
// bookkeeping in mosh 1.4.0 src/network/transportsender-impl.h and networktransport-impl.h, so that unit tests and the
// trace walk drive nexus's states (sync/actions.h, sync/sessions.h) through the same calls, in the same order, under
// loss, duplication and reordering chosen by a seeded generator.
//
//   sender:   keeps the states it sent (the first one acknowledged), subtracts the acknowledged one from every state
//             it holds before each send (rationalize_states), diffs the current state from an assumed receiver state
//             it sent (or from the acknowledged one: the prospective resend), numbers a new state one past the last
//             sent, keeps at most 32 (dropping the sixteenth from the end), and drops all before an acknowledgement.
//   receiver: ignores a state it holds and a diff from a state it lacks; drops the states before the sender's
//             throwaway number; applies the diff to a copy of the reference state; inserts it in number order; and
//             get_remote_diff() diffs the newest from the last one asked for, then subtracts the oldest from each.

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <string>
#include <vector>

namespace nexus::test {

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
    bool chance(std::size_t inN) { return below(inN) == 0; }
};

struct Packet {
    std::uint64_t oldNum;
    std::uint64_t newNum;
    std::uint64_t throwaway;
    std::string diff;
};

template <class S>
class FakeSender {
public:
    S current;

    FakeSender() { sent_.push_back({0, S()}); }
    explicit FakeSender(S initial) : current(initial) { sent_.push_back({0, std::move(initial)}); }

    // One send, as tick() makes it, from the assumed receiver state: one picked at random among the states sent (the
    // newest sent within the retransmission timeout, the acknowledged one for the prospective resend), or with
    // `timedOut`, when every unacknowledged state has timed out, the acknowledged one.
    Packet send(Rng& rng, bool timedOut = false) {
        rationalize();
        auto assumed = sent_.begin();
        if (!timedOut)
            std::advance(assumed, static_cast<std::ptrdiff_t>(rng.below(sent_.size())));
        Packet p;
        p.oldNum = assumed->num;
        p.diff = current.diff_from(assumed->state);
        const bool fresh = !(current == sent_.back().state);
        p.newNum = fresh ? sent_.back().num + 1 : sent_.back().num;
        if (fresh) {
            sent_.push_back({p.newNum, current});
            if (sent_.size() > 32) {
                auto last = sent_.end();
                for (int i = 0; i < 16; ++i)
                    --last;
                sent_.erase(last);
            }
        }
        p.throwaway = sent_.front().num;
        return p;
    }

    void acknowledge(std::uint64_t num) {
        bool known = false;
        for (const auto& s : sent_)
            known = known || s.num == num;
        if (!known)
            return;
        while (sent_.front().num < num)
            sent_.pop_front();
    }

    std::uint64_t acknowledged() const { return sent_.front().num; }
    const S& acknowledgedState() const { return sent_.front().state; }
    std::uint64_t last() const { return sent_.back().num; }
    bool settled() const { return current == sent_.back().state && sent_.size() == 1; }

private:
    struct Sent {
        std::uint64_t num;
        S state;
    };
    void rationalize() {
        const S* known = &sent_.front().state;
        current.subtract(known);
        for (auto it = sent_.rbegin(); it != sent_.rend(); ++it)
            it->state.subtract(known);
    }
    std::list<Sent> sent_;
};

template <class S>
class FakeReceiver {
public:
    FakeReceiver() { states_.push_back({0, S()}); }
    explicit FakeReceiver(S initial) : last_(initial) { states_.push_back({0, std::move(initial)}); }

    // Returns the acknowledgement it would send: the newest state's number.
    std::uint64_t receive(const Packet& p) {
        for (const auto& s : states_) {
            if (s.num == p.newNum)
                return states_.back().num;
        }
        auto ref = states_.begin();
        for (; ref != states_.end(); ++ref) {
            if (ref->num == p.oldNum)
                break;
        }
        if (ref == states_.end())
            return states_.back().num;
        for (auto it = states_.begin(); it != states_.end();) {
            if (it->num < p.throwaway && states_.size() > 1 && it != ref)
                it = states_.erase(it);
            else
                ++it;
        }
        Held fresh{p.newNum, ref->state};
        if (!p.diff.empty())
            fresh.state.apply_string(p.diff);
        auto at = states_.begin();
        while (at != states_.end() && at->num < fresh.num)
            ++at;
        states_.insert(at, fresh);
        return states_.back().num;
    }

    std::string remoteDiff() {
        std::string diff = states_.back().state.diff_from(last_);
        const S* oldest = &states_.front().state;
        for (auto it = states_.rbegin(); it != states_.rend(); ++it)
            it->state.subtract(oldest);
        last_ = states_.back().state;
        return diff;
    }

    const S& latest() const { return states_.back().state; }
    std::uint64_t latestNum() const { return states_.back().num; }

private:
    struct Held {
        std::uint64_t num;
        S state;
    };
    std::list<Held> states_;
    S last_;
};

} // namespace nexus::test
