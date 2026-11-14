// The server's durable state (store.h): the sessions, the next session id and each client's applied counter, over one
// journal.

#include "store/store.h"

namespace nexus::store {

namespace {

constexpr std::uint8_t kRecordFormat = 3;     // 2: a session's info without its instance, still read; 1: a friend
                                              // tab's, by name, still read and migrated
constexpr std::uint8_t kRecordNoInstance = 2;
constexpr std::uint8_t kSnapshotFormat = 4;   // 3: without instances, still read; 2: friend tabs; 1: friend tabs before
                                              // drivers; both still read and migrated
constexpr std::uint8_t kSnapshotNoInstance = 3;

void put_entry(Writer& w, const Entry& e) {
    w.u8(e.kind);
    w.u8(e.closed ? 1 : 0);
    w.str(e.text);
}

bool get_entry(Reader& r, Entry& e) {
    e.kind = r.u8();
    std::uint8_t closed = r.u8();
    e.text = r.str();
    e.closed = closed == 1;
    return closed <= 1;
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
    w.str(i.instance);
}

void get_info(Reader& r, Info& i, bool withInstance) {
    i.friend_name = r.str();
    i.harness = r.str();
    i.account = r.str();
    i.model = r.str();
    i.effort = r.str();
    i.instance = withInstance ? r.str() : std::string();   // a session from before instances: its friend's default
}

Bytes encode_record(std::uint64_t session, std::span<const Change> changes, const std::string& client,
                    std::uint64_t num) {
    Writer w;
    w.u8(kRecordFormat);
    w.u64(session);
    w.u32(std::uint32_t(changes.size()));
    for (const Change& c : changes) {
        w.u8(std::uint8_t(c.kind));
        if (c.kind == Change::Kind::Append) {
            put_entry(w, c.entry);
        } else if (c.kind == Change::Kind::Enqueue) {
            w.str(c.prompt);
        } else if (c.kind == Change::Kind::SetDriver) {
            put_driver(w, c.driver);
        } else if (c.kind == Change::Kind::SetInfo) {
            put_info(w, c.info);
        }
    }
    w.str(client);
    w.u64(num);
    return std::move(w.out());
}

Status unreadable(const std::string& dir, const std::string& what) {
    return Status::failed("store " + dir + ": " + what + " is not one this store writes (a newer nexus-server's, or "
                          "damaged after its checksum was taken); run the nexus-server that wrote it, or move " + dir +
                          " aside to start empty");
}

} // namespace

Store::Store(Fs& fs, std::string dir, const StoreOptions& options)
    : dir_(std::move(dir)), options_(options), journal_(fs, dir_) {}

Status Store::open(Fs& fs, const std::string& dir, const StoreOptions& options, std::unique_ptr<Store>& out,
                   OpenReport* report) {
    std::unique_ptr<Store> s(new Store(fs, dir, options));
    Journal::Recovery rec;
    Status st = s->journal_.open(rec);
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
    if (s.is_ok()) {
        s = journal_.sync();
    }
    if (!s.is_ok()) {
        failed_ = true;
        return s;
    }
    apply(session, changes, client, num);
    // The step is durable whatever compaction does: a failure here only stops later steps.
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
    Reader r(rec);
    const std::uint8_t format = r.u8();
    if (format != 1 && format != kRecordNoInstance && format != kRecordFormat) {
        return unreadable(dir_, "record " + std::to_string(seq));
    }
    std::uint64_t session = 0;
    std::string tab;
    if (format != 1) {
        session = r.u64();
    } else {
        tab = r.str();
    }
    std::uint32_t n = r.u32();
    std::vector<Change> changes;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        Change c;
        std::uint8_t kind = r.u8();
        if (kind == std::uint8_t(Change::Kind::Append)) {
            c.kind = Change::Kind::Append;
            if (!get_entry(r, c.entry)) {
                return unreadable(dir_, "record " + std::to_string(seq));
            }
        } else if (kind == std::uint8_t(Change::Kind::Enqueue)) {
            c.kind = Change::Kind::Enqueue;
            c.prompt = r.str();
        } else if (kind == std::uint8_t(Change::Kind::SetDriver)) {
            c.kind = Change::Kind::SetDriver;
            get_driver(r, c.driver);
        } else if (kind == std::uint8_t(Change::Kind::Dequeue)) {
            c.kind = Change::Kind::Dequeue;
        } else if (kind == std::uint8_t(Change::Kind::SetInfo) && format != 1) {
            c.kind = Change::Kind::SetInfo;
            get_info(r, c.info, format == kRecordFormat);
        } else {
            return unreadable(dir_, "record " + std::to_string(seq));
        }
        changes.push_back(std::move(c));
    }
    std::string client = r.str();
    std::uint64_t num = r.u64();
    if (!r.ok() || !r.done()) {
        return unreadable(dir_, "record " + std::to_string(seq));
    }
    if (format == 1) {
        // A friend tab's record: its tab is a session, made (as the next) if no snapshot or record named it before.
        legacy_read_ = true;
        if (!changes.empty() && tab.empty()) {
            return unreadable(dir_, "record " + std::to_string(seq));
        }
        if (!changes.empty()) {
            session = legacy_session(tab, true);
        }
    }
    if (!refusal(session, changes).empty()) {
        return unreadable(dir_, "record " + std::to_string(seq));
    }
    apply(session, changes, client, num);
    return Status::ok();
}

