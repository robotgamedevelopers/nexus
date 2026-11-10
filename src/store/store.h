#pragma once
#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "store/status.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The server's durable state, for layer 2's protocol (the spec is models/sync/ on layer2/sync-model: TabSync.tla,
// Session.tla, Blob.tla, and its README's "State, and what is durable"). Every call that changes the state is one
// durable step: when it returns Ok the change is on disk (written and synced), and only then may the protocol send
// anything that depends on it. A refused call changes nothing.
//
// Under the root directory:
//   store/          the store: a journal (journal.h) whose records are the changes below, and blobs/. A restore from
//                   a backup copies an older store/ in; open() detects it and moves to a new generation.
//   store/blobs/    <hex> a complete, hash-checked blob; <hex>.<attempt>.part an upload in progress
//   server/         outside the store, never restored from a backup: a journal holding the incarnation, the
//                   generation, the per-client epoch fence `seen`, and the store's high-water mark (the last record
//                   number of the store acknowledged in this generation)
//   incoming/<tab>/ where an applied action's attachment is placed for the tab's AI (StoreOptions::incoming)
namespace nexus::store {

using TabId = std::uint64_t;
using ClientId = std::array<std::uint8_t, 16>;

constexpr std::size_t kQueueCap = 64;                     // prompts queued per tab; a full queue refuses
constexpr std::uint64_t kMaxBlobSize = 100ull << 20;     // 100 MB

enum class Author : std::uint8_t { Person = 1, Friend = 2 };

// A tab's operations (TabSync.tla's Step): new opens an entry from the friend, grow appends text to the open entry,
// close closes it, send appends a closed entry from the person, enq queues a prompt, deliver moves the oldest queued
// prompt to the end of the transcript. new, send and deliver close the open entry first.
enum class OpKind : std::uint8_t { New = 1, Grow, Close, Send, Enq, Deliver };

// An entry of the transcript or a queued prompt. Its tag is (gen, id): id is the tab's version just after the
// operation that made it, gen the generation it was made in. Text never changes once written; an open entry only
// grows until it is closed.
struct Item {
    std::uint64_t id = 0;
    std::uint64_t gen = 0;
    Author author = Author::Person;
    bool open = false;
    std::string text;
    std::optional<Hash> blob;
};

// One operation as a diff carries it: version base + i is the tab after the i-th. id and gen are the item it made
// (new, send, enq); text is what it wrote (new, send, enq, grow).
struct TabOp {
    OpKind kind = OpKind::New;
    std::uint64_t id = 0;
    std::uint64_t gen = 0;
    Author author = Author::Person;
    std::string text;
    std::optional<Hash> blob;
};

// One change asked of a tab.
struct Change {
    TabId tab = 0;
    OpKind kind = OpKind::New;
    std::string text;
    std::optional<Hash> blob;
};

// A tab's tail: its version, the last entries from position lo (1-based, as in the spec), and the whole queue.
struct Snap {
    std::uint64_t version = 0;
    std::uint64_t lo = 1;
    std::vector<Item> entries;
    std::vector<Item> queue;
};

struct HelloResult {
    bool superseded = false;  // the epoch is below the one last welcomed: answer Superseded
    std::uint64_t inc = 0;
    std::uint64_t gen = 0;
    std::uint64_t applied = 0;
};

// A numbered action of the person (Session.tla): send or queue text, maybe with an attachment that is already
// complete on the server; file_name is the name it is placed under (sanitised and made unique by the store).
struct Action {
    enum class Kind : std::uint8_t { Send = 1, Queue = 2 };
    std::uint64_t n = 0;
    TabId tab = 0;
    Kind kind = Kind::Send;
    std::string text;
    std::optional<Hash> blob;
    std::string file_name;
};

struct ActResult {
    std::uint64_t applied = 0;           // the highest number applied: ActAck's
    std::vector<std::uint64_t> refused;  // numbers applied as refusals: a queue action on a full queue
};

struct BlobState {
    bool known = false;  // offered
    std::uint64_t size = 0;
    std::uint64_t held = 0;
    std::uint64_t attempt = 0;
    bool complete = false;
};

struct StoreOptions {
    std::uint64_t recent_ops = 256;        // operations kept per tab for diffs (TabSync's Recent)
    std::uint64_t compact_bytes = 4 << 20;  // the store's log is compacted past this many bytes
    std::uint64_t meta_compact_bytes = 64 << 10;
    std::function<std::string(TabId)> incoming;  // where tab's attachments are placed; default root/incoming/<tab>
};

// What open() found.
struct OpenReport {
    bool fresh = false;           // no store and no server record: a new install
    bool restored = false;        // the store is not the one the server record describes: a new generation
    std::uint64_t store_cut = 0;  // bytes of torn tail cut from the store's log
    std::uint64_t meta_cut = 0;
    std::uint64_t placements_redone = 0;
};

class Store {
public:
    // Opens (or creates) the state under root. now_secs is the wall clock in seconds since 1970, an argument so that
    // tests choose it: the incarnation is max(stored + 1, now_secs), so it goes forward even when the server record
    // itself was restored.
    static Status open(Fs& fs, const std::string& root, std::uint64_t now_secs, const StoreOptions& options,
                       std::unique_ptr<Store>& out, OpenReport* report = nullptr);

