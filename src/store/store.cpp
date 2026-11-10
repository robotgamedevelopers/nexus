// The server's durable state (store.h): tabs, the per-client applied counter and epoch fence, blobs, and the
// incarnation and generation, over two journals.

#include "store/store.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nexus::store {

// One fact of a record. A store record is the facts of one durable step: a change of several tabs, an Act's
// actions with the new applied number and the placements they make, a re-base, a blob's offer or new attempt, a
// placement done, a new generation. A server record holds Inc, MetaGen, Mark and Seen facts.
struct Store::Fact {
    enum class Type : std::uint8_t {
        TabOp = 1,   // tab, kind, text, blob
        Applied = 2, // client, a = the highest action number applied
        Rebase = 3,  // client, a..b skipped (lost with a replaced store); applied = b
        Offer = 4,   // blob, a = size
        Retry = 5,   // blob, a = the new attempt (the last one failed its hash check)
        Place = 6,   // blob, dir, name: place it, once the record is durable
        Placed = 7,  // blob, dir, name: done
        Gen = 8,     // a = the generation the store is in from here on
        Inc = 20,    // server record: a = incarnation
        MetaGen = 21,
        Mark = 22,   // a = the store's last record number, durable and acknowledged
        Seen = 23,   // client, a = the highest epoch welcomed
    };
    Type type = Type::TabOp;
    TabId tab = 0;
    OpKind kind = OpKind::New;
    std::string text;
    std::optional<Hash> blob;
    ClientId client{};
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::string dir;
    std::string name;
};

namespace {

using Fact = Store::Fact;

void put_hash(Writer& w, const Hash& h) {
    w.raw(std::span<const std::uint8_t>(h.data(), h.size()));
}

Hash get_hash(Reader& r) {
    Hash h{};
    r.raw(h.data(), h.size());
    return h;
}

void put_opt_hash(Writer& w, const std::optional<Hash>& h) {
    w.u8(h ? 1 : 0);
    if (h)
        put_hash(w, *h);
}

std::optional<Hash> get_opt_hash(Reader& r) {
    if (r.u8() == 0)
        return std::nullopt;
    return get_hash(r);
}

void put_client(Writer& w, const ClientId& c) {
    w.raw(std::span<const std::uint8_t>(c.data(), c.size()));
}

ClientId get_client(Reader& r) {
    ClientId c{};
    r.raw(c.data(), c.size());
    return c;
}

void put_item(Writer& w, const Item& it) {
    w.u64(it.id);
    w.u64(it.gen);
    w.u8(std::uint8_t(it.author));
    w.u8(it.open ? 1 : 0);
    w.str(it.text);
    put_opt_hash(w, it.blob);
}

Item get_item(Reader& r) {
    Item it;
    it.id = r.u64();
    it.gen = r.u64();
    it.author = Author(r.u8());
    it.open = r.u8() != 0;
    it.text = r.str();
    it.blob = get_opt_hash(r);
    return it;
}

void put_op(Writer& w, const TabOp& op) {
    w.u8(std::uint8_t(op.kind));
    w.u64(op.id);
    w.u64(op.gen);
    w.u8(std::uint8_t(op.author));
    w.str(op.text);
    put_opt_hash(w, op.blob);
}

TabOp get_op(Reader& r) {
    TabOp op;
    op.kind = OpKind(r.u8());
    op.id = r.u64();
    op.gen = r.u64();
    op.author = Author(r.u8());
    op.text = r.str();
    op.blob = get_opt_hash(r);
    return op;
}

Bytes encode_facts(const std::vector<Fact>& facts) {
    Writer w;
    w.u32(std::uint32_t(facts.size()));
    for (const Fact& f : facts) {
        w.u8(std::uint8_t(f.type));
        switch (f.type) {
        case Fact::Type::TabOp:
            w.u64(f.tab);
            w.u8(std::uint8_t(f.kind));
            w.str(f.text);
            put_opt_hash(w, f.blob);
            break;
        case Fact::Type::Applied:
        case Fact::Type::Seen:
            put_client(w, f.client);
            w.u64(f.a);
            break;
        case Fact::Type::Rebase:
            put_client(w, f.client);
            w.u64(f.a);
            w.u64(f.b);
            break;
        case Fact::Type::Offer:
        case Fact::Type::Retry:
            put_hash(w, *f.blob);
            w.u64(f.a);
            break;
        case Fact::Type::Place:
        case Fact::Type::Placed:
            put_hash(w, *f.blob);
            w.str(f.dir);
            w.str(f.name);
            break;
        case Fact::Type::Gen:
        case Fact::Type::Inc:
        case Fact::Type::MetaGen:
        case Fact::Type::Mark:
            w.u64(f.a);
            break;
        }
    }
    return std::move(w.out());
}

bool decode_facts(std::span<const std::uint8_t> in, std::vector<Fact>& out) {
    out.clear();
    Reader r(in);
    std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        Fact f;
        f.type = Fact::Type(r.u8());
        switch (f.type) {
        case Fact::Type::TabOp: {
            f.tab = r.u64();
            std::uint8_t k = r.u8();
            if (k < std::uint8_t(OpKind::New) || k > std::uint8_t(OpKind::Deliver))
                return false;
            f.kind = OpKind(k);
            f.text = r.str();
            f.blob = get_opt_hash(r);
            break;
        }
        case Fact::Type::Applied:
        case Fact::Type::Seen:
            f.client = get_client(r);
            f.a = r.u64();
            break;
        case Fact::Type::Rebase:
            f.client = get_client(r);
            f.a = r.u64();
            f.b = r.u64();
            break;
        case Fact::Type::Offer:
        case Fact::Type::Retry:
            f.blob = get_hash(r);
            f.a = r.u64();
            break;
        case Fact::Type::Place:
        case Fact::Type::Placed:
            f.blob = get_hash(r);
            f.dir = r.str();
            f.name = r.str();
            break;
        case Fact::Type::Gen:
        case Fact::Type::Inc:
        case Fact::Type::MetaGen:
        case Fact::Type::Mark:
            f.a = r.u64();
            break;
        default:
            return false;
        }
        out.push_back(std::move(f));
    }
    return r.ok() && r.done();
}