Bytes Store::encode_snapshot() const {
    Writer w;
    w.u8(kSnapshotFormat);
    w.u64(next_session_);
    w.u32(std::uint32_t(sessions_.size()));
    for (const auto& [id, t] : sessions_) {
        w.u64(id);
        put_info(w, t.info);
        w.u32(std::uint32_t(t.transcript.size()));
        for (const Entry& e : t.transcript) {
            put_entry(w, e);
        }
        w.u32(std::uint32_t(t.queue.size()));
        for (const std::string& q : t.queue) {
            w.str(q);
        }
        put_driver(w, t.driver);
    }
    w.u32(std::uint32_t(applied_.size()));
    for (const auto& [client, num] : applied_) {
        w.str(client);
        w.u64(num);
    }
    return std::move(w.out());
}

Status Store::replay_snapshot(std::span<const std::uint8_t> snap) {
    Reader r(snap);
    const std::uint8_t format = r.u8();
    if (format != 1 && format != 2 && format != kSnapshotNoInstance && format != kSnapshotFormat) {
        return unreadable(dir_, "the snapshot");
    }
    if (format >= kSnapshotNoInstance) {
        next_session_ = r.u64();
        if (next_session_ == 0) {
            return unreadable(dir_, "the snapshot");
        }
    } else {
        legacy_read_ = true;
    }
    std::uint32_t count = r.u32();
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        std::uint64_t id = 0;
        Info info;
        if (format >= kSnapshotNoInstance) {
            id = r.u64();
            get_info(r, info, format == kSnapshotFormat);
            if (id == 0 || id >= next_session_ || sessions_.count(id) != 0) {
                return unreadable(dir_, "the snapshot");
            }
        } else {
            const std::string tab = r.str();
            if (legacy_.count(tab) != 0) {
                return unreadable(dir_, "the snapshot");
            }
            id = legacy_session(tab, true);     // in the order written: by name, so ids 1, 2, ... in name order
            info = sessions_[id].info;
        }
        Session& t = sessions_[id];
        t.info = std::move(info);
        std::uint32_t entries = r.u32();
        for (std::uint32_t k = 0; k < entries && r.ok(); ++k) {
            Entry e;
            if (!get_entry(r, e)) {
                return unreadable(dir_, "the snapshot");
            }
            t.transcript.push_back(std::move(e));
        }
        std::uint32_t prompts = r.u32();
        for (std::uint32_t k = 0; k < prompts && r.ok(); ++k) {
            t.queue.push_back(r.str());
        }
        if (format != 1) {
            get_driver(r, t.driver);
        }
    }
    std::uint32_t clients = r.u32();
    for (std::uint32_t i = 0; i < clients && r.ok(); ++i) {
        std::string c = r.str();
        applied_[c] = r.u64();
    }
    if (!r.ok() || !r.done()) {
        return unreadable(dir_, "the snapshot");
    }
    return Status::ok();
}

} // namespace nexus::store
