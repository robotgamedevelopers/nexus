// nexus-server's tabs: what the clients' actions do to them, and the down state (sync/tabs.h) every client is sent.
// No AI yet: a prompt sent to a friend's tab goes in its transcript, followed by a placeholder reply, "(received)", so
// that a round trip shows; a queued prompt goes in its queue.
//
// The tabs live in a TabStore, the seam for the store layer to come; MemoryStore keeps them in memory, so they last as
// long as the server runs.

#pragma once

#include <cstddef>
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

class TabStore {
public:
    virtual ~TabStore() = default;
    virtual const StoredTab* find(const std::string& name) const = 0;
    virtual std::size_t size() const = 0;   // how many tabs it holds
    // Each makes the tab if it does not exist yet.
    virtual void append(const std::string& name, EntryRef entry) = 0;
    virtual void enqueue(const std::string& name, std::string prompt) = 0;
};

class MemoryStore : public TabStore {
public:
    const StoredTab* find(const std::string& name) const override;
    std::size_t size() const override { return tabs_.size(); }
    void append(const std::string& name, EntryRef entry) override;
    void enqueue(const std::string& name, std::string prompt) override;

private:
    std::map<std::string, StoredTab> tabs_;
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
    explicit Hub(TabStore& store, Limits limits = {});

    // Applies one action (each once: the session's Applier sees to that). A send puts the prompt in the tab's
    // transcript and then the placeholder reply; a queue puts it in the tab's queue. An action past the limits is
    // refused: the tab's transcript gets a notice that says so (when the tab exists or can be made), and `why` says it
    // too. Returns false when refused.
    bool apply(const Action& action, std::string* why = nullptr);

    const Tabs& state() const { return state_; }
    const Limits& limits() const { return limits_; }

private:
    void refresh(const std::string& name);
    TabStore& store_;
    Limits limits_;
    Tabs state_;
};

} // namespace nexus::sync