bool is_meta(Fact::Type t) {
    return t == Fact::Type::Inc || t == Fact::Type::MetaGen || t == Fact::Type::Mark || t == Fact::Type::Seen;
}

void close_last(std::vector<Item>& tr) {
    if (!tr.empty())
        tr.back().open = false;
}

bool parse_hex_hash(const std::string& s, Hash& out) {
    if (s.size() != 64)
        return false;
    for (std::size_t i = 0; i < 32; ++i) {
        int v = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0)
                return false;
            v = v * 16 + d;
        }
        out[i] = std::uint8_t(v);
    }
    return true;
}

std::string tab_name(TabId t) {
    return "tab " + std::to_string(t);
}

} // namespace

std::string sanitise_file_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '-' || c == '_';
        out.push_back(keep ? c : '_');
    }
    while (!out.empty() && out[0] == '.')
        out.erase(0, 1);
    if (out.size() > 128)
        out.resize(128);
    if (out.empty())
        out = "file";
    return out;
}

Store::Store(Fs& fs, std::string root, const StoreOptions& options)
    : fs_(fs), root_(std::move(root)), options_(options), store_(fs, root_ + "/store"), meta_(fs, root_ + "/server") {
    if (!options_.incoming) {
        std::string r = root_;
        options_.incoming = [r](TabId t) { return r + "/incoming/" + std::to_string(t); };
    }
    if (options_.recent_ops == 0)
        options_.recent_ops = 1;
}

Status Store::refuse_if_failed() const {
    if (failed_)
        return Status::failed("store " + root_ + ": an earlier write or sync failed, so what is on disk is unknown; "
                              "restart the server to reopen the store from what is durable");
    return Status::ok();
}

std::string Store::blob_path(const Hash& h) const {
    return root_ + "/store/blobs/" + to_hex(h);
}

std::string Store::part_path(const Hash& h, std::uint64_t attempt) const {
    return root_ + "/store/blobs/" + to_hex(h) + "." + std::to_string(attempt) + ".part";
}

// ---- applying facts ----

void Store::apply_op(TabId tab, OpKind kind, const std::string& text, const std::optional<Hash>& blob) {
    Tab& t = tabs_[tab];
    std::uint64_t v = t.version + 1;
    TabOp op;
    op.kind = kind;
    op.text = text;
    op.blob = blob;
    switch (kind) {
    case OpKind::New:
    case OpKind::Send: {
        close_last(t.transcript);
        Item it;
        it.id = v;
        it.gen = store_gen_;
        it.author = kind == OpKind::New ? Author::Friend : Author::Person;
        it.open = kind == OpKind::New;
        it.text = text;
        it.blob = blob;
        t.where[v] = t.transcript.size();
        t.transcript.push_back(std::move(it));
        op.id = v;
        op.gen = store_gen_;
        op.author = t.transcript.back().author;
        break;
    }
    case OpKind::Grow:
        if (!t.transcript.empty() && t.transcript.back().open)
            t.transcript.back().text += text;
        op.author = Author::Friend;
        break;
    case OpKind::Close:
        close_last(t.transcript);
        break;
    case OpKind::Enq: {
        Item it;
        it.id = v;
        it.gen = store_gen_;
        it.author = Author::Person;
        it.text = text;
        it.blob = blob;
        t.queue.push_back(std::move(it));
        op.id = v;
        op.gen = store_gen_;
        break;
    }
    case OpKind::Deliver:
        if (!t.queue.empty()) {
            close_last(t.transcript);
            Item it = std::move(t.queue.front());
            t.queue.erase(t.queue.begin());
            it.open = false;
            op.id = it.id;
            op.gen = it.gen;
            t.where[it.id] = t.transcript.size();
            t.transcript.push_back(std::move(it));
        }
        op.text.clear();
        break;
    }
    t.version = v;
    t.recent.push_back(std::move(op));
    if (t.recent.size() > options_.recent_ops)
        t.recent.pop_front();
}

