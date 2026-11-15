// The server's durable state (store.h): the sessions, the next session id and each client's applied counter, over one
// journal.

#include "store/store.h"

#include <chrono>

namespace nexus::store {

namespace {

// The entry kinds the unframed formats had: Person 0, Friend 1, Notice 2. A later kind (Tool 3) comes only in a framed
// format, and a framed format may hold kinds this build does not know: they are kept as they are, and the server shows
// them as notices (server/durable.cpp).
constexpr std::uint8_t kUnframedKinds = 3;

void put_entry(Writer& w, const Entry& e) {
    Writer frame;
    frame.u8(e.kind);
    frame.u8(e.closed ? 1 : 0);
    frame.str(e.text);
    w.bytes(frame.out());
}

// An entry of a framed format: what it adds past the text is skipped.
bool get_entry(Reader& r, Entry& e) {
    const Bytes frame = r.bytes();
    Reader in(frame);
    e.kind = in.u8();
    const std::uint8_t closed = in.u8();
    e.text = in.str();
    e.closed = closed == 1;
    return in.ok() && closed <= 1;
}

// An entry of an unframed format, whose kinds are only the first three.
bool get_unframed_entry(Reader& r, Entry& e) {
    e.kind = r.u8();
    const std::uint8_t closed = r.u8();
    e.text = r.str();
    e.closed = closed == 1;
    return closed <= 1 && e.kind < kUnframedKinds;
}

void put_driver(Writer& w, const Driver& d) {
    w.u64(d.next_turn);
    w.u64(d.open_turn);
    w.str(d.session);
    w.u32(std::uint32_t(d.waiting.size()));
    for (const std::string& p : d.waiting) {
        w.str(p);
    }
}

void get_driver(Reader& r, Driver& d) {
    d.next_turn = r.u64();
    d.open_turn = r.u64();
    d.session = r.str();
    d.waiting.clear();
    std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        d.waiting.push_back(r.str());
    }
}

void put_info(Writer& w, const Info& i) {
    w.str(i.friend_name);
    w.str(i.harness);
    w.str(i.account);
    w.str(i.model);
    w.str(i.effort);
}

void get_info(Reader& r, Info& i) {
    i.friend_name = r.str();
    i.harness = r.str();
    i.account = r.str();
    i.model = r.str();
    i.effort = r.str();
}

// What the framed formats append, at the end of a frame (store.h): a driver's summary flag and its session's directory,
// and an info's instance. A frame written before they were has none of them, and each reads as its default: no
// summary owed, the directory unknown (the driver resumes its session), and the friend's default instance.
void put_driver_tail(Writer& w, const Driver& d) {
    w.u8(d.summary ? 1 : 0);
    w.str(d.session_dir);
}

bool get_driver_tail(Reader& r, Driver& d) {
    d.summary = false;
    d.session_dir.clear();
    if (r.done()) {
        return true;
    }
    const std::uint8_t summary = r.u8();
    d.summary = summary == 1;
    d.session_dir = r.str();
    return summary <= 1;
}

void put_info_tail(Writer& w, const Info& i) {
    w.str(i.instance);
}

void get_info_tail(Reader& r, Info& i) {
    i.instance = r.done() ? std::string() : r.str();
}

// A change's fields: an append's entry, an enqueue's prompt, a driver, an info; a dequeue has none.
void put_change(Writer& w, const Change& c) {
    if (c.kind == Change::Kind::Append) {
        put_entry(w, c.entry);
    } else if (c.kind == Change::Kind::Enqueue) {
        w.str(c.prompt);
    } else if (c.kind == Change::Kind::SetDriver) {
        put_driver(w, c.driver);
        put_driver_tail(w, c.driver);
    } else if (c.kind == Change::Kind::SetInfo) {
        put_info(w, c.info);
        put_info_tail(w, c.info);
    }
}

Bytes encode_record(std::uint64_t session, std::span<const Change> changes, const std::string& client,
                    std::uint64_t num) {
    Writer w;
    w.u8(kRecordFormat);
    w.u8(kRecordReader);
    w.u64(session);
    w.u32(std::uint32_t(changes.size()));
    for (const Change& c : changes) {
        w.u8(std::uint8_t(c.kind));
        Writer frame;
        put_change(frame, c);
        w.bytes(frame.out());
    }
    w.str(client);
    w.u64(num);
    return std::move(w.out());
}

Status unreadable(const std::string& dir, const std::string& what, const std::string& why = std::string()) {
    return Status::failed("store " + dir + ": " + what + (why.empty() ? std::string() : " (" + why + ")") +
                          " is not one this store writes (a newer nexus-server's, or damaged after its checksum was "
                          "taken); run the nexus-server that wrote it, or move " + dir + " aside to start empty");
}

// A framed record or snapshot that declares it needs a reader of a format this build does not know.
Status tooNew(const std::string& dir, const std::string& what, unsigned format, unsigned reader, unsigned ours) {
    return Status::failed("store " + dir + ": " + what + " is in format " + std::to_string(format) + ", which only a "
                          "nexus-server that reads format " + std::to_string(reader) + " or later can read, and this "
                          "one reads up to " + std::to_string(ours) + "; run the newer nexus-server that wrote it");
}

} // namespace

