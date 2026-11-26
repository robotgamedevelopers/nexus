// The window's side of the relay's life, with every decision in it: when a relay (nexus-link-relay inside WSL2,
// relay/relay_wire.h) may start, what is written to it and in what order, how the window numbers its actions across
// relays, which it learns the server applied, and which a relay that ended took with it. models/RelayLife.tla is the
// model; tests/relay/relay_walk.cpp binds this code to it by trace. relay/relay_port.h does the processes, the pipes
// and the threads around it, and asks it everything; there is no OS and no clock here (time is an argument, in ms).
//
// One relay at a time: started is refused while a relay is live (started and not yet ended), and the port reports
// ended only once the relay's process has exited. The first starts at once; after one ends, the next waits a backoff:
// 1 s, doubling to at most 10 s, and 1 s again after a relay that ran 10 s or more.
//
// The window's numbers: send numbers the window's actions 1, 2, 3, ... across every relay, never reusing one. Each
// relay's link numbers the actions written to it from 1 (link/client.h), and they are written in the window's order,
// so its k-th action is the window's base + k, where base is how many were written before it started. The server's
// state says, in the link's numbers, the first action it has not applied (sync::Sessions::nextAction); acked is that,
// less one, in the window's numbers, and poll shows the window's next action unapplied in its place.
//
// A relay that ends takes with it the actions written to it that the server had not acknowledged: its link, and with
// it the link's client id, are gone, and a new relay is a new link, as a new launch of the window is. They are never
// written again (the server may have applied one whose acknowledgement was lost: written again under the new link's
// client id, it would be applied twice). Each is cut: in LinkState::cut at the next poll, exactly once, for the window
// to show. Actions not yet written wait for the next relay, in order, after its Hello. At the window's exit (stop)
// nothing more is written and no relay starts; what was not acknowledged is what a window that quits leaves, as on
// macOS and Linux.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "link_port.h"
#include "relay/relay_ends.h"

namespace nexus::relay {

using LifeMs = std::uint64_t;

constexpr LifeMs kFirstBackoffMs = 1000;
constexpr LifeMs kLastBackoffMs = 10000;
// A relay that ran this long before it ended was working: the next waits the first backoff again.
constexpr LifeMs kRanWellMs = 10000;

class RelayLife {
public:
    enum class Phase {
        Waiting,    // no relay: the next starts when the backoff is over (mayStart)
        Hello,      // a relay is live and its Hello not yet read
        Running,    // its Hello read and found this build's
        Stopped,    // the window is exiting: no relay starts, nothing more is written
    };

    // The window's calls.
    // An action for the server: queued for the relay, in order. Its number in the window's numbering.
    std::uint64_t send(const sync::Action& action);
    void history(sync::SessionId id, std::uint64_t epoch, std::uint64_t before);
    // The latest state, with the window's next unapplied action and the actions cut since the last poll; true if it
    // changed since the last call that returned true.
    bool poll(LinkState* out);
    void stop();

    // The port's calls.
    bool mayStart(LifeMs now) const;
    // How long until mayStart, 0 when it is now; with a relay live or the window stopped, kLastBackoffMs (nothing to
    // wait for here: the port waits on the relay).
    LifeMs waitMs(LifeMs now) const;
    // A relay was started (or its start failed: the port then reports ended at once). False, changing nothing, unless
    // mayStart.
    bool started(LifeMs now);
    // True while there are bytes to write to the live relay.
    bool wantsWrite() const;
    // The next bytes for the live relay, its Hello first; false with nothing to write. An action's bytes count as
    // written once taken: the port writes them, or the relay has ended.
    bool take(std::string* bytes);
    // Bytes the live relay wrote. False, with why, once they are not the relay protocol: the port ends that relay.
    bool feed(const char* data, std::size_t size, std::string* why);
    // The live relay's process exited, for `why`. The actions written to it and not acknowledged are cut.
    void ended(const std::string& why, LifeMs now);

    // Read by the walk and the tests.
    Phase phase() const;
    bool live() const { return live_; }
    std::uint64_t relays() const { return relays_; }        // relays started
    std::uint64_t sent() const { return sent_; }            // the window's last action's number
    std::uint64_t written() const { return written_; }      // the last action written to a relay
    std::uint64_t base() const { return base_; }            // written before the latest relay started
    std::uint64_t acked() const { return acked_; }          // the server applied every action up to this one
    const std::vector<std::uint64_t>& cut() const { return cut_; }  // every action cut, in order
    LifeMs backoffMs() const { return backoff_; }           // the next backoff, unless the relay runs well
    LifeMs leftMs(LifeMs now) const;                        // of the backoff, while Waiting

private:
    struct Out {
        bool action = false;
        std::uint64_t number = 0;   // with action: its window number
        std::string bytes;
    };

    RelayClient client_;
    std::deque<Out> out_;
    bool stopped_ = false;
    bool live_ = false;
    bool helloOwed_ = false;
    std::uint64_t relays_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t base_ = 0;
    std::uint64_t acked_ = 0;
    LifeMs startedAt_ = 0;
    LifeMs dueAt_ = 0;
    LifeMs backoff_ = kFirstBackoffMs;
    std::vector<std::uint64_t> cut_;
    std::vector<std::uint64_t> untold_;     // cut, not yet in a poll
    bool changed_ = false;                  // cut or acked moved since the last poll
};

} // namespace nexus::relay