void Store::apply_fact(const Fact& f) {
    switch (f.type) {
    case Fact::Type::TabOp:
        apply_op(f.tab, f.kind, f.text, f.blob);
        break;
    case Fact::Type::Applied:
        clients_[f.client].applied = f.a;
        break;
    case Fact::Type::Rebase: {
        Client& c = clients_[f.client];
        c.skipped.emplace_back(f.a, f.b);
        c.applied = f.b;
        break;
    }
    case Fact::Type::Offer:
        blobs_[*f.blob] = Blob{f.a, 0, 0, false};
        break;
    case Fact::Type::Retry: {
        Blob& b = blobs_[*f.blob];
        b.attempt = f.a;
        b.held = 0;
        break;
    }
    case Fact::Type::Place:
        placements_.push_back(Placement{*f.blob, f.dir, f.name});
        break;
    case Fact::Type::Placed: {
        Placement p{*f.blob, f.dir, f.name};
        auto it = std::find(placements_.begin(), placements_.end(), p);
        if (it != placements_.end())
            placements_.erase(it);
        break;
    }
    case Fact::Type::Gen:
        store_gen_ = f.a;
        break;
    case Fact::Type::Inc:
        inc_ = f.a;
        break;
    case Fact::Type::MetaGen:
        gen_ = f.a;
        break;
    case Fact::Type::Mark:
        mark_ = f.a;
        break;
    case Fact::Type::Seen:
        seen_[f.client] = f.a;
        break;
    }
}

// ---- commits ----

Status Store::commit(const std::vector<Fact>& facts) {
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    Bytes rec = encode_facts(facts);
    if (!(s = store_.append(rec)).is_ok() || !(s = store_.sync()).is_ok()) {
        failed_ = true;
        return s;
    }
    for (const Fact& f : facts)
        apply_fact(f);
    // The high-water mark, outside the store, before anything is acknowledged: a store copied in from a backup taken
    // before this record ends below it, and open() sees that.
    Fact mark;
    mark.type = Fact::Type::Mark;
    mark.a = store_.last_seq();
    if (!(s = meta_commit({mark})).is_ok())
        return s;
    if (store_.log_bytes() > options_.compact_bytes && !(s = store_.compact(encode_store())).is_ok()) {
        failed_ = true;
        return s;
    }
    return Status::ok();
}

Status Store::meta_commit(const std::vector<Fact>& facts) {
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    if (!(s = meta_.append(encode_facts(facts))).is_ok() || !(s = meta_.sync()).is_ok()) {
        failed_ = true;
        return s;
    }
    for (const Fact& f : facts)
        apply_fact(f);
    if (meta_.log_bytes() > options_.meta_compact_bytes && !(s = meta_.compact(encode_meta())).is_ok()) {
        failed_ = true;
        return s;
    }
    return Status::ok();
}

// ---- snapshots ----

Bytes Store::encode_store() const {
    Writer w;
    w.u64(store_gen_);
    w.u32(std::uint32_t(tabs_.size()));
    for (const auto& [id, t] : tabs_) {
        w.u64(id);
        w.u64(t.version);
        w.u32(std::uint32_t(t.transcript.size()));
        for (const Item& it : t.transcript)
            put_item(w, it);
        w.u32(std::uint32_t(t.queue.size()));
        for (const Item& it : t.queue)
            put_item(w, it);
        w.u32(std::uint32_t(t.recent.size()));
        for (const TabOp& op : t.recent)
            put_op(w, op);
    }
    w.u32(std::uint32_t(clients_.size()));
    for (const auto& [id, c] : clients_) {
        put_client(w, id);
        w.u64(c.applied);
        w.u32(std::uint32_t(c.skipped.size()));
        for (const auto& [from, to] : c.skipped) {
            w.u64(from);
            w.u64(to);
        }
    }
    w.u32(std::uint32_t(blobs_.size()));
    for (const auto& [h, b] : blobs_) {
        put_hash(w, h);
        w.u64(b.size);
        w.u64(b.attempt);
    }
    w.u32(std::uint32_t(placements_.size()));
    for (const Placement& p : placements_) {
        put_hash(w, p.blob);
        w.str(p.dir);
        w.str(p.name);
    }
    return std::move(w.out());
}

Bytes Store::encode_meta() const {
    Writer w;
    w.u64(inc_);
    w.u64(gen_);
    w.u64(mark_);
    w.u32(std::uint32_t(seen_.size()));
    for (const auto& [c, ep] : seen_) {
        put_client(w, c);
        w.u64(ep);
    }
    return std::move(w.out());
}

