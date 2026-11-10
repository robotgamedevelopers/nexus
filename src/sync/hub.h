// nexus-server's tabs: what the clients' actions do to them, and the down state (sync/tabs.h) every client is sent.
// No AI yet: a prompt sent to a friend's tab goes in its transcript, followed by a placeholder reply, "(received)", so
// that a round trip shows; a queued prompt goes in its queue.
//
// The tabs live in a TabStore: MemoryStore keeps them in memory, for the tests and walks; nexus-server's DurableStore
// (server/durable.h) keeps them on disk (src/store). Each action is one step of the store: what it does to its tab and
// the client's applied counter land together or not at all, so that after a restart the counter says exactly which of a
// reconnecting window's actions were applied.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sync/actions.h"
#include "sync/tabs.h"

namespace nexus::sync {

// What the server holds of one tab: its whole transcript and its queue.
struct StoredTab {
    std::vector<EntryRef> transcript;
    std::vector<std::string> queue;
};

// One change to a tab: an entry at the end of its transcript, or (entry null) a prompt at the end of its queue.
struct TabChange {
    EntryRef entry;
    std::string prompt;
};

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
// diff from the empty state is at most 16 tabs of (a 32 KiB entry or a 16 KiB tail) and a 32 KiB queue: about 1 MiB,
// under the 4 MiB mosh can carry (its compressor's buffer, mosh src/network/compressor.h). These numbers follow from
// that limit and are not measured.
struct Limits {
    std::size_t text = 32 * 1024;
    std::size_t queue = 8;
    std::size_t queueBytes = 32 * 1024;
    std::size_t tabs = 16;
    Tail tail;
};

extern const char* const kReceived;  // the placeholder reply: "(received)"

class Hub {
public:
    // The state starts as every tab the store already holds.
    explicit Hub(TabStore& store, Limits limits = {});

    // Applies one action (each once: the session's Applier sees to that). A send puts the prompt in the tab's
    // transcript and then the placeholder reply; a queue puts it in the tab's queue. An action past the limits is
    // refused: the tab's transcript gets a notice that says so (when the tab exists or can be made), and `why` says it
    // too. Either way, with `client` given, the store's counter for it becomes action.num in the same step. Returns
    // false when refused, or when the store did not take the step (failed() then says whether it can take any more).
    bool apply(const Action& action, std::string* why = nullptr, const std::string& client = std::string());

    const Tabs& state() const { return state_; }
    const Limits& limits() const { return limits_; }
    bool failed() const { return store_.failed(); }

private:
    void refresh(const std::string& name);
    TabStore& store_;
    Limits limits_;
    Tabs state_;
};

} // namespace nexus::sync