    std::uint64_t incarnation() const { return inc_; }
    std::uint64_t generation() const { return gen_; }
    bool failed() const { return failed_; }

    // Tabs. A tab never touched is empty at version 0.
    Status change(std::span<const Change> changes);  // all or none, one durable step
    Status open_entry(TabId tab, std::string text) { return one({tab, OpKind::New, std::move(text), {}}); }
    Status grow(TabId tab, std::string text) { return one({tab, OpKind::Grow, std::move(text), {}}); }
    Status close(TabId tab) { return one({tab, OpKind::Close, {}, {}}); }
    Status send(TabId tab, std::string text) { return one({tab, OpKind::Send, std::move(text), {}}); }
    Status enqueue(TabId tab, std::string text) { return one({tab, OpKind::Enq, std::move(text), {}}); }
    Status deliver(TabId tab) { return one({tab, OpKind::Deliver, {}, {}}); }

    std::vector<TabId> tabs() const;
    std::uint64_t version(TabId tab) const;
    Snap snapshot(TabId tab, std::size_t window) const;
    // The operations after `base`, at most max_ops: false when base is ahead of the tab or older than the operations
    // kept (then send a snapshot).
    bool diff(TabId tab, std::uint64_t base, std::size_t max_ops, std::vector<TabOp>& out) const;
    // The entries before position lo, at most page (TabSync's More/Older); empty when lo is out of range.
    std::vector<Item> older(TabId tab, std::uint64_t lo, std::size_t page) const;
    // An item's text from byte off, at most max bytes, cut back to a UTF-8 boundary; false when there is no such item
    // or off is at or past its end.
    bool content(TabId tab, std::uint64_t id, std::uint64_t off, std::size_t max, std::string& out) const;
    const Item* item(TabId tab, std::uint64_t id) const;

    // Session.
    Status hello(const ClientId& c, std::uint64_t ep, std::uint64_t sgen, std::uint64_t first, HelloResult& out);
    Status apply(const ClientId& c, std::span<const Action> acts, ActResult& out);
    std::uint64_t applied(const ClientId& c) const;
    std::uint64_t seen(const ClientId& c) const;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> skipped(const ClientId& c) const;  // lost by a restore

    // Blobs (Blob.tla).
    Status offer(const Hash& h, std::uint64_t size, BlobState& out);
    Status chunk(const Hash& h, std::uint64_t attempt, std::uint64_t off, std::span<const std::uint8_t> bytes,
                 BlobState& out);
    BlobState blob(const Hash& h) const;
    Status read_blob(const Hash& h, std::uint64_t off, std::uint64_t max, Bytes& out);

    // The store's journal, for the tests.
    const Journal& store_journal() const { return store_; }

    // One fact of a record (store.cpp has the encoding). A record is the facts of one durable step.
    struct Fact;

private:
    struct Tab {
        std::uint64_t version = 0;
        std::vector<Item> transcript;
        std::vector<Item> queue;
        std::deque<TabOp> recent;  // the last recent_ops operations: versions version - recent.size() + 1 .. version
        std::unordered_map<std::uint64_t, std::size_t> where;  // entry id -> position in the transcript
    };
    struct Client {
        std::uint64_t applied = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> skipped;
    };
    struct Blob {
        std::uint64_t size = 0;
        std::uint64_t attempt = 0;
        std::uint64_t held = 0;
        bool complete = false;
    };
    struct Placement {
        Hash blob{};
        std::string dir;
        std::string name;
        bool operator==(const Placement&) const = default;
    };

    Store(Fs& fs, std::string root, const StoreOptions& options);
    Status one(Change c) { return change(std::span<const Change>(&c, 1)); }
    Status refuse_if_failed() const;
    Status commit(const std::vector<Fact>& facts);
    Status meta_commit(const std::vector<Fact>& facts);
    void apply_fact(const Fact& f);
    void apply_op(TabId tab, OpKind kind, const std::string& text, const std::optional<Hash>& blob);
    Status replay_store(const Journal::Recovery& r);
    Status replay_meta(const Journal::Recovery& r, bool& any);
    Bytes encode_store() const;
    Bytes encode_meta() const;
    Status recover_blobs();
    Status place(const Placement& p);
    std::string blob_path(const Hash& h) const;
    std::string part_path(const Hash& h, std::uint64_t attempt) const;
    std::string unique_name(const std::string& dir, const std::string& want, const std::vector<Placement>& taken);

    Fs& fs_;
    std::string root_;
    StoreOptions options_;
    Journal store_;
    Journal meta_;
    bool failed_ = false;

    // the store (replaced as a whole by a restore)
    std::uint64_t store_gen_ = 0;
    std::map<TabId, Tab> tabs_;
    std::map<ClientId, Client> clients_;
    std::map<Hash, Blob> blobs_;
    std::vector<Placement> placements_;  // recorded, not yet known to be done

    // outside the store
    std::uint64_t inc_ = 0;
    std::uint64_t gen_ = 0;
    std::uint64_t mark_ = 0;
    std::map<ClientId, std::uint64_t> seen_;
};

// "file name.pdf" -> "file_name.pdf": letters, digits, '.', '-' and '_' kept, others '_', no leading dot, at most 128
// bytes, never empty.
std::string sanitise_file_name(const std::string& name);

} // namespace nexus::store