Status Store::replay_store(const Journal::Recovery& r) {
    if (!r.snapshot.empty()) {
        Reader rd(r.snapshot);
        store_gen_ = rd.u64();
        std::uint32_t nt = rd.u32();
        for (std::uint32_t i = 0; i < nt && rd.ok(); ++i) {
            TabId id = rd.u64();
            Tab& t = tabs_[id];
            t.version = rd.u64();
            std::uint32_t n = rd.u32();
            for (std::uint32_t k = 0; k < n && rd.ok(); ++k) {
                t.transcript.push_back(get_item(rd));
                t.where[t.transcript.back().id] = t.transcript.size() - 1;
            }
            n = rd.u32();
            for (std::uint32_t k = 0; k < n && rd.ok(); ++k)
                t.queue.push_back(get_item(rd));
            n = rd.u32();
            for (std::uint32_t k = 0; k < n && rd.ok(); ++k)
                t.recent.push_back(get_op(rd));
        }
        std::uint32_t nc = rd.u32();
        for (std::uint32_t i = 0; i < nc && rd.ok(); ++i) {
            Client& c = clients_[get_client(rd)];
            c.applied = rd.u64();
            std::uint32_t n = rd.u32();
            for (std::uint32_t k = 0; k < n && rd.ok(); ++k) {
                std::uint64_t from = rd.u64();
                c.skipped.emplace_back(from, rd.u64());
            }
        }
        std::uint32_t nb = rd.u32();
        for (std::uint32_t i = 0; i < nb && rd.ok(); ++i) {
            Hash h = get_hash(rd);
            Blob& b = blobs_[h];
            b.size = rd.u64();
            b.attempt = rd.u64();
        }
        std::uint32_t np = rd.u32();
        for (std::uint32_t i = 0; i < np && rd.ok(); ++i) {
            Placement p;
            p.blob = get_hash(rd);
            p.dir = rd.str();
            p.name = rd.str();
            placements_.push_back(std::move(p));
        }
        if (!rd.ok() || !rd.done())
            return Status::failed("store " + root_ + ": its snapshot does not decode (written by another version of "
                                  "nexus-server?); restore the store directory from a backup");
    }
    std::vector<Fact> facts;
    for (std::size_t i = 0; i < r.records.size(); ++i) {
        if (!decode_facts(r.records[i], facts))
            return Status::failed("store " + root_ + ": record " + std::to_string(r.snapshot_seq + i + 1) +
                                  " does not decode (written by another version of nexus-server?); restore the "
                                  "store directory from a backup");
        for (const Fact& f : facts) {
            if (is_meta(f.type))
                return Status::failed("store " + root_ + ": record " + std::to_string(r.snapshot_seq + i + 1) +
                                      " holds a server fact; is server/ copied into store/?");
            apply_fact(f);
        }
    }
    return Status::ok();
}

Status Store::replay_meta(const Journal::Recovery& r, bool& any) {
    any = !r.snapshot.empty() || !r.records.empty() || r.snapshot_seq > 0;
    if (!r.snapshot.empty()) {
        Reader rd(r.snapshot);
        inc_ = rd.u64();
        gen_ = rd.u64();
        mark_ = rd.u64();
        std::uint32_t n = rd.u32();
        for (std::uint32_t i = 0; i < n && rd.ok(); ++i) {
            ClientId c = get_client(rd);
            seen_[c] = rd.u64();
        }
        if (!rd.ok() || !rd.done())
            return Status::failed("server record " + root_ + "/server: its snapshot does not decode; move the "
                                  "directory aside to start a new incarnation and generation");
    }
    std::vector<Fact> facts;
    for (std::size_t i = 0; i < r.records.size(); ++i) {
        if (!decode_facts(r.records[i], facts))
            return Status::failed("server record " + root_ + "/server: record " +
                                  std::to_string(r.snapshot_seq + i + 1) + " does not decode; move the directory "
                                  "aside to start a new incarnation and generation");
        for (const Fact& f : facts) {
            if (!is_meta(f.type))
                return Status::failed("server record " + root_ + "/server: record " +
                                      std::to_string(r.snapshot_seq + i + 1) + " holds a store fact; is store/ "
                                      "copied into server/?");
            apply_fact(f);
        }
    }
    return Status::ok();
}

// ---- open ----