Store::Store(Fs& fs, std::string dir, const StoreOptions& options)
    : dir_(std::move(dir)), options_(options), journal_(fs, dir_) {}

Status Store::open(Fs& fs, const std::string& dir, const StoreOptions& options, std::unique_ptr<Store>& out,
                   OpenReport* report) {
    std::unique_ptr<Store> s(new Store(fs, dir, options));
    // The lock, on the directory itself, before anything in it is read or written (the directory made first if it is
    // not there: a make_dir changes nothing a store holds). From here on the directory is the hold's, not its name's.
    Status st = fs.make_dir(dir);
    if (!st.is_ok()) {
        return st;
    }
    if (!options.test_unlocked) {
        st = fs.lock(dir, LockMode::Exclusive, s->lock_);
        if (st.code() == Status::Code::Busy) {
            return Status::busy("store " + dir + " is open in another process, which holds its lock (on the directory "
                                "itself): " + st.message());
        }
        if (!st.is_ok()) {
            return Status::failed("store " + dir + ": its lock cannot be taken: " + st.message());
        }
    }
    Journal::Recovery rec;
    st = s->journal_.open(rec);
    if (!st.is_ok()) {
        return st;
    }
    if (rec.snapshot_seq > 0 && !(st = s->replay_snapshot(rec.snapshot)).is_ok()) {
        return st;
    }
    for (std::size_t i = 0; i < rec.records.size(); ++i) {
        if (!(st = s->replay_record(rec.records[i], rec.snapshot_seq + i + 1)).is_ok()) {
            return st;
        }
    }
    // An older format, migrated as it was read: written in this one before any step, so that nothing later depends on
    // reading the older one again.
    if (s->legacy_read_) {
        if (!(st = s->journal_.compact(s->encode_snapshot())).is_ok()) {
            return Status::failed("store " + dir + ": its friend tabs were read as sessions, but writing them in the "
                                  "sessions format failed (" + st.message() + "); the store is as it was, so run "
                                  "nexus-server again once the disk takes writes");
        }
        s->legacy_.clear();
    }
    if (report != nullptr) {
        report->fresh = s->journal_.last_seq() == 0;
        report->records = rec.records.size();
        report->cut_bytes = rec.cut_bytes;
        report->migrated = s->migrated_;
    }
    out = std::move(s);
    return Status::ok();
}

