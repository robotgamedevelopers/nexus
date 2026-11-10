#pragma once
#include "store/fs.h"
#include "store/journal.h"
#include "store/status.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The server's durable state: the friend tabs (each a transcript of entries and a queue of prompts) and each client's
// applied counter, the number of the last of its actions the server applied. The counter is what keeps a window that
// reconnects, and sends again the actions it has no acknowledgement for, from having them applied twice.
//
// One journal (journal.h) in the store's directory. Every commit is one record of the journal, appended and synced
// before commit returns Ok: a tab's changes and the counter of the action that made them land together or not at all,
// and nothing the server acknowledges or sends is lost to a crash. When the log passes StoreOptions::compact_bytes the
// whole state is written as the journal's snapshot and the log starts again.
//
// A record: u8 format (1); str tab; u32 count, then each change: u8 kind, and for an append u8 entry kind, u8 closed,
// str text, for an enqueue str prompt; then str client (empty: none) and u64 its new counter. The snapshot: u8 format
// (1); u32 tabs, each str name, u32 entries (u8 kind, u8 closed, str text), u32 prompts (str); u32 clients, each str
// client, u64 counter. Little-endian; str is a u32 length and the bytes (codec.h).
namespace nexus::store {

// An entry of a transcript. The store keeps kind as the server gives it (sync's EntryKind); an open entry (closed
// false) is one that may still grow.
struct Entry {
    std::uint8_t kind = 0;
    std::string text;
    bool closed = true;
    bool operator==(const Entry&) const = default;
};

struct Tab {
    std::vector<Entry> transcript;
    std::vector<std::string> queue;
    bool operator==(const Tab&) const = default;
};

struct Change {
    enum class Kind : std::uint8_t { Append = 1, Enqueue = 2 };
    Kind kind = Kind::Append;
    Entry entry;          // Append: the entry added at the end of the transcript
    std::string prompt;   // Enqueue: the prompt added at the end of the queue
};

struct StoreOptions {
    std::uint64_t compact_bytes = 4 << 20;  // the log is compacted past this many bytes
};

// What open() found.
struct OpenReport {
    bool fresh = false;          // nothing there: a new store
    std::uint64_t records = 0;   // records replayed after the snapshot
    std::uint64_t cut_bytes = 0; // bytes of torn tail cut from the log
};

class Store {
public:
    // Opens (or makes) the store in `dir`. Failed, saying why and what to do, on a disk the journal refuses or a record
    // this store cannot read.
    static Status open(Fs& fs, const std::string& dir, const StoreOptions& options, std::unique_ptr<Store>& out,
                       OpenReport* report = nullptr);

    // One durable step, all or none: `changes` to the tab named `tab` (made if it does not exist), and, when `client`
    // is not empty, that client's counter set to `num`. Refused, changing nothing: a change with no tab name, a step
    // with nothing in it, or a `num` other than one past the client's counter (an action applied twice, or one
    // skipped). Failed: the write or the sync failed; what reached the disk is unknown, and the store refuses every
    // later step until it is opened again.
    Status commit(const std::string& tab, std::span<const Change> changes, const std::string& client = std::string(),
                  std::uint64_t num = 0);

    const std::map<std::string, Tab>& tabs() const { return tabs_; }
    const Tab* tab(const std::string& name) const;
    std::uint64_t applied(const std::string& client) const;  // 0 for a client never seen
    bool failed() const { return failed_; }
    const Journal& journal() const { return journal_; }

private:
    Store(Fs& fs, std::string dir, const StoreOptions& options);
    void apply(const std::string& tab, std::span<const Change> changes, const std::string& client, std::uint64_t num);
    Status replay_record(std::span<const std::uint8_t> rec, std::uint64_t seq);
    Status replay_snapshot(std::span<const std::uint8_t> snap);
    Bytes encode_snapshot() const;

    std::string dir_;
    StoreOptions options_;
    Journal journal_;
    bool failed_ = false;
    std::map<std::string, Tab> tabs_;
    std::map<std::string, std::uint64_t> applied_;
};

} // namespace nexus::store