Status Store::open(Fs& fs, const std::string& root, std::uint64_t now_secs, const StoreOptions& options,
                   std::unique_ptr<Store>& out, OpenReport* report) {
    out.reset();
    std::unique_ptr<Store> s(new Store(fs, root, options));
    OpenReport rep;
    Journal::Recovery mr;
    Status st = s->meta_.open(mr);
    if (!st.is_ok())
        return st;
    bool any = false;
    if (!(st = s->replay_meta(mr, any)).is_ok())
        return st;
    Journal::Recovery sr;
    if (!(st = s->store_.open(sr)).is_ok())
        return st;
    if (!(st = s->replay_store(sr)).is_ok())
        return st;
    rep.meta_cut = mr.cut_bytes;
    rep.store_cut = sr.cut_bytes;

    // The incarnation goes forward, even past a server record restored with the disk. The store is the one the
    // server record describes when it is in that record's generation and reaches its mark; anything else is a store
    // replaced by another copy, and the generation moves to the new incarnation.
    std::uint64_t last = s->store_.last_seq();
    rep.fresh = !any && last == 0;
    std::uint64_t inc = std::max(s->inc_ + 1, now_secs);
    rep.restored = !rep.fresh && (!any || s->store_gen_ != s->gen_ || last < s->mark_);
    std::uint64_t gen = rep.fresh || rep.restored ? inc : s->gen_;
    std::vector<Fact> facts(3);
    facts[0].type = Fact::Type::Inc;
    facts[0].a = inc;
    facts[1].type = Fact::Type::MetaGen;
    facts[1].a = gen;
    facts[2].type = Fact::Type::Mark;
    facts[2].a = last;
    if (!(st = s->meta_commit(facts)).is_ok())
        return st;
    if (s->store_gen_ != gen) {
        Fact g;
        g.type = Fact::Type::Gen;
        g.a = gen;
        if (!(st = s->commit({g})).is_ok())
            return st;
    }
    if (!(st = s->recover_blobs()).is_ok())
        return st;
    std::vector<Placement> todo = s->placements_;
    for (const Placement& p : todo) {
        if (!(st = s->place(p)).is_ok())
            return st;
        ++rep.placements_redone;
    }
    if (!todo.empty() && !(st = s->store_.sync()).is_ok()) {
        s->failed_ = true;
        return st;
    }
    if (report != nullptr)
        *report = rep;
    out = std::move(s);
    return Status::ok();
}

Status Store::recover_blobs() {
    std::string dir = root_ + "/store/blobs";
    Status s = fs_.make_dir(dir);
    if (!s.is_ok())
        return s;
    std::vector<std::string> names;
    if (!(s = fs_.list(dir, names)).is_ok())
        return s;
    // Complete blobs first, so that a part of one is known to be left over.
    for (const std::string& n : names) {
        Hash h{};
        if (!parse_hex_hash(n, h))
            continue;
        auto b = blobs_.find(h);
        if (b != blobs_.end()) {
            b->second.complete = true;
            b->second.held = b->second.size;
        }
    }
    for (const std::string& n : names) {
        Hash h{};
        bool part = n.size() > 64 + 6 && n.compare(n.size() - 5, 5, ".part") == 0 && n[64] == '.' &&
                    parse_hex_hash(n.substr(0, 64), h);
        if (!part)
            continue;  // not the store's: leave it
        std::uint64_t attempt = std::strtoull(n.substr(65, n.size() - 65 - 5).c_str(), nullptr, 10);
        auto b = blobs_.find(h);
        if (b != blobs_.end() && !b->second.complete && attempt == b->second.attempt) {
            std::uint64_t size = 0;
            if (!(s = fs_.size(dir + "/" + n, size)).is_ok())
                return s;
            b->second.held = std::min(size, b->second.size);
        } else if (!(s = fs_.remove(dir + "/" + n)).is_ok()) {
            return s;  // an attempt that failed its hash check, or a blob already complete
        }
    }
    return Status::ok();
}

// ---- tabs ----

Status Store::change(std::span<const Change> changes) {
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    struct Sim {
        bool open = false;
        std::size_t queue = 0;
    };
    std::map<TabId, Sim> sim;
    std::vector<Fact> facts;
    for (const Change& c : changes) {
        auto at = sim.find(c.tab);
        if (at == sim.end()) {
            Sim fresh;
            auto t = tabs_.find(c.tab);
            if (t != tabs_.end()) {
                fresh.open = !t->second.transcript.empty() && t->second.transcript.back().open;
                fresh.queue = t->second.queue.size();
            }
            at = sim.emplace(c.tab, fresh).first;
        }
        Sim& m = at->second;
        switch (c.kind) {
        case OpKind::New:
            m.open = true;
            break;
        case OpKind::Grow:
        case OpKind::Close:
            if (!m.open)
                return Status::refused(tab_name(c.tab) + ": no open entry to " +
                                       (c.kind == OpKind::Grow ? "grow" : "close") + " (open one first)");
            m.open = c.kind == OpKind::Grow;
            break;
        case OpKind::Send:
            m.open = false;
            break;
        case OpKind::Enq:
            if (m.queue >= kQueueCap)
                return Status::refused(tab_name(c.tab) + ": the queue holds " + std::to_string(kQueueCap) +
                                       " prompts, the most it can; wait for one to be delivered before queueing more");
            ++m.queue;
            break;
        case OpKind::Deliver:
            if (m.queue == 0)
                return Status::refused(tab_name(c.tab) + ": nothing is queued to deliver");
            --m.queue;
            m.open = false;
            break;
        }
        Fact f;
        f.type = Fact::Type::TabOp;
        f.tab = c.tab;
        f.kind = c.kind;
        f.text = c.text;
        f.blob = c.blob;
        facts.push_back(std::move(f));
    }
    if (facts.empty())
        return Status::ok();
    return commit(facts);
}