Status Store::inspect(Fs& fs, const std::string& dir, MemFs& copy, std::unique_ptr<Store>& out, OpenReport* report,
                      const std::function<std::uint64_t()>& now_ms) {
    std::function<std::uint64_t()> now = now_ms;
    if (!now) {
        now = [] {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    }
    std::vector<std::string> names;
    Status st = fs.list(dir, names);
    if (st.code() == Status::Code::NotFound) {
        return Status::not_found("no store in " + dir);
    }
    if (!st.is_ok()) {
        return st;
    }
    {
        std::unique_ptr<FileLock> lock;
        st = fs.lock(dir, LockMode::Shared, lock);
        if (st.code() == Status::Code::Busy) {
            return Status::busy("store " + dir + " is open in a running nexus-server, which holds its lock; it is not "
                                "read while a server writes it (`nexus-server stop` stops the server)");
        }
        if (st.code() == Status::Code::Refused) {
            return Status::busy("store " + dir + " has a nexus-server starting on it (it waits for the store's lock, and "
                                "a reader gives way to it), so it was not read; try again once the server serves, or "
                                "after `nexus-server stop`");
        }
        if (!st.is_ok()) {
            return Status::failed("store " + dir + ": its lock cannot be taken: " + st.message());
        }
        const std::uint64_t start = now();
        if (!(st = copy.make_dir(dir)).is_ok()) {
            return st;
        }
        const Status late = Status::failed("store " + dir + ": its copy took longer than " +
                                           std::to_string(kReaderCopyLimitMs) + " ms under its shared lock, so it was "
                                           "given up and the lock let go, so that no starting server waits on this "
                                           "reader; nothing was read (check the disk, then try again)");
        // The journal's files, as they are under the lock (through the hold, so the directory locked is the one read);
        // the temporaries of a compaction too, which open removes; nothing else. Each is read in chunks of
        // kReaderChunkBytes with the clock asked between them, so one slow read cannot hold the lock past the limit by
        // more than a chunk's read: a copy that outlasts it lets the lock go and fails this reader, never a server
        // waiting to start.
        for (const char* name : {"log", "snapshot", "snapshot.tmp", "log.new"}) {
            const std::string path = dir + "/" + name;
            std::uint64_t size = 0;
            st = fs.size(path, size);
            if (st.code() == Status::Code::NotFound) {
                continue;
            }
            if (!st.is_ok() || !(st = copy.create(path)).is_ok()) {
                return st;
            }
            for (std::uint64_t off = 0; off < size;) {
                if (now() - start > kReaderCopyLimitMs) {
                    return late;
                }
                Bytes bytes;
                if (!(st = fs.read(path, off, kReaderChunkBytes, bytes)).is_ok()) {
                    return st;
                }
                if (bytes.empty()) {
                    break;      // the file is shorter than it was: a writer cut it, which no writer does under the lock
                }
                if (!(st = copy.append(path, bytes)).is_ok()) {
                    return st;
                }
                off += bytes.size();
            }
            if (now() - start > kReaderCopyLimitMs) {
                return late;
            }
            if (!(st = copy.sync(path)).is_ok()) {
                return st;
            }
        }
        if (!(st = copy.sync_dir(dir)).is_ok()) {
            return st;
        }
    }
    return open(copy, dir, StoreOptions{}, out, report);
}

const Session* Store::session(std::uint64_t id) const {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

std::uint64_t Store::applied(const std::string& client) const {
    auto it = applied_.find(client);
    return it == applied_.end() ? 0 : it->second;
}

void Store::apply(std::uint64_t session, std::span<const Change> changes, const std::string& client,
                  std::uint64_t num) {
    if (!changes.empty()) {
        if (sessions_.find(session) == sessions_.end()) {
            next_session_ = session + 1;    // refusal() let it be made only as the next
        }
        Session& t = sessions_[session];
        for (const Change& c : changes) {
            if (c.kind == Change::Kind::Append) {
                t.transcript.push_back(c.entry);
            } else if (c.kind == Change::Kind::Enqueue) {
                t.queue.push_back(c.prompt);
            } else if (c.kind == Change::Kind::Dequeue) {
                t.queue.erase(t.queue.begin());     // commit and replay refuse a dequeue from an empty queue
            } else if (c.kind == Change::Kind::SetInfo) {
                t.info = c.info;
            } else {
                t.driver = c.driver;
            }
        }
    }
    if (!client.empty()) {
        applied_[client] = num;
    }
}

// Why a step of `changes` to `session` cannot be taken, or empty when it can: a session neither held nor made by the
// step as the next, or a dequeue that finds the queue empty at its point of the step.
std::string Store::refusal(std::uint64_t session, std::span<const Change> changes) const {
    if (changes.empty()) {
        return session == 0 ? std::string() : "a step with no change names session " + std::to_string(session);
    }
    if (session == 0) {
        return "a change names no session";
    }
    const Session* t = this->session(session);
    if (t == nullptr && session != next_session_) {
        return "session " + std::to_string(session) + " is not held, and the next one made is " +
            std::to_string(next_session_);
    }
    if (t == nullptr && changes.front().kind != Change::Kind::SetInfo) {
        return "session " + std::to_string(session) + " is not held, and the step does not make it";
    }
    std::size_t queued = t == nullptr ? 0 : t->queue.size();
    for (const Change& c : changes) {
        if (c.kind == Change::Kind::Enqueue) {
            ++queued;
        } else if (c.kind == Change::Kind::Dequeue) {
            if (queued == 0) {
                return "session " + std::to_string(session) + ": a dequeue from a queue with nothing left in it";
            }
            --queued;
        }
    }
    return std::string();
}

Status Store::commit(std::uint64_t session, std::span<const Change> changes, const std::string& client,
                     std::uint64_t num) {
    if (failed_ || journal_.failed()) {
        return Status::failed("store " + dir_ + ": an earlier write or sync failed, so what is on disk is unknown; "
                              "restart the server to reopen the store from what is durable");
    }
    if (changes.empty() && client.empty()) {
        return Status::refused("store " + dir_ + ": a step with no change and no client changes nothing");
    }
    const std::string refused = refusal(session, changes);
    if (!refused.empty()) {
        return Status::refused("store " + dir_ + ": " + refused);
    }
    if (!client.empty() && num != applied(client) + 1) {
        return Status::refused("store " + dir_ + ": client " + client + " has applied up to " +
                               std::to_string(applied(client)) + ", so its next action is " +
                               std::to_string(applied(client) + 1) + ", not " + std::to_string(num));
    }
    Status s = journal_.append(encode_record(session, changes, client, num));
    if (s.is_ok() && !batching_) {
        s = held_alive();
    }
    if (s.is_ok() && !batching_) {
        s = journal_.sync();
    }
    if (!s.is_ok()) {
        failed_ = true;
        return s;
    }
    apply(session, changes, client, num);
    if (batching_) {
        return Status::ok();    // durable, and compacted if need be, at flush()
    }
    // The step is durable whatever compaction does: a failure here only stops later steps.
    if (journal_.log_bytes() > options_.compact_bytes && !(s = journal_.compact(encode_snapshot())).is_ok()) {
        failed_ = true;
    }
    return Status::ok();
}

Status Store::flush() {
    batching_ = false;
    if (failed_ || journal_.failed()) {
        return Status::failed("store " + dir_ + ": an earlier write or sync failed, so what is on disk is unknown; "
                              "restart the server to reopen the store from what is durable");
    }
    // The directory still named (one fstat per batch), then SyncFile("log") once, for every record the batch appended (a
    // no-op when it appended none).
    Status s = held_alive();
    if (s.is_ok()) {
        s = journal_.sync();
    }
    if (!s.is_ok()) {
        failed_ = true;
        return s;
    }
    // The batch is durable whatever compaction does: a failure here only stops later steps.
    if (journal_.log_bytes() > options_.compact_bytes && !(s = journal_.compact(encode_snapshot())).is_ok()) {
        failed_ = true;
    }
    return Status::ok();
}

// The session an older format's friend tab became; with `make`, a tab not seen before becomes the next session.
std::uint64_t Store::legacy_session(const std::string& tab, bool make) {
    auto it = legacy_.find(tab);
    if (it != legacy_.end()) {
        return it->second;
    }
    if (!make) {
        return 0;
    }
    const std::uint64_t id = next_session_++;
    Session& s = sessions_[id];
    s.info.friend_name = tab;
    s.info.harness = options_.legacy_harness;
    legacy_[tab] = id;
    ++migrated_;
    return id;
}

Status Store::replay_record(std::span<const std::uint8_t> rec, std::uint64_t seq) {
    const std::string what = "record " + std::to_string(seq) + " of its journal";
    Reader r(rec);
    const std::uint8_t format = r.u8();
    if (format == 0) {
        return unreadable(dir_, what);
    }
    const bool framed = format >= 3;
    if (framed) {
        // A framed format names the oldest one its reader must know: a later one this build can read says so.
        const std::uint8_t reader = r.u8();
        if (reader > kRecordFormat) {
            return tooNew(dir_, what, format, reader, kRecordFormat);
        }
    }
    std::uint64_t session = 0;
    std::string tab;
    if (format >= 2) {
        session = r.u64();
    } else {
        tab = r.str();
    }
    std::uint32_t n = r.u32();
    std::vector<Change> changes;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        Change c;
        const std::uint8_t kind = r.u8();
        Bytes frame;
        if (framed) {
            frame = r.bytes();
        }
        Reader framedIn(frame);
        Reader& in = framed ? framedIn : r;
        if (kind == std::uint8_t(Change::Kind::Append)) {
            c.kind = Change::Kind::Append;
            if (framed ? !get_entry(in, c.entry) : !get_unframed_entry(in, c.entry)) {
                return unreadable(dir_, what, "an entry of kind " + std::to_string(c.entry.kind) + ", which format " +
                    std::to_string(format) + " does not have, or a bad closed flag");
            }
        } else if (kind == std::uint8_t(Change::Kind::Enqueue)) {
            c.kind = Change::Kind::Enqueue;
            c.prompt = in.str();
        } else if (kind == std::uint8_t(Change::Kind::SetDriver)) {
            c.kind = Change::Kind::SetDriver;
            get_driver(in, c.driver);
            if (framed && !get_driver_tail(in, c.driver)) {
                return unreadable(dir_, what, "a driver's summary flag is neither 0 nor 1");
            }
        } else if (kind == std::uint8_t(Change::Kind::Dequeue)) {
            c.kind = Change::Kind::Dequeue;
        } else if (kind == std::uint8_t(Change::Kind::SetInfo) && format >= 2) {
            c.kind = Change::Kind::SetInfo;
            get_info(in, c.info);
            if (framed) {
                get_info_tail(in, c.info);
            }
        } else if (framed) {
            continue;   // a change a later format adds: skipped, its frame read past
        } else {
            return unreadable(dir_, what, "a change of kind " + std::to_string(kind));
        }
        if (framed && !framedIn.ok()) {
            return unreadable(dir_, what);
        }
        changes.push_back(std::move(c));
    }
    std::string client = r.str();
    std::uint64_t num = r.u64();
    // An unframed record ends there; a framed one may go on with what a later format adds, which is skipped.
    if (!r.ok() || (!framed && !r.done())) {
        return unreadable(dir_, what);
    }
    if (format == 1) {
        // A friend tab's record: its tab is a session, made (as the next) if no snapshot or record named it before.
        legacy_read_ = true;
        if (!changes.empty() && tab.empty()) {
            return unreadable(dir_, what);
        }
        if (!changes.empty()) {
            session = legacy_session(tab, true);
        }
    }
    const std::string refused = refusal(session, changes);
    if (!refused.empty()) {
        return unreadable(dir_, what, refused);
    }
    apply(session, changes, client, num);
    return Status::ok();
}

Bytes Store::encode_snapshot() const {
    Writer w;
    w.u8(kSnapshotFormat);
    w.u8(kSnapshotReader);
    w.u64(next_session_);
    w.u32(std::uint32_t(sessions_.size()));
    for (const auto& [id, t] : sessions_) {
        Writer frame;
        frame.u64(id);
        put_info(frame, t.info);
        frame.u32(std::uint32_t(t.transcript.size()));
        for (const Entry& e : t.transcript) {
            put_entry(frame, e);
        }
        frame.u32(std::uint32_t(t.queue.size()));
        for (const std::string& q : t.queue) {
            frame.str(q);
        }
        put_driver(frame, t.driver);
        put_driver_tail(frame, t.driver);
        put_info_tail(frame, t.info);
        w.bytes(frame.out());
    }
    w.u32(std::uint32_t(applied_.size()));
    for (const auto& [client, num] : applied_) {
        w.str(client);
        w.u64(num);
    }
    return std::move(w.out());
}

Status Store::replay_snapshot(std::span<const std::uint8_t> snap) {
    const std::string what = "its snapshot";
    Reader r(snap);
    const std::uint8_t format = r.u8();
    if (format == 0) {
        return unreadable(dir_, what);
    }
    const bool framed = format >= 4;
    if (framed) {
        const std::uint8_t reader = r.u8();
        if (reader > kSnapshotFormat) {
            return tooNew(dir_, what, format, reader, kSnapshotFormat);
        }
    }
    const bool bySession = format >= 3;
    if (bySession) {
        next_session_ = r.u64();
        if (next_session_ == 0) {
            return unreadable(dir_, what);
        }
    } else {
        legacy_read_ = true;
    }
    std::uint32_t count = r.u32();
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Bytes frame;
        if (framed) {
            frame = r.bytes();
        }
        Reader framedIn(frame);
        Reader& in = framed ? framedIn : r;
        std::uint64_t id = 0;
        Info info;
        if (bySession) {
            id = in.u64();
            get_info(in, info);
            if (id == 0 || id >= next_session_ || sessions_.count(id) != 0) {
                return unreadable(dir_, what);
            }
        } else {
            const std::string tab = in.str();
            if (legacy_.count(tab) != 0) {
                return unreadable(dir_, what);
            }
            id = legacy_session(tab, true);     // in the order written: by name, so ids 1, 2, ... in name order
            info = sessions_[id].info;
        }
        Session& t = sessions_[id];
        t.info = std::move(info);
        std::uint32_t entries = in.u32();
        for (std::uint32_t k = 0; k < entries && in.ok(); ++k) {
            Entry e;
            if (framed ? !get_entry(in, e) : !get_unframed_entry(in, e)) {
                return unreadable(dir_, what, "session " + std::to_string(id) + " has an entry of kind " +
                    std::to_string(e.kind) + ", which format " + std::to_string(format) + " does not have, or a bad "
                    "closed flag");
            }
            t.transcript.push_back(std::move(e));
        }
        std::uint32_t prompts = in.u32();
        for (std::uint32_t k = 0; k < prompts && in.ok(); ++k) {
            t.queue.push_back(in.str());
        }
        if (format != 1) {
            get_driver(in, t.driver);
        }
        if (framed) {
            if (!get_driver_tail(in, t.driver)) {
                return unreadable(dir_, what, "session " + std::to_string(id) + " has a summary flag neither 0 nor 1");
            }
            get_info_tail(in, t.info);
        }
        // A framed session may go on with what a later format adds, which is skipped.
        if (framed && !framedIn.ok()) {
            return unreadable(dir_, what);
        }
    }
    std::uint32_t clients = r.u32();
    for (std::uint32_t i = 0; i < clients && r.ok(); ++i) {
        std::string c = r.str();
        applied_[c] = r.u64();
    }
    if (!r.ok() || (!framed && !r.done())) {
        return unreadable(dir_, what);
    }
    return Status::ok();
}

} // namespace nexus::store
