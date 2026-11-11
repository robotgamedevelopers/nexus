// The server's durable state (store.h): the tabs and each client's applied counter, over one journal.

#include "store/store.h"

namespace nexus::store {

namespace {

constexpr std::uint8_t kRecordFormat = 1;
constexpr std::uint8_t kSnapshotFormat = 2;   // 1: before drivers, still read

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
    for (const std::string& p : d.waiting)
        w.str(p);
}

void get_driver(Reader& r, Driver& d) {
    d.next_turn = r.u64();
    d.open_turn = r.u64();
    d.session = r.str();
    d.waiting.clear();
    std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i)
        d.waiting.push_back(r.str());
}

Bytes encode_record(const std::string& tab, std::span<const Change> changes, const std::string& client,
                    std::uint64_t num) {
    Writer w;
    w.u8(kRecordFormat);
    w.str(tab);
    w.u32(std::uint32_t(changes.size()));
    for (const Change& c : changes) {
        w.u8(std::uint8_t(c.kind));
        if (c.kind == Change::Kind::Append)
            put_entry(w, c.entry);
        else if (c.kind == Change::Kind::Enqueue)
            w.str(c.prompt);
        else
            put_driver(w, c.driver);
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
    if (!st.is_ok())
        return st;
    if (rec.snapshot_seq > 0 && !(st = s->replay_snapshot(rec.snapshot)).is_ok())
        return st;
    for (std::size_t i = 0; i < rec.records.size(); ++i) {
        if (!(st = s->replay_record(rec.records[i], rec.snapshot_seq + i + 1)).is_ok())
            return st;
    }
    if (report != nullptr) {
        report->fresh = s->journal_.last_seq() == 0;
        report->records = rec.records.size();
        report->cut_bytes = rec.cut_bytes;
    }
    out = std::move(s);
    return Status::ok();
}

const Tab* Store::tab(const std::string& name) const {
    auto it = tabs_.find(name);
    return it == tabs_.end() ? nullptr : &it->second;
}

std::uint64_t Store::applied(const std::string& client) const {
    auto it = applied_.find(client);
    return it == applied_.end() ? 0 : it->second;
}

void Store::apply(const std::string& tab, std::span<const Change> changes, const std::string& client,
                  std::uint64_t num) {
    if (!changes.empty()) {
        Tab& t = tabs_[tab];
        for (const Change& c : changes) {
            if (c.kind == Change::Kind::Append)
                t.transcript.push_back(c.entry);
            else if (c.kind == Change::Kind::Enqueue)
                t.queue.push_back(c.prompt);
            else
                t.driver = c.driver;
        }
    }
    if (!client.empty())
        applied_[client] = num;
}

Status Store::commit(const std::string& tab, std::span<const Change> changes, const std::string& client,
                     std::uint64_t num) {
    if (failed_ || journal_.failed())
        return Status::failed("store " + dir_ + ": an earlier write or sync failed, so what is on disk is unknown; "
                              "restart the server to reopen the store from what is durable");
    if (!changes.empty() && tab.empty())
        return Status::refused("store " + dir_ + ": a change names no tab");
    if (changes.empty() && client.empty())
        return Status::refused("store " + dir_ + ": a step with no change and no client changes nothing");
    if (!client.empty() && num != applied(client) + 1)
        return Status::refused("store " + dir_ + ": client " + client + " has applied up to " +
                               std::to_string(applied(client)) + ", so its next action is " +
                               std::to_string(applied(client) + 1) + ", not " + std::to_string(num));
    Status s = journal_.append(encode_record(tab, changes, client, num));
    if (s.is_ok())
        s = journal_.sync();
    if (!s.is_ok()) {
        failed_ = true;
        return s;
    }
    apply(tab, changes, client, num);
    // The step is durable whatever compaction does: a failure here only stops later steps.
    if (journal_.log_bytes() > options_.compact_bytes && !(s = journal_.compact(encode_snapshot())).is_ok())
        failed_ = true;
    return Status::ok();
}

Status Store::replay_record(std::span<const std::uint8_t> rec, std::uint64_t seq) {
    Reader r(rec);
    if (r.u8() != kRecordFormat)
        return unreadable(dir_, "record " + std::to_string(seq));
    std::string tab = r.str();
    std::uint32_t n = r.u32();
    std::vector<Change> changes;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        Change c;
        std::uint8_t kind = r.u8();
        if (kind == std::uint8_t(Change::Kind::Append)) {
            c.kind = Change::Kind::Append;
            if (!get_entry(r, c.entry))
                return unreadable(dir_, "record " + std::to_string(seq));
        } else if (kind == std::uint8_t(Change::Kind::Enqueue)) {
            c.kind = Change::Kind::Enqueue;
            c.prompt = r.str();
        } else if (kind == std::uint8_t(Change::Kind::SetDriver)) {
            c.kind = Change::Kind::SetDriver;
            get_driver(r, c.driver);
        } else {
            return unreadable(dir_, "record " + std::to_string(seq));
        }
        changes.push_back(std::move(c));
    }
    std::string client = r.str();
    std::uint64_t num = r.u64();
    if (!r.ok() || !r.done())
        return unreadable(dir_, "record " + std::to_string(seq));
    apply(tab, changes, client, num);
    return Status::ok();
}

Bytes Store::encode_snapshot() const {
    Writer w;
    w.u8(kSnapshotFormat);
    w.u32(std::uint32_t(tabs_.size()));
    for (const auto& [name, t] : tabs_) {
        w.str(name);
        w.u32(std::uint32_t(t.transcript.size()));
        for (const Entry& e : t.transcript)
            put_entry(w, e);
        w.u32(std::uint32_t(t.queue.size()));
        for (const std::string& q : t.queue)
            w.str(q);
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
    if (format != 1 && format != kSnapshotFormat)
        return unreadable(dir_, "the snapshot");
    std::uint32_t tabs = r.u32();
    for (std::uint32_t i = 0; i < tabs && r.ok(); ++i) {
        Tab& t = tabs_[r.str()];
        std::uint32_t entries = r.u32();
        for (std::uint32_t k = 0; k < entries && r.ok(); ++k) {
            Entry e;
            if (!get_entry(r, e))
                return unreadable(dir_, "the snapshot");
            t.transcript.push_back(std::move(e));
        }
        std::uint32_t prompts = r.u32();
        for (std::uint32_t k = 0; k < prompts && r.ok(); ++k)
            t.queue.push_back(r.str());
        if (format == kSnapshotFormat)
            get_driver(r, t.driver);
    }
    std::uint32_t clients = r.u32();
    for (std::uint32_t i = 0; i < clients && r.ok(); ++i) {
        std::string c = r.str();
        applied_[c] = r.u64();
    }
    if (!r.ok() || !r.done())
        return unreadable(dir_, "the snapshot");
    return Status::ok();
}

} // namespace nexus::store