std::vector<TabId> Store::tabs() const {
    std::vector<TabId> out;
    for (const auto& [id, t] : tabs_)
        out.push_back(id);
    return out;
}

std::uint64_t Store::version(TabId tab) const {
    auto t = tabs_.find(tab);
    return t == tabs_.end() ? 0 : t->second.version;
}

Snap Store::snapshot(TabId tab, std::size_t window) const {
    Snap out;
    auto t = tabs_.find(tab);
    if (t == tabs_.end())
        return out;
    const Tab& tb = t->second;
    out.version = tb.version;
    std::size_t n = tb.transcript.size();
    std::size_t first = n > window ? n - window : 0;  // TailLo - 1
    out.lo = first + 1;
    out.entries.assign(tb.transcript.begin() + std::ptrdiff_t(first), tb.transcript.end());
    out.queue = tb.queue;
    return out;
}

bool Store::diff(TabId tab, std::uint64_t base, std::size_t max_ops, std::vector<TabOp>& out) const {
    out.clear();
    auto t = tabs_.find(tab);
    std::uint64_t v = t == tabs_.end() ? 0 : t->second.version;
    if (base > v)
        return false;
    if (base == v)
        return true;
    const Tab& tb = t->second;
    std::uint64_t oldest = v - tb.recent.size();  // the version before the oldest operation kept
    if (base < oldest)
        return false;
    std::uint64_t from = base - oldest;  // index of the operation that made version base + 1
    std::uint64_t to = std::min<std::uint64_t>(tb.recent.size(), from + max_ops);
    out.assign(tb.recent.begin() + std::ptrdiff_t(from), tb.recent.begin() + std::ptrdiff_t(to));
    return true;
}

std::vector<Item> Store::older(TabId tab, std::uint64_t lo, std::size_t page) const {
    auto t = tabs_.find(tab);
    if (t == tabs_.end() || lo <= 1 || lo > t->second.transcript.size() + 1)
        return {};
    std::uint64_t k = std::min<std::uint64_t>(page, lo - 1);
    const auto& tr = t->second.transcript;
    return std::vector<Item>(tr.begin() + std::ptrdiff_t(lo - 1 - k), tr.begin() + std::ptrdiff_t(lo - 1));
}

const Item* Store::item(TabId tab, std::uint64_t id) const {
    auto t = tabs_.find(tab);
    if (t == tabs_.end())
        return nullptr;
    auto w = t->second.where.find(id);
    if (w != t->second.where.end())
        return &t->second.transcript[w->second];
    for (const Item& it : t->second.queue) {
        if (it.id == id)
            return &it;
    }
    return nullptr;
}

bool Store::content(TabId tab, std::uint64_t id, std::uint64_t off, std::size_t max, std::string& out) const {
    out.clear();
    const Item* it = item(tab, id);
    if (it == nullptr || off >= it->text.size() || max == 0)
        return false;
    std::size_t end = std::size_t(std::min<std::uint64_t>(it->text.size(), off + max));
    // Back up over UTF-8 continuation bytes (10xxxxxx) so no code point is cut, unless that leaves nothing.
    std::size_t cut = end;
    while (cut > off && cut < it->text.size() && (std::uint8_t(it->text[cut]) & 0xC0) == 0x80)
        --cut;
    if (cut > off)
        end = cut;
    out = it->text.substr(std::size_t(off), end - std::size_t(off));
    return true;
}

// ---- session ----

std::uint64_t Store::applied(const ClientId& c) const {
    auto it = clients_.find(c);
    return it == clients_.end() ? 0 : it->second.applied;
}

