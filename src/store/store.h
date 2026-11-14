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

// The server's durable state: the sessions (each what it is, a transcript of entries, a queue of prompts and its
// driver's state), the next session id, and each client's applied counter, the number of the last of its actions the
// server applied. The counter is what keeps a window that reconnects, and sends again the actions it has no
// acknowledgement for, from having them applied twice.
//
// A session's id is minted here, from 1: a session is made by a step to the next id whose first change is its info,
// and the next id is durable with it, so an id is never reused, across restarts and compactions. A session is never
// removed.
//
// One journal (journal.h) in the store's directory. Every commit is one record of the journal, appended and synced
// before commit returns Ok: a session's changes and the counter of the action that made them land together or not at
// all, and nothing the server acknowledges or sends is lost to a crash. When the log passes StoreOptions::compact_bytes
// the whole state is written as the journal's snapshot and the log starts again.
//
// A record: u8 format (3); u8 the oldest format a reader must know to read it (3); u64 session (0: none); u32 count,
// then each change: u8 kind and its frame (bytes: a u32 length and the bytes), which holds for an append the entry,
// for an enqueue str prompt, for a dequeue nothing (the queue's oldest prompt leaves it), for a driver the driver, for
// an info the info; then str client (empty: none) and u64 its new counter. An entry: its frame, holding u8 kind, u8
// closed, str text. An info: str friend, str harness, str account, str model, str effort. A driver: u64 next turn,
// u64 open turn, str session, u32 waiting prompts (str). The snapshot: u8 format (4); u8 the oldest format a reader
// must know (4); u64 next session id; u32 sessions, each a frame holding u64 id, the info, u32 entries, u32 prompts
// (str) and the driver; u32 clients, each str client, u64 counter. Little-endian; str is a u32 length and the bytes
// (codec.h).
//
// Across versions (docs/formats.md): a reader skips, in a framed format, what a later one adds at the end of a frame
// or of the record (a change of a kind it does not know is skipped whole), keeps an entry kind it does not know as it
// is (the server shows it as a notice), and refuses only a record or snapshot that names, as the oldest format its
// reader must know, one later than its own. Format 2 records and format 3 snapshots, by session, are the same without
// the frames and the reader byte, and hold entry kinds 0 to 2 only; they are read as they are, never rewritten.
//
// Older stores, keyed by friend tab: a record of format 1 names its tab by the friend's name where format 2 has the
// session; a snapshot of format 2 has no next id, and its tabs (in name order) are each str name, the entries, the
// prompts and the driver; one of format 1 (before drivers) has no driver. They are read, and migrated as they are
// read: each tab becomes a session with its friend, the harness StoreOptions::legacy_harness and the defaults, its
// transcript, queue and driver intact; the snapshot's tabs get ids 1, 2, ... in name order, and a tab a record names
// first the next id. A store found in an older format is compacted at open, so it is in this format from then on (a
// crash during that leaves the older one, read again the same way). The older formats are never written.
namespace nexus::store {

// An entry of a transcript. The store keeps kind as the server gives it (sync's EntryKind); an open entry (closed
// false) is one that may still grow.
struct Entry {
    std::uint8_t kind = 0;
    std::string text;
    bool closed = true;
    bool operator==(const Entry&) const = default;
};

// A session's driver, as the server keeps it across restarts (server/supervisor.h): the next turn number, the turn
// open (0 for none), the last session id the harness reported, and the prompts waiting for a driver.
struct Driver {
    std::uint64_t next_turn = 1;
    std::uint64_t open_turn = 0;
    std::string session;
    std::vector<std::string> waiting;
    bool operator==(const Driver&) const = default;
};

// What a session is (sync's SessionInfo): the friend it is with, its harness, account, model and effort.
struct Info {
    std::string friend_name;
    std::string harness;
    std::string account;
    std::string model;
    std::string effort;
    bool operator==(const Info&) const = default;
};

struct Session {
    Info info;
    std::vector<Entry> transcript;
    std::vector<std::string> queue;
    Driver driver;
    bool operator==(const Session&) const = default;
};

struct Change {
    enum class Kind : std::uint8_t { Append = 1, Enqueue = 2, SetDriver = 3, Dequeue = 4, SetInfo = 5 };
    Kind kind = Kind::Append;
    Entry entry;          // Append: the entry added at the end of the transcript
    std::string prompt;   // Enqueue: the prompt added at the end of the queue. Dequeue: the oldest prompt leaves it
    Driver driver;        // SetDriver: the session's driver, replaced whole
    Info info;            // SetInfo: what the session is, replaced whole; first in a step, it makes the next session
};

struct StoreOptions {
    std::uint64_t compact_bytes = 4 << 20;  // the log is compacted past this many bytes
    std::string legacy_harness = "claude";  // the harness of a session migrated from a friend tab
};

// What open() found.
struct OpenReport {
    bool fresh = false;          // nothing there: a new store
    std::uint64_t records = 0;   // records replayed after the snapshot
    std::uint64_t cut_bytes = 0; // bytes of torn tail cut from the log
    std::uint64_t migrated = 0;  // sessions made from the friend tabs of an older format (then compacted into this one)
};

class Store {
public:
    // Opens (or makes) the store in `dir`, migrating an older format (and compacting it into this one). Failed,
    // saying why and what to do, on a disk the journal refuses, a record this store cannot read, or a migration it
    // could not write.
    static Status open(Fs& fs, const std::string& dir, const StoreOptions& options, std::unique_ptr<Store>& out,
                       OpenReport* report = nullptr);

    // One durable step, all or none: `changes` to session `session`, and, when `client` is not empty, that client's
    // counter set to `num`. A session the store does not hold is made only by a step to next_session() whose first
    // change is a SetInfo. Refused, changing nothing: a change to session 0, a step to a session neither held nor made
    // by it, a step with nothing in it (a client's counter alone names session 0), a dequeue from a queue with nothing
    // left in it, or a `num` other than one past the client's counter (an action applied twice, or one skipped).
    // Failed: the write or the sync failed; what reached the disk is unknown, and the store refuses every later step
    // until it is opened again.
    Status commit(std::uint64_t session, std::span<const Change> changes, const std::string& client = std::string(),
                  std::uint64_t num = 0);

    const std::map<std::uint64_t, Session>& sessions() const { return sessions_; }
    const Session* session(std::uint64_t id) const;
    std::uint64_t next_session() const { return next_session_; }   // the id the next session made gets
    std::uint64_t applied(const std::string& client) const;  // 0 for a client never seen
    bool failed() const { return failed_; }
    const Journal& journal() const { return journal_; }

private:
    Store(Fs& fs, std::string dir, const StoreOptions& options);
    void apply(std::uint64_t session, std::span<const Change> changes, const std::string& client, std::uint64_t num);
    std::string refusal(std::uint64_t session, std::span<const Change> changes) const;
    Status replay_record(std::span<const std::uint8_t> rec, std::uint64_t seq);
    Status replay_snapshot(std::span<const std::uint8_t> snap);
    std::uint64_t legacy_session(const std::string& tab, bool make);
    Bytes encode_snapshot() const;

    std::string dir_;
    StoreOptions options_;
    Journal journal_;
    bool failed_ = false;
    std::map<std::uint64_t, Session> sessions_;
    std::uint64_t next_session_ = 1;
    std::map<std::string, std::uint64_t> applied_;
    std::map<std::string, std::uint64_t> legacy_;   // while an older format is read: each tab's session
    std::uint64_t migrated_ = 0;
    bool legacy_read_ = false;                      // an older format was read: compact at open
};

} // namespace nexus::store
