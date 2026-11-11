// nexus-server's tabs: what the clients' actions do to them, and the down state (sync/tabs.h) every client is sent.
// A prompt sent to a friend's tab goes to its Responder: in nexus-server, the tab's driver supervisor (server/drivers.h),
// which puts the prompt in the transcript and writes it to the friend's driver, whose replies it adds later (commit).
// Without a Responder (the tests, and the link's walk), the prompt is followed by a placeholder reply, "(received)", so
// that a round trip shows. A queued prompt goes in its queue (and to the Responder, which takes it from the queue's
// head at the end of a turn). A stop goes to the Responder at once, and changes no tab of its own.
//
// A friend's reply as it grows is a tab's open entry (setOpen): the newest entry of the tab's state, still open, kept
// in memory only and never in the store. When its block ends, the Responder commits the closed entry (and the open one
// goes): so the store holds each closed entry once, and a restart loses at most the open one.
//
// The tabs live in a TabStore: MemoryStore keeps them in memory, for the tests and walks; nexus-server's DurableStore
// (server/durable.h) keeps them on disk (src/store). Each action is one step of the store: what it does to its tab and
// the client's applied counter land together or not at all, so that after a restart the counter says exactly which of a
// reconnecting window's actions were applied.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sync/actions.h"
#include "sync/tabs.h"

namespace nexus::sync {

// What nexus-server keeps of a friend tab's driver (server/supervisor.h) across restarts of the driver and of the
// server: the next turn number (from 1, never reused), the turn open (0 for none), the last session id the driver
// reported (the next start's --resume), and the prompts waiting for a driver, oldest first.
struct DriverState {
    std::uint64_t nextTurn = 1;
    std::uint64_t openTurn = 0;
    std::string session;
    std::vector<std::string> waiting;
    bool operator==(const DriverState&) const = default;
};

// What the server holds of one tab: its whole transcript, its queue, and its driver's state.
struct StoredTab {
    std::vector<EntryRef> transcript;
    std::vector<std::string> queue;
    DriverState driver;
};

// One change to a tab: an entry at the end of its transcript; or (entry null) a prompt at the end of its queue; or
// (driver set) its driver's state, replaced whole; or (take set) the oldest prompt of its queue leaves it.
struct TabChange {
    EntryRef entry;
    std::string prompt;
    std::optional<DriverState> driver = std::nullopt;
    bool take = false;
};

// The take change: the oldest queued prompt leaves the queue.
TabChange takeQueued();

class TabStore {
public:
    virtual ~TabStore() = default;
    virtual const StoredTab* find(const std::string& name) const = 0;
    virtual std::size_t size() const = 0;   // how many tabs it holds
    virtual std::vector<std::string> names() const = 0;
    // One step, all or none: `changes` to the tab `name` (made if it does not exist yet), and, when `client` is not
    // empty, that client's applied counter set to `num`. False, with why, when the step was not taken; a store that
    // failed to write takes no later step either (failed()).
    virtual bool commit(const std::string& name, const std::vector<TabChange>& changes, const std::string& client,
                        std::uint64_t num, std::string* why) = 0;
    virtual std::uint64_t applied(const std::string& client) const = 0;   // 0 for a client never seen
    virtual bool failed() const = 0;
};

class MemoryStore : public TabStore {
public:
    const StoredTab* find(const std::string& name) const override;
    std::size_t size() const override { return tabs_.size(); }
    std::vector<std::string> names() const override;
    bool commit(const std::string& name, const std::vector<TabChange>& changes, const std::string& client,
                std::uint64_t num, std::string* why) override;
    std::uint64_t applied(const std::string& client) const override;
    bool failed() const override { return false; }

private:
    std::map<std::string, StoredTab> tabs_;
    std::map<std::string, std::uint64_t> applied_;
};

// The bounds that keep every diff small (sync/tabs.h): no single text larger than `text` bytes, no queue longer than
// `queue` prompts or `queueBytes` bytes, at most `tabs` tabs, and each transcript's tail by `tail`. With the defaults a
// diff from the empty state is at most 16 tabs of (a 32 KiB closed entry or a 16 KiB tail), a 32 KiB open entry and a
// 32 KiB queue: 96 KiB a tab, about 1.5 MiB in all, under the 4 MiB mosh can carry (its compressor's buffer, mosh
// src/network/compressor.h). These numbers follow from that limit and are not measured.
struct Limits {
    std::size_t text = 32 * 1024;
    std::size_t queue = 8;
    std::size_t queueBytes = 32 * 1024;
    std::size_t tabs = 16;
    Tail tail;
};

extern const char* const kReceived;  // the placeholder reply: "(received)"

// What a send, a queue and a stop to a friend's tab do once they are within the hub's limits.
class Responder {
public:
    virtual ~Responder() = default;
    // The changes a send of `prompt` makes to `tab` (the person's entry and anything after it, or a notice that
    // refuses it), which the hub commits in one step with the client's counter.
    virtual std::vector<TabChange> send(const std::string& tab, const std::string& prompt) = 0;
    // The changes a queue of `prompt` makes: the prompt at the end of the queue first, then anything that follows (the
    // oldest taken at once, when the friend is free), committed the same way.
    virtual std::vector<TabChange> queue(const std::string& tab, const std::string& prompt) = 0;
    // The changes a stop of the friend's work in `tab` makes (none, so far), committed the same way.
    virtual std::vector<TabChange> stop(const std::string& tab) = 0;
    // Whether the store took the step of the send, queue or stop just given. Only once it did may the responder act
    // on it.
    virtual void sent(const std::string& tab, bool committed) = 0;
};

class Hub {
public:
    // The state starts as every tab the store already holds. Sends go to `responder` when one is given.
    explicit Hub(TabStore& store, Limits limits = {}, Responder* responder = nullptr);

    // Applies one action (each once: the session's Applier sees to that). A send gives the responder's changes, or
    // without one the prompt and then the placeholder reply; a queue puts the prompt in the tab's queue (with the
    // responder's changes after it); a stop gives the responder's changes, or none. An action past the limits is
    // refused: the tab's transcript gets a notice that says so (when the tab exists or can be made), and `why` says it
    // too. A stop is refused for a tab the store does not hold (nothing runs there, and no tab is made for it), and
    // when it carries text. Either way, with `client` given, the store's counter for it becomes action.num in the same
    // step. Returns false when refused, or when the store did not take the step (failed() then says whether it can
    // take any more).
    bool apply(const Action& action, std::string* why = nullptr, const std::string& client = std::string());

    // Changes that come from the server rather than a client (a friend's reply), in one step of the store. False, with
    // why, when the store did not take it.
    bool commit(const std::string& tab, const std::vector<TabChange>& changes, std::string* why = nullptr);

    // The friend's text still growing in `tab`, its open entry, after the stored transcript (empty: none). In memory
    // only. A tab the store does not hold has none. Within the hub's text limit: longer text is refused (false).
    // Returns true when the state changed.
    bool setOpen(const std::string& tab, const std::string& text);

    const Tabs& state() const { return state_; }
    const Limits& limits() const { return limits_; }
    bool failed() const { return store_.failed(); }

private:
    void refresh(const std::string& name);
    TabStore& store_;
    Limits limits_;
    Responder* responder_;
    std::map<std::string, EntryRef> open_;     // each tab's open entry, if it has one
    Tabs state_;
};

} // namespace nexus::sync