std::uint64_t Store::seen(const ClientId& c) const {
    auto it = seen_.find(c);
    return it == seen_.end() ? 0 : it->second;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Store::skipped(const ClientId& c) const {
    auto it = clients_.find(c);
    return it == clients_.end() ? std::vector<std::pair<std::uint64_t, std::uint64_t>>{} : it->second.skipped;
}

Status Store::hello(const ClientId& c, std::uint64_t ep, std::uint64_t sgen, std::uint64_t first, HelloResult& out) {
    out = HelloResult{};
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    out.inc = inc_;
    out.gen = gen_;
    if (ep < seen(c)) {
        out.superseded = true;
        out.applied = applied(c);
        return Status::ok();
    }
    // Re-base: a client from another generation whose first held number is beyond what this store applied. The
    // numbers between were acked by a store that is gone.
    std::uint64_t a = applied(c);
    if (sgen != gen_ && first > 0 && first - 1 > a) {
        Fact f;
        f.type = Fact::Type::Rebase;
        f.client = c;
        f.a = a + 1;
        f.b = first - 1;
        if (!(s = commit({f})).is_ok())
            return s;
    }
    if (ep != seen(c)) {
        Fact f;
        f.type = Fact::Type::Seen;
        f.client = c;
        f.a = ep;
        if (!(s = meta_commit({f})).is_ok())
            return s;
    }
    out.applied = applied(c);
    return Status::ok();
}

std::string Store::unique_name(const std::string& dir, const std::string& want, const std::vector<Placement>& taken) {
    std::string base = sanitise_file_name(want);
    std::vector<std::string> names;
    if (!fs_.list(dir, names).is_ok())
        names.clear();
    auto used = [&](const std::string& n) {
        if (std::find(names.begin(), names.end(), n) != names.end())
            return true;
        for (const Placement& p : placements_) {
            if (p.dir == dir && p.name == n)
                return true;
        }
        for (const Placement& p : taken) {
            if (p.dir == dir && p.name == n)
                return true;
        }
        return false;
    };
    if (!used(base))
        return base;
    std::size_t dot = base.rfind('.');
    std::string stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
    std::string ext = dot == std::string::npos || dot == 0 ? std::string() : base.substr(dot);
    for (std::uint64_t k = 2;; ++k) {
        std::string n = stem + "-" + std::to_string(k) + ext;
        if (!used(n))
            return n;
    }
}

Status Store::apply(const ClientId& c, std::span<const Action> acts, ActResult& out) {
    out = ActResult{};
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    // The actions not yet applied, in order, from applied + 1 on: one durable step with the new applied number.
    std::uint64_t a = applied(c);
    std::vector<const Action*> fresh;
    for (const Action& x : acts) {
        if (x.n <= a)
            continue;
        if (x.n != a + 1 + fresh.size())
            break;
        fresh.push_back(&x);
    }
    out.applied = a;
    if (fresh.empty())
        return Status::ok();
    std::vector<Fact> facts;
    std::vector<Placement> places;
    std::map<TabId, std::size_t> queued;
    for (const Action* x : fresh) {
        auto q = queued.find(x->tab);
        if (q == queued.end()) {
            auto t = tabs_.find(x->tab);
            q = queued.emplace(x->tab, t == tabs_.end() ? 0 : t->second.queue.size()).first;
        }
        if (x->kind == Action::Kind::Queue && q->second >= kQueueCap) {
            out.refused.push_back(x->n);  // applied as a refusal: the person is told, nothing is dropped later
            continue;
        }
        Fact f;
        f.type = Fact::Type::TabOp;
        f.tab = x->tab;
        f.kind = x->kind == Action::Kind::Queue ? OpKind::Enq : OpKind::Send;
        f.text = x->text;
        f.blob = x->blob;
        facts.push_back(std::move(f));
        if (x->kind == Action::Kind::Queue)
            ++q->second;
        // The attachment is placed in the same durable step; a blob not complete is applied without its file.
        if (x->blob) {
            auto b = blobs_.find(*x->blob);
            if (b != blobs_.end() && b->second.complete) {
                std::string dir = options_.incoming(x->tab);
                Placement p{*x->blob, dir, unique_name(dir, x->file_name.empty() ? to_hex(*x->blob) : x->file_name,
                                                      places)};
                places.push_back(p);
                Fact pf;
                pf.type = Fact::Type::Place;
                pf.blob = p.blob;
                pf.dir = p.dir;
                pf.name = p.name;
                facts.push_back(std::move(pf));
            }
        }
    }
    Fact af;
    af.type = Fact::Type::Applied;
    af.client = c;
    af.a = fresh.back()->n;
    facts.push_back(af);
    if (!(s = commit(facts)).is_ok())
        return s;
    out.applied = af.a;
    for (const Placement& p : places) {
        if (!(s = place(p)).is_ok())
            return s;
    }
    return Status::ok();
}

// ---- blobs ----

Status Store::place(const Placement& p) {
    auto b = blobs_.find(p.blob);
    if (b == blobs_.end() || !b->second.complete)
        return Status::failed("store " + root_ + ": a placement of blob " + to_hex(p.blob) + " is recorded but the "
                              "blob is not complete in store/blobs; restore the store directory from a backup");
    Status s = fs_.make_dir(p.dir);
    if (!s.is_ok())
        return s;
    std::string tmp = p.dir + "/.nexus-" + p.name + ".tmp";
    std::uint64_t ignored = 0;
    if (fs_.size(tmp, ignored).is_ok() && !(s = fs_.remove(tmp)).is_ok())
        return s;
    if (!(s = fs_.create(tmp)).is_ok()) {
        failed_ = true;
        return s;
    }
    const std::uint64_t piece = 1 << 20;
    Bytes buf;
    for (std::uint64_t off = 0; off < b->second.size; off += piece) {
        if (!(s = fs_.read(blob_path(p.blob), off, piece, buf)).is_ok() || !(s = fs_.append(tmp, buf)).is_ok()) {
            failed_ = true;
            return s;
        }
    }
    if (!(s = fs_.sync(tmp)).is_ok() || !(s = fs_.rename(tmp, p.dir + "/" + p.name)).is_ok() ||
        !(s = fs_.sync_dir(p.dir)).is_ok()) {
        failed_ = true;
        return s;
    }
    // Done: recorded with the next durable step (a crash before that redoes the placement, which is idempotent).
    Fact f;
    f.type = Fact::Type::Placed;
    f.blob = p.blob;
    f.dir = p.dir;
    f.name = p.name;
    if (!(s = store_.append(encode_facts({f}))).is_ok()) {
        failed_ = true;
        return s;
    }
    apply_fact(f);
    return Status::ok();
}

BlobState Store::blob(const Hash& h) const {
    BlobState out;
    auto b = blobs_.find(h);
    if (b == blobs_.end())
        return out;
    out.known = true;
    out.size = b->second.size;
    out.held = b->second.held;
    out.attempt = b->second.attempt;
    out.complete = b->second.complete;
    return out;
}

Status Store::offer(const Hash& h, std::uint64_t size, BlobState& out) {
    out = BlobState{};
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    if (size == 0 || size > kMaxBlobSize)
        return Status::refused("blob " + to_hex(h) + ": " + std::to_string(size) + " bytes; an attachment holds 1 "
                               "byte to " + std::to_string(kMaxBlobSize) + " bytes");
    auto b = blobs_.find(h);
    if (b != blobs_.end()) {
        if (b->second.size != size)
            return Status::refused("blob " + to_hex(h) + " was offered with " + std::to_string(b->second.size) +
                                   " bytes, not " + std::to_string(size) + "; the hash names one content");
        out = blob(h);
        return Status::ok();
    }
    Fact f;
    f.type = Fact::Type::Offer;
    f.blob = h;
    f.a = size;
    if (!(s = commit({f})).is_ok())
        return s;
    out = blob(h);
    return Status::ok();
}

Status Store::chunk(const Hash& h, std::uint64_t attempt, std::uint64_t off, std::span<const std::uint8_t> bytes,
                    BlobState& out) {
    out = BlobState{};
    Status s = refuse_if_failed();
    if (!s.is_ok())
        return s;
    auto it = blobs_.find(h);
    if (it == blobs_.end())
        return Status::refused("blob " + to_hex(h) + " was never offered; offer it before sending chunks");
    Blob& b = it->second;
    if (bytes.empty() || off + bytes.size() > b.size)
        return Status::refused("blob " + to_hex(h) + ": a chunk of " + std::to_string(bytes.size()) + " bytes at " +
                               std::to_string(off) + " does not fit its " + std::to_string(b.size) + " bytes");
    // Blob.tla's RecvChunk: it extends what is held when it starts at or before the end held, belongs to the current
    // attempt and the blob is not complete. The bytes are durable before the Have that reports them.
    bool grows = !b.complete && attempt == b.attempt && off <= b.held && off + bytes.size() > b.held;
    if (grows) {
        std::string part = part_path(h, b.attempt);
        std::uint64_t ignored = 0;
        if (!fs_.size(part, ignored).is_ok()) {
            if (!(s = fs_.create(part)).is_ok() || !(s = fs_.sync_dir(root_ + "/store/blobs")).is_ok()) {
                failed_ = true;
                return s;
            }
        }
        std::span<const std::uint8_t> rest = bytes.subspan(std::size_t(b.held - off));
        if (!(s = fs_.append(part, rest)).is_ok() || !(s = fs_.sync(part)).is_ok()) {
            failed_ = true;
            return s;
        }
        b.held = off + bytes.size();
        if (b.held == b.size) {
            // The last unit: the hash check and completion in one durable step (the rename, made durable).
            Sha256 sha;
            Bytes buf;
            const std::uint64_t piece = 1 << 20;
            for (std::uint64_t at = 0; at < b.size; at += piece) {
                if (!(s = fs_.read(part, at, piece, buf)).is_ok()) {
                    failed_ = true;
                    return s;
                }
                sha.update(buf);
            }
            if (sha.finish() == h) {
                if (!(s = fs_.rename(part, blob_path(h))).is_ok() ||
                    !(s = fs_.sync_dir(root_ + "/store/blobs")).is_ok()) {
                    failed_ = true;
                    return s;
                }
                b.complete = true;
            } else {
                // A mismatch discards the attempt and starts a new one from zero.
                Fact f;
                f.type = Fact::Type::Retry;
                f.blob = h;
                f.a = b.attempt + 1;
                if (!(s = commit({f})).is_ok())
                    return s;
                if (!(s = fs_.remove(part)).is_ok()) {
                    failed_ = true;
                    return s;
                }
            }
        }
    }
    out = blob(h);
    return Status::ok();
}

Status Store::read_blob(const Hash& h, std::uint64_t off, std::uint64_t max, Bytes& out) {
    out.clear();
    auto b = blobs_.find(h);
    if (b == blobs_.end() || !b->second.complete)
        return Status::not_found("blob " + to_hex(h) + " is not complete on this server");
    if (off >= b->second.size)
        return Status::ok();
    return fs_.read(blob_path(h), off, max, out);
}

} // namespace nexus::store
