#pragma once
#include "store/fs.h"
#include "store/journal.h"
#include "store/status.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
// before commit returns Ok, or, in a batch, before flush() returns Ok: a session's changes and the counter of the
// action that made them land together or not at all, and nothing the server acknowledges or sends is lost to a crash. When the log passes StoreOptions::compact_bytes
// the whole state is written as the journal's snapshot and the log starts again.
//
// A record: u8 format (3); u8 the oldest format a reader must know to read it (3); u64 session (0: none); u32 count,
// then each change: u8 kind and its frame (bytes: a u32 length and the bytes), which holds for an append the entry,
// for an enqueue str prompt, for a dequeue nothing (the queue's oldest prompt leaves it), for a driver the driver and
// its tail, for an info the info and its tail; then str client (empty: none) and u64 its new counter. An entry: its
// frame, holding u8 kind, u8 closed, str text. An info: str friend, str harness, str account, str model, str effort;
// its tail: str instance. A driver: u64 next turn, u64 open turn, str session, u32 waiting prompts (str); its tail: u8
// summary (1: the next send carries a summary of the conversation), str the directory its session was made in. The
// snapshot: u8 format (4); u8 the oldest format a reader must know (4); u64 next session id; u32 sessions, each a frame
// holding u64 id, the info, u32 entries, u32 prompts (str), the driver, the driver's tail and the info's tail; u32
// clients, each str client, u64 counter. Little-endian; str is a u32 length and the bytes (codec.h).
//
// The tails were added to formats 3 and 4 at the end of their frames, so a reader from before them skips them, and
// neither format's number nor its reader byte changed. A frame without them reads as their defaults: the friend's
// default instance, no summary owed, and the directory not known. A driver record or session snapshot frame may
// end with 0xA1 and str session account. An absent owner makes a harness ID ineligible for resume.
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

// The formats this build writes, and the oldest format of a reader each declares it can be read by. Every older format
// is still read. 3 and 4 frame each change, entry and session by its length, so that a later format's additions are
// skipped by this build, as long as that format declares this one can read it. A placed build lists them in its
// manifest (server/build_manifest.h), and a switch keeps the rule they make.
constexpr std::uint8_t kRecordFormat = 3;       // 2: by session, unframed; 1: a friend tab's, by name, migrated
constexpr std::uint8_t kRecordReader = 3;       // the oldest record format a reader must know to read ours
constexpr std::uint8_t kSnapshotFormat = 4;     // 3: by session, unframed; 2: friend tabs; 1: before drivers
constexpr std::uint8_t kSnapshotReader = 4;

// An entry of a transcript. The store keeps kind as the server gives it (sync's EntryKind); an open entry (closed
// false) is one that may still grow.
struct Entry {
    std::uint8_t kind = 0;
    std::string text;
    bool closed = true;
    std::uint32_t account_email_end = 0;
    bool operator==(const Entry&) const = default;
};

// A session's driver, as the server keeps it across restarts (server/supervisor.h): the next turn number, the turn
// open (0 for none), the last session id the harness reported, and the prompts waiting for a driver.
struct Driver {
    std::uint64_t next_turn = 1;
    std::uint64_t open_turn = 0;
    std::string session;
    std::vector<std::string> waiting;
    bool summary = false;     // the next send carries a summary of the conversation (its harness session was dropped)
    std::string session_dir{};    // the directory the harness session was made in (empty: not known)
    std::optional<std::string> session_account{};   // the reporting driver's account, absent in older stores
    std::string continuation_prompt{};
    std::vector<std::string> exhausted_accounts{};
    bool model_limit_pending = false;
    bool operator==(const Driver&) const = default;
};

// What a session is (sync's SessionInfo): the friend it is with, its harness, account, model and effort.
struct Info {
    std::string friend_name;
    std::string harness;
    std::string account;
    std::string model;
    std::string effort;
    std::string instance{};     // the friend's instance (empty: its default)
    bool auto_account = false;
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
    // Test-only, for the trace binding's negative check (tests/store/store_lock_walk.cpp --faulty-unlocked), which TLC
    // must reject: open without the lock, as nexus-server did before it had one. Never set by the server or a tool.
    bool test_unlocked = false;
    // Test-only, for migration_walk --fault refuse-unknown-change: replay_record refuses a framed record on a session
    // whose only changes are unknown Change::Kinds, as the store before layer 5 did.
    bool test_refuse_unknown_change = false;
    // Test-only, so simulated newer-format builds (e.g. 5/5 in takeover_walk and fake_build_server) can open and
    // replay records/snapshots they wrote above kRecordFormat / kSnapshotFormat.
    std::uint8_t max_record_reader = kRecordFormat;
    std::uint8_t max_snapshot_reader = kSnapshotFormat;
};

// What open() found.
struct OpenReport {
    bool fresh = false;          // nothing there: a new store
    std::uint64_t records = 0;   // records replayed after the snapshot
    std::uint64_t cut_bytes = 0; // bytes of torn tail cut from the log
    std::uint64_t migrated = 0;  // sessions made from the friend tabs of an older format (then compacted into this one)
    std::uint64_t skipped = 0;   // unknown changes and frames with trailing fields from a later format skipped on replay
};

// The oldest format versions a reader must know to open the store in `dir` as it lies on disk (layer 5's check for
// `builds back`, write-downgrade `builds current`, and `nexus-server store check --for <build>`). 0 when the store has
// no records or no snapshot.
struct StoreReaders {
    bool exists = false;                    // a store with a snapshot or at least one record is in `dir`
    std::uint32_t record_reader = 0;        // highest reader required by any record after the snapshot (0: none)
    std::uint64_t record_seq = 0;           // sequence number of a record requiring `record_reader`
    std::uint32_t snapshot_reader = 0;      // reader required by the snapshot (0: no snapshot)
    std::uint64_t snapshot_seq = 0;
};

class Store {
public:
    // Opens (or makes) the store in `dir`, migrating an older format (and compacting it into this one). First, before
    // anything in `dir` is read or written, the store's lock, exclusive, on the directory itself (Fs::lock): held by
    // this Store until it is destroyed, or until the process ends however it ends, so that at most one process ever has
    // the store open for writing (models/store/StoreLock.tla). Every later read and write of the store goes through
    // that hold, never through the name `dir` again. Busy, having read nothing, when a writer holds it. Failed, saying
    // why and what to do, when only readers held it for all of the wait, where locks cannot be trusted, on a disk the
    // journal refuses, a record this store cannot read, or a migration it could not write.
    static Status open(Fs& fs, const std::string& dir, const StoreOptions& options, std::unique_ptr<Store>& out,
                       OpenReport* report = nullptr);

    // Reads the store in `dir` without changing anything there, and makes nothing there: under the lock, shared and
    // held only while its files are copied (at most kReaderCopyLimitMs by `now_ms`, past which the copy is given up and
    // Failed, so that a reader never keeps a starting server out), the log and the snapshot are copied into `copy`,
    // and the store is opened on the copy (so a torn tail is cut, or a migration written, only there; `report` says
    // what an open would do). Busy, having read nothing, while a writer holds the lock (a running nexus-server);
    // NotFound when there is no store in `dir`. `now_ms` empty: the steady clock.
    static Status inspect(Fs& fs, const std::string& dir, MemFs& copy, std::unique_ptr<Store>& out,
                          OpenReport* report = nullptr, const std::function<std::uint64_t()>& now_ms = {});

    // Reads only the format reader versions required by the snapshot and records in `dir`, under the shared lock and
    // kReaderCopyLimitMs bound of inspect(), changing and making nothing on disk. Succeeds even when `dir` holds a
    // format newer than this build can replay, so a rollback check learns the version on disk. Missing `dir`: Ok with
    // `out.exists` false and zeros. Busy while a writer holds the lock.
    static Status inspect_readers(Fs& fs, const std::string& dir, StoreReaders& out,
                                  const std::function<std::uint64_t()>& now_ms = {});

    // One durable step, all or none: `changes` to session `session`, and, when `client` is not empty, that client's
    // counter set to `num`. A session the store does not hold is made only by a step to next_session() whose first
    // change is a SetInfo. Refused, changing nothing: a change to session 0, a step to a session neither held nor made
    // by it, a step with nothing in it (a client's counter alone names session 0), a dequeue from a queue with nothing
    // left in it, or a `num` other than one past the client's counter (an action applied twice, or one skipped).
    // Failed: the write or the sync failed; what reached the disk is unknown, and the store refuses every later step
    // until it is opened again.
    Status commit(std::uint64_t session, std::span<const Change> changes, const std::string& client = std::string(),
                  std::uint64_t num = 0);

    // A batch (group commit): from begin_batch() until flush(), each commit appends its step's record and applies it
    // here, but does not sync it; flush() makes every step of the batch durable with one sync (the journal's sync
    // covers every record appended before it), then compacts if the log has grown past compact_bytes. Nothing done
    // with a step committed in a batch may leave the process (an acknowledgement, a state sent, a prompt written to a
    // driver) until flush() has returned Ok: a crash before then loses the whole batch, and nothing of it was told.
    // Outside a batch, commit syncs each step before it returns, as it always has. Failed: as commit.
    void begin_batch() { batching_ = true; }
    Status flush();
    bool batching() const { return batching_; }

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
    std::unique_ptr<FileLock> lock_;    // before journal_, so released after the journal's log is closed
    // Ok while the held directory has a name (FileLock::alive); once it has none the store has failed.
    Status held_alive() {
        Status s = lock_ ? lock_->alive() : Status::ok();
        if (!s.is_ok()) {
            failed_ = true;
        }
        return s;
    }
    Journal journal_;
    bool failed_ = false;
    bool batching_ = false;
    std::map<std::uint64_t, Session> sessions_;
    std::uint64_t next_session_ = 1;
    std::map<std::string, std::uint64_t> applied_;
    std::map<std::string, std::uint64_t> legacy_;   // while an older format is read: each tab's session
    std::uint64_t migrated_ = 0;
    std::uint64_t skipped_ = 0;
    bool legacy_read_ = false;                      // an older format was read: compact at open
};

} // namespace nexus::store
