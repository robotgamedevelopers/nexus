// Unit tests of the server's durable state (src/store/). Everything runs on MemFs, in memory: no time (the clock is
// an argument), no sockets, no network, no files. Crashes are injected at every mutating filesystem call of each
// scenario, and each crash is taken through several plans (MemFs::plan), so that every write and sync point is a
// crash point.
//
//   store_test            runs every group
//   store_test <group>    runs one: codec memfs journal journal_crash store store_crash blob session restore

#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "store/store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace nexus::store;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_OK(expr)                                                                                   \
    do {                                                                                                 \
        Status s_ = (expr);                                                                              \
        if (!s_.is_ok()) {                                                                               \
            std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__, __LINE__, #expr, s_.message().c_str()); \
            ++failures;                                                                                  \
        }                                                                                                \
    } while (0)

const std::uint64_t kNow = 2000000000;  // the clock, as an argument

Bytes bytes_of(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

std::span<const std::uint8_t> span_of(const std::string& s) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

std::string hex(const Hash& h) {
    return to_hex(h);
}

// ---- codec ----

void test_codec() {
    CHECK(crc32c(span_of("123456789")) == 0xE3069283u);
    CHECK(crc32c(span_of("")) == 0);
    CHECK(crc32c(span_of("6789"), crc32c(span_of("12345"))) == 0xE3069283u);
    CHECK(hex(sha256(span_of(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(sha256(span_of("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(sha256(span_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // fed in pieces across the 64-byte block
    std::string million(1000, 'a');
    Sha256 s;
    for (int i = 0; i < 1000; ++i)
        s.update(span_of(million));
    CHECK(hex(s.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    Writer w;
    w.u8(7);
    w.u32(0xDEADBEEF);
    w.u64(0x0102030405060708ull);
    w.str("hello");
    Reader r(w.out());
    CHECK(r.u8() == 7);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0102030405060708ull);
    CHECK(r.str() == "hello");
    CHECK(r.ok() && r.done());
    CHECK(r.u32() == 0);
    CHECK(!r.ok());
    Bytes trunc(w.out().begin(), w.out().begin() + 15);
    Reader r2(trunc);
    r2.u8();
    r2.u32();
    r2.u64();
    CHECK(r2.str().empty());
    CHECK(!r2.ok());
    CHECK(sanitise_file_name("a b/c.pdf") == "a_b_c.pdf");
    CHECK(sanitise_file_name("..hidden") == "hidden");
    CHECK(sanitise_file_name("") == "file");
    CHECK(sanitise_file_name(std::string(300, 'x')).size() == 128);
}

// ---- MemFs ----

void test_memfs() {
    MemFs fs;
    CHECK_OK(fs.make_dir("d"));
    CHECK_OK(fs.create("d/a"));
    CHECK_OK(fs.append("d/a", span_of("hello")));
    // nothing synced: the create and the bytes go
    {
        MemFs c = fs;
        c.crash(c.plan_all(false));
        CHECK(!c.has("d/a"));
    }
    // kept: everything
    {
        MemFs c = fs;
        c.crash(c.plan_all(true));
        CHECK(c.data("d/a") == bytes_of("hello"));
    }
    CHECK_OK(fs.sync_dir("d"));
    CHECK_OK(fs.sync("d/a"));
    CHECK_OK(fs.append("d/a", span_of("world")));
    // a torn write: the synced bytes, three more, the second of them damaged
    {
        MemFs c = fs;
        MemFs::CrashPlan p;
        p.files["d/a"] = MemFs::FileFate{false, 3, 6, 7};
        c.crash(p);
        Bytes want = bytes_of("hellowor");
        want[6] ^= 0xA5;
        CHECK(c.data("d/a") == want);
        CHECK(c.durable("d/a") == want);
    }
    // a rename is atomic, and kept or not as one change
    CHECK_OK(fs.create("d/b"));
    CHECK_OK(fs.sync("d/b"));
    CHECK_OK(fs.rename("d/b", "d/a"));
    CHECK(fs.data("d/a").empty());
    {
        MemFs c = fs;
        MemFs::CrashPlan p;
        p.dir_keep["d"] = {false, true};  // the create lost, the rename kept: the name a goes to b's file
        c.crash(p);
        CHECK(c.has("d/a") && !c.has("d/b"));
        CHECK(c.data("d/a").empty());
    }
    {
        MemFs c = fs;
        MemFs::CrashPlan p;
        p.dir_keep["d"] = {true, false};
        p.files["d/a"] = MemFs::FileFate{false, 100, 0, 0};
        c.crash(p);
        CHECK(c.has("d/a") && c.has("d/b"));
        CHECK(c.data("d/a") == bytes_of("helloworld"));
    }
    // crash injection: the k-th call fails and every later one too
    MemFs f2;
    CHECK_OK(f2.make_dir("x"));
    f2.fail_at(2);
    CHECK_OK(f2.create("x/1"));
    CHECK(!f2.create("x/2").is_ok());
    CHECK(f2.crashed());
    CHECK(!f2.create("x/3").is_ok());
    f2.crash(f2.plan_all(true));
    CHECK(!f2.crashed());
    CHECK(f2.has("x/1") && !f2.has("x/2"));
    // truncate kept or lost
    MemFs f3;
    CHECK_OK(f3.make_dir("t"));
    CHECK_OK(f3.create("t/f"));
    CHECK_OK(f3.append("t/f", span_of("abcdef")));
    CHECK_OK(f3.sync("t/f"));
    CHECK_OK(f3.sync_dir("t"));
    CHECK_OK(f3.truncate("t/f", 2));
    {
        MemFs c = f3;
        c.crash(c.plan_all(false));
        CHECK(c.data("t/f") == bytes_of("abcdef"));
    }
    {
        MemFs c = f3;
        c.crash(c.plan_all(true));
        CHECK(c.data("t/f") == bytes_of("ab"));
    }
    std::vector<std::string> names;
    CHECK_OK(f3.list("t", names));
    CHECK(names.size() == 1 && names[0] == "f");
    CHECK(f3.list("nope", names).code() == Status::Code::NotFound);
}

// ---- journal ----

std::string rec_text(std::uint64_t i) {
    return "record " + std::to_string(i) + std::string(i % 7, '.');
}

struct OpenedJournal {
    std::unique_ptr<Journal> j;
    Journal::Recovery rec;
    Status st;
};

OpenedJournal open_journal(MemFs& fs) {
    OpenedJournal o;
    o.j = std::make_unique<Journal>(fs, "j");
    o.st = o.j->open(o.rec);
    return o;
}

// What the journal holds: the snapshot's own text (the test's snapshots list the records they stand for, one per
// line) and the records after it.
std::vector<std::string> contents(const Journal::Recovery& rec) {
    std::vector<std::string> out;
    std::string snap(rec.snapshot.begin(), rec.snapshot.end());
    std::size_t at = 0;
    while (at < snap.size()) {
        std::size_t nl = snap.find('\n', at);
        out.push_back(snap.substr(at, nl - at));
        at = nl + 1;
    }
    for (const Bytes& b : rec.records)
        out.push_back(std::string(b.begin(), b.end()));
    return out;
}

std::string snapshot_of(const std::vector<std::string>& recs) {
    std::string s;
    for (const std::string& r : recs)
        s += r + "\n";
    return s;
}

void test_journal() {
    MemFs fs;
    {
        OpenedJournal o = open_journal(fs);
        CHECK_OK(o.st);
        CHECK(o.j->last_seq() == 0);
        for (int i = 1; i <= 3; ++i)
            CHECK_OK(o.j->append(span_of(rec_text(std::uint64_t(i)))));
        CHECK(o.j->durable_seq() == 0);
        CHECK_OK(o.j->sync());
        CHECK(o.j->durable_seq() == 3);
        CHECK(o.j->append(Bytes{}).code() == Status::Code::Refused);
    }
    fs.crash(fs.plan_all(false));
    {
        OpenedJournal o = open_journal(fs);
        CHECK_OK(o.st);
        CHECK(o.j->last_seq() == 3);
        std::vector<std::string> want = {rec_text(1), rec_text(2), rec_text(3)};
        CHECK(contents(o.rec) == want);
        // compaction: the snapshot stands for 1..3, the log starts again
        CHECK_OK(o.j->compact(span_of(snapshot_of(want))));
        CHECK(o.j->log_bytes() == 0);
        CHECK_OK(o.j->append(span_of(rec_text(4))));
        CHECK_OK(o.j->sync());
    }
    fs.crash(fs.plan_all(false));
    {
        OpenedJournal o = open_journal(fs);
        CHECK_OK(o.st);
        CHECK(o.rec.snapshot_seq == 3);
        CHECK(o.j->last_seq() == 4);
        std::vector<std::string> want = {rec_text(1), rec_text(2), rec_text(3), rec_text(4)};
        CHECK(contents(o.rec) == want);
    }

    // A torn tail at every length and with every damaged byte: the synced records stay, whole unsynced records stay,
    // the rest is cut, and the log is truncated so that the next record follows cleanly.
    MemFs base;
    std::uint64_t synced_bytes = 0;
    std::uint64_t total = 0;
    {
        OpenedJournal o = open_journal(base);
        CHECK_OK(o.j->append(span_of(rec_text(1))));
        CHECK_OK(o.j->append(span_of(rec_text(2))));
        CHECK_OK(o.j->sync());
        synced_bytes = o.j->log_bytes();
        CHECK_OK(o.j->append(span_of(rec_text(3))));
        CHECK_OK(o.j->append(span_of(rec_text(4))));
        total = o.j->log_bytes();
    }
    std::uint64_t rec3_end = synced_bytes + Journal::kRecordHeader + rec_text(3).size();
    std::uint64_t cases = 0;
    for (std::uint64_t keep = 0; keep <= total - synced_bytes; ++keep) {
        for (std::uint64_t bad = synced_bytes; bad <= synced_bytes + keep; ++bad) {
            MemFs c = base;
            MemFs::CrashPlan p;
            p.dir_keep["j"] = {};
            bool damaged = bad < synced_bytes + keep;
            p.files["j/log"] = MemFs::FileFate{false, keep, damaged ? bad : 0, damaged ? bad + 1 : 0};
            c.crash(p);
            OpenedJournal o = open_journal(c);
            CHECK_OK(o.st);
            std::uint64_t end = synced_bytes + keep;
            std::uint64_t want = 2;
            if (end >= rec3_end && !(damaged && bad < rec3_end))
                want = 3;
            if (end == total && !damaged)
                want = 4;
            CHECK(o.j->last_seq() == want);
            CHECK(o.rec.records.size() == want);
            std::uint64_t cut_to = want == 2 ? synced_bytes : want == 3 ? rec3_end : total;
            CHECK(c.data("j/log").size() == cut_to);
            CHECK(o.rec.cut_bytes == end - cut_to);
            // the next record follows the cut, and survives the next crash once synced
            CHECK_OK(o.j->append(span_of("next")));
            CHECK_OK(o.j->sync());
            c.crash(c.plan_all(false));
            OpenedJournal o2 = open_journal(c);
            CHECK_OK(o2.st);
            CHECK(o2.j->last_seq() == want + 1);
            CHECK(!o2.rec.records.empty() && std::string(o2.rec.records.back().begin(), o2.rec.records.back().end()) ==
                                                 "next");
            ++cases;
        }
    }
    CHECK(cases > 100);

    // A damaged snapshot is a disk fault: open refuses, and says so.
    {
        MemFs c = fs;
        c.corrupt("j/snapshot", 30);
        OpenedJournal o = open_journal(c);
        CHECK(o.st.code() == Status::Code::Failed);
        CHECK(o.st.message().find("snapshot is damaged") != std::string::npos);
    }
    // A damaged durable record is cut like a torn tail (the checksum cannot tell them apart); what follows it goes.
    {
        MemFs c = fs;
        c.corrupt("j/log", 20);
        OpenedJournal o = open_journal(c);
        CHECK_OK(o.st);
        CHECK(o.j->last_seq() == 3);
    }
    // A log that ends before the snapshot's number, and a gap after it: refused.
    {
        MemFs c;
        OpenedJournal o = open_journal(c);
        CHECK_OK(o.j->append(span_of("a")));
        CHECK_OK(o.j->sync());
        MemFs d = c;  // the log holds record 1
        CHECK_OK(o.j->append(span_of("b")));
        CHECK_OK(o.j->append(span_of("c")));
        CHECK_OK(o.j->compact(span_of("a\nb\nc\n")));
        // put the old log (record 1 only) back beside the snapshot of 1..3
        MemFs e = c;
        CHECK_OK(e.remove("j/log"));
        CHECK_OK(e.create("j/log"));
        CHECK_OK(e.append("j/log", d.data("j/log")));
        OpenedJournal o2 = open_journal(e);
        CHECK(o2.st.code() == Status::Code::Failed);
        CHECK(o2.st.message().find("ends at record 1") != std::string::npos);
        // a log whose first record after the snapshot is 5: records 4 is missing
        MemFs g;
        OpenedJournal o3 = open_journal(g);
        for (int i = 0; i < 5; ++i)
            CHECK_OK(o3.j->append(span_of("x")));
        CHECK_OK(o3.j->sync());
        Bytes log = g.data("j/log");
        Bytes tail(log.begin() + 4 * (Journal::kRecordHeader + 1), log.end());  // record 5 alone
        MemFs h = c;
        CHECK_OK(h.remove("j/log"));
        CHECK_OK(h.create("j/log"));
        CHECK_OK(h.append("j/log", tail));
        OpenedJournal o4 = open_journal(h);
        CHECK(o4.st.code() == Status::Code::Failed);
        CHECK(o4.st.message().find("records 4 to 4 are missing") != std::string::npos);
    }
    // After a failed write the journal refuses everything until it is opened again.
    {
        MemFs c;
        OpenedJournal o = open_journal(c);
        c.fail_at(1);
        CHECK(!o.j->append(span_of("a")).is_ok());
        CHECK(o.j->failed());
        c.crash(c.plan_all(true));
        CHECK(o.j->append(span_of("b")).code() == Status::Code::Failed);
        CHECK(o.j->append(span_of("b")).message().find("restart the server") != std::string::npos);
        Journal::Recovery rec;
        CHECK_OK(o.j->open(rec));
        CHECK_OK(o.j->append(span_of("b")));
    }
}

// Runs `scenario` with a crash injected at each of its mutating calls in turn, each crash taken through several plans;
// `check` is given the filesystem after the crash. Returns the number of crashes taken.
std::uint64_t at_every_point(const MemFs& start, const std::function<void(MemFs&)>& scenario,
                             const std::function<void(MemFs&, std::uint64_t point)>& check) {
    std::uint64_t taken = 0;
    for (std::uint64_t point = 1;; ++point) {
        bool reached = false;
        for (std::uint64_t plan = 0; plan < 10; ++plan) {
            MemFs fs = start;
            fs.fail_at(point);
            scenario(fs);
            if (!fs.crashed())
                break;
            reached = true;
            if (plan == 0) {
                fs.crash(fs.plan_all(false));
            } else if (plan == 1) {
                fs.crash(fs.plan_all(true));
            } else {
                Rng rng(point * 131 + plan);
                fs.crash(fs.plan(rng));
            }
            check(fs, point);
            ++taken;
        }
        if (!reached)
            return taken;
    }
}

void test_journal_crash() {
    // A scenario of appends, syncs and two compactions; record i's text is rec_text(i). Acknowledged: what the last
    // successful sync made durable.
    std::uint64_t acked = 0;
    auto scenario = [&acked](MemFs& fs) {
        acked = 0;
        Journal j(fs, "j");
        Journal::Recovery rec;
        if (!j.open(rec).is_ok())
            return;
        std::vector<std::string> all;
        std::uint64_t n = 0;
        auto add = [&](int count) {
            for (int i = 0; i < count; ++i) {
                ++n;
                if (!j.append(span_of(rec_text(n))).is_ok())
                    return false;
                all.push_back(rec_text(n));
            }
            if (!j.sync().is_ok())
                return false;
            acked = j.durable_seq();
            return true;
        };
        if (!add(2) || !add(1))
            return;
        if (!j.compact(span_of(snapshot_of(all))).is_ok())
            return;
        if (!add(3))
            return;
        if (!j.append(span_of(rec_text(++n))).is_ok())
            return;
        all.push_back(rec_text(n));
        if (!j.compact(span_of(snapshot_of(all))).is_ok())
            return;
        acked = j.durable_seq();
        add(2);
    };
    std::uint64_t taken = at_every_point(MemFs(), scenario, [&acked](MemFs& fs, std::uint64_t point) {
        // Open again, crashing inside open at each of its calls too, then once cleanly.
        for (std::uint64_t k = 1; k <= 4; ++k) {
            MemFs c = fs;
            c.fail_at(k);
            Journal j(c, "j");
            Journal::Recovery rec;
            if (j.open(rec).is_ok())
                break;
            Rng rng(point + k);
            c.crash(c.plan(rng));
            OpenedJournal o = open_journal(c);
            CHECK_OK(o.st);
            CHECK(o.j->last_seq() >= acked);
        }
        OpenedJournal o = open_journal(fs);
        CHECK_OK(o.st);
        CHECK(o.j->last_seq() >= acked);
        std::vector<std::string> got = contents(o.rec);
        CHECK(got.size() == o.j->last_seq());
        for (std::size_t i = 0; i < got.size(); ++i)
            CHECK(got[i] == rec_text(i + 1));
        // and it goes on: a record after the recovered ones survives a crash
        CHECK_OK(o.j->append(span_of("after")));
        CHECK_OK(o.j->sync());
        fs.crash(fs.plan_all(false));
        OpenedJournal o2 = open_journal(fs);
        CHECK_OK(o2.st);
        CHECK(o2.j->last_seq() == got.size() + 1);
    });
    CHECK(taken > 200);
}

// ---- store ----

struct Opened {
    std::unique_ptr<Store> s;
    OpenReport report;
    Status st;
};

Opened open_store(MemFs& fs, std::uint64_t now = kNow, StoreOptions options = {}) {
    Opened o;
    o.st = Store::open(fs, "root", now, options, o.s, &o.report);
    return o;
}

std::string texts(const std::vector<Item>& items) {
    std::string out;
    for (const Item& it : items)
        out += "[" + it.text + (it.open ? "+" : "") + "]";
    return out;
}

void test_store() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    CHECK(o.report.fresh && !o.report.restored);
    Store& s = *o.s;
    CHECK(s.incarnation() == kNow);
    CHECK(s.generation() == kNow);
    CHECK(s.version(1) == 0);

    CHECK(s.grow(1, "x").code() == Status::Code::Refused);
    CHECK(s.close(1).code() == Status::Code::Refused);
    CHECK(s.deliver(1).code() == Status::Code::Refused);
    CHECK(s.version(1) == 0);

    CHECK_OK(s.send(1, "hi"));             // v1
    CHECK_OK(s.open_entry(1, "Hel"));      // v2
    CHECK_OK(s.grow(1, "lo"));             // v3
    CHECK_OK(s.enqueue(1, "later"));       // v4
    CHECK_OK(s.enqueue(1, "much later"));  // v5
    CHECK(s.version(1) == 5);
    Snap sn = s.snapshot(1, 10);
    CHECK(texts(sn.entries) == "[hi][Hello+]");
    CHECK(texts(sn.queue) == "[later][much later]");
    CHECK(sn.lo == 1 && sn.version == 5);
    CHECK_OK(s.deliver(1));  // v6: closes the open entry, the oldest prompt moves to the end
    sn = s.snapshot(1, 10);
    CHECK(texts(sn.entries) == "[hi][Hello][later]");
    CHECK(texts(sn.queue) == "[much later]");
    CHECK(sn.entries[2].id == 4);  // a delivered prompt keeps its id: the version that queued it
    CHECK(sn.entries[0].id == 1 && sn.entries[0].gen == s.generation());
    CHECK(sn.entries[1].author == Author::Friend && sn.entries[0].author == Author::Person);

    // the tail of a snapshot: lo is the position of its first entry
    sn = s.snapshot(1, 2);
    CHECK(sn.lo == 2 && texts(sn.entries) == "[Hello][later]");

    // a batch is all or none
    std::vector<Change> batch = {{1, OpKind::New, "a", {}}, {1, OpKind::Grow, "b", {}}, {1, OpKind::Close, {}, {}},
                                 {1, OpKind::Grow, "c", {}}};
    CHECK(s.change(batch).code() == Status::Code::Refused);
    CHECK(s.version(1) == 6);
    batch.pop_back();
    CHECK_OK(s.change(batch));  // v7, v8, v9
    CHECK(s.version(1) == 9);

    // diffs: from any version still kept, at most max_ops
    std::vector<TabOp> ops;
    CHECK(s.diff(1, 9, 32, ops) && ops.empty());
    CHECK(s.diff(1, 5, 32, ops) && ops.size() == 4);
    CHECK(ops[0].kind == OpKind::Deliver && ops[0].id == 4);
    CHECK(ops[1].kind == OpKind::New && ops[1].id == 7 && ops[1].text == "a");
    CHECK(s.diff(1, 0, 2, ops) && ops.size() == 2 && ops[0].kind == OpKind::Send && ops[1].kind == OpKind::New);
    CHECK(!s.diff(1, 10, 32, ops));

    // older entries, before position lo
    CHECK(texts(s.older(1, 3, 10)) == "[hi][Hello]");
    CHECK(texts(s.older(1, 3, 1)) == "[Hello]");
    CHECK(s.older(1, 1, 10).empty());
    CHECK(s.older(1, 99, 10).empty());

    // text by range, cut on a UTF-8 boundary
    std::string out;
    CHECK(s.content(1, 2, 1, 3, out) && out == "ell");
    CHECK(s.content(1, 2, 3, 99, out) && out == "lo");
    CHECK(!s.content(1, 2, 5, 3, out));
    CHECK(!s.content(1, 77, 0, 3, out));
    CHECK_OK(s.send(2, "a\xC3\xA9z"));  // a, e-acute (two bytes), z
    CHECK(s.content(2, 1, 0, 2, out) && out == "a");
    CHECK(s.content(2, 1, 0, 3, out) && out == "a\xC3\xA9");
    CHECK(s.content(2, 1, 1, 1, out) && out == "\xC3");  // never empty: one byte when the page is smaller

    // the queue holds 64; the 65th is refused and nothing changes
    for (std::size_t i = 0; i < kQueueCap; ++i)
        CHECK_OK(s.enqueue(3, "q" + std::to_string(i)));
    std::uint64_t v = s.version(3);
    Status full = s.enqueue(3, "one too many");
    CHECK(full.code() == Status::Code::Refused);
    CHECK(full.message().find("64") != std::string::npos);
    CHECK(s.version(3) == v);
    CHECK(s.snapshot(3, 1).queue.size() == kQueueCap);
    CHECK_OK(s.deliver(3));
    CHECK_OK(s.enqueue(3, "now it fits"));
    CHECK(s.tabs() == (std::vector<TabId>{1, 2, 3}));

    // everything is durable: a crash that keeps nothing volatile loses nothing acknowledged
    Snap before = s.snapshot(1, 100);
    Snap before3 = s.snapshot(3, 100);
    o.s.reset();
    fs.crash(fs.plan_all(false));
    Opened o2 = open_store(fs, kNow - 50);  // the clock went back: the incarnation still goes forward
    CHECK_OK(o2.st);
    CHECK(!o2.report.fresh && !o2.report.restored);
    CHECK(o2.s->incarnation() == kNow + 1);
    CHECK(o2.s->generation() == kNow);
    Snap after = o2.s->snapshot(1, 100);
    CHECK(texts(after.entries) == texts(before.entries) && texts(after.queue) == texts(before.queue));
    CHECK(after.version == before.version);
    CHECK(texts(o2.s->snapshot(3, 100).queue) == texts(before3.queue));
    CHECK(o2.s->diff(1, 5, 32, ops) && ops.size() == 4);

    // compaction: a small threshold compacts often; the state, recent operations included, survives
    StoreOptions small;
    small.compact_bytes = 300;
    small.recent_ops = 4;
    MemFs fs2;
    Opened c = open_store(fs2, kNow, small);
    CHECK_OK(c.st);
    for (int i = 0; i < 40; ++i)
        CHECK_OK(c.s->send(9, "message " + std::to_string(i)));
    CHECK(c.s->store_journal().log_bytes() <= 300);
    CHECK(!c.s->diff(9, 35, 32, ops));  // older than the four kept: a snapshot instead
    CHECK(c.s->diff(9, 36, 32, ops) && ops.size() == 4 && ops[3].text == "message 39");
    c.s.reset();
    fs2.crash(fs2.plan_all(false));
    Opened c2 = open_store(fs2, kNow, small);
    CHECK_OK(c2.st);
    CHECK(c2.s->version(9) == 40);
    CHECK(c2.s->snapshot(9, 100).entries.size() == 40);
    CHECK(c2.s->diff(9, 36, 32, ops) && ops.size() == 4 && ops[0].text == "message 36");
}

// A digest of the state a store shows, to compare a recovered store with the states the scenario passed through.
std::string digest(const Store& s, const std::vector<ClientId>& clients) {
    std::string d;
    for (TabId t : s.tabs()) {
        Snap sn = s.snapshot(t, 1000);
        d += "tab" + std::to_string(t) + "@" + std::to_string(sn.version) + texts(sn.entries) + "|" + texts(sn.queue);
    }
    for (const ClientId& c : clients)
        d += " applied" + std::to_string(c[0]) + "=" + std::to_string(s.applied(c));
    return d;
}

ClientId client(std::uint8_t n) {
    ClientId c{};
    c[0] = n;
    return c;
}

void test_store_crash() {
    // The scenario: tab changes, a batch, actions applied twice (exactly once), a delivery, enough to compact. Each
    // step's digest is recorded on a run without crashes; after a crash at any point the store must show the digest
    // of some step at or after the last one acknowledged, and must not think it was restored.
    StoreOptions options;
    options.compact_bytes = 400;
    std::vector<ClientId> clients = {client(1), client(2)};
    MemFs start;
    {
        Opened o = open_store(start, kNow, options);
        CHECK_OK(o.st);
    }
    std::vector<std::string> digests;
    std::size_t acked = 0;
    bool reference = true;
    auto scenario = [&](MemFs& fs) {
        acked = 0;
        Opened o = open_store(fs, kNow + 1, options);
        if (!o.st.is_ok())
            return;
        Store& s = *o.s;
        auto step = [&](Status st) {
            if (!st.is_ok())
                return false;
            if (reference)
                digests.push_back(digest(s, clients));
            ++acked;
            return true;
        };
        if (reference)
            digests.push_back(digest(s, clients));
        if (!step(s.send(1, "hello")) || !step(s.open_entry(1, "rep")) || !step(s.grow(1, "ly")))
            return;
        std::vector<Change> batch = {{1, OpKind::Close, {}, {}}, {2, OpKind::Enq, "q1", {}}, {2, OpKind::Enq, "q2", {}}};
        if (!step(s.change(batch)))
            return;
        std::vector<Action> acts = {{1, 1, Action::Kind::Send, "a1", {}, {}}, {2, 2, Action::Kind::Queue, "a2", {}, {}}};
        ActResult r;
        if (!step(s.apply(clients[0], acts, r)))
            return;
        acts.push_back({3, 1, Action::Kind::Send, "a3", {}, {}});
        if (!step(s.apply(clients[0], acts, r)))  // 1 and 2 again: only 3 applies
            return;
        if (!step(s.deliver(2)))
            return;
        for (int i = 0; i < 6; ++i) {
            if (!step(s.send(3, "filler " + std::to_string(i))))
                return;
        }
        std::vector<Action> other = {{1, 3, Action::Kind::Send, "b1", {}, {}}};
        step(s.apply(clients[1], other, r));
    };
    {
        MemFs ref = start;
        scenario(ref);
        reference = false;
        CHECK(acked == 14);
        CHECK(digests.size() == 15);
    }
    std::uint64_t taken = at_every_point(start, scenario, [&](MemFs& fs, std::uint64_t) {
        Opened o = open_store(fs, kNow + 2, options);
        CHECK_OK(o.st);
        if (!o.st.is_ok())
            return;
        CHECK(!o.report.restored && !o.report.fresh);
        CHECK(o.s->generation() == kNow);
        std::string d = digest(*o.s, clients);
        bool found = false;
        for (std::size_t i = acked; i < digests.size(); ++i)
            found = found || digests[i] == d;
        CHECK(found);
        if (!found)
            std::fprintf(stderr, "  acked %zu, recovered %s\n", acked, d.c_str());
    });
    CHECK(taken > 300);
}

// ---- blobs ----

Hash hash_of(const std::string& s) {
    return sha256(span_of(s));
}

void test_blob() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    Store& s = *o.s;
    std::string content = "the attachment's bytes, thirty-nine long";
    Hash h = hash_of(content);
    BlobState b;
    CHECK(s.offer(h, 0, b).code() == Status::Code::Refused);
    CHECK(s.offer(h, kMaxBlobSize + 1, b).code() == Status::Code::Refused);
    CHECK(s.chunk(h, 0, 0, span_of("x"), b).code() == Status::Code::Refused);  // not offered
    CHECK_OK(s.offer(h, content.size(), b));
    CHECK(b.known && b.held == 0 && b.attempt == 0 && !b.complete);
    CHECK(s.offer(h, content.size() + 1, b).code() == Status::Code::Refused);
    CHECK(s.chunk(h, 0, 30, span_of(std::string(20, 'x')), b).code() == Status::Code::Refused);  // past the size

    CHECK_OK(s.chunk(h, 0, 0, span_of(content.substr(0, 10)), b));
    CHECK(b.held == 10);
    CHECK_OK(s.chunk(h, 0, 20, span_of(content.substr(20, 5)), b));  // past what is held: ignored
    CHECK(b.held == 10);
    CHECK_OK(s.chunk(h, 1, 10, span_of(content.substr(10, 5)), b));  // another attempt: ignored
    CHECK(b.held == 10);
    CHECK_OK(s.chunk(h, 0, 5, span_of(content.substr(5, 15)), b));  // overlaps: extends
    CHECK(b.held == 20);

    // a server restart keeps what is held; the upload resumes from there
    o.s.reset();
    fs.crash(fs.plan_all(false));
    o = open_store(fs);
    CHECK_OK(o.st);
    b = o.s->blob(h);
    CHECK(b.known && b.held == 20 && b.attempt == 0 && !b.complete);
    CHECK_OK(o.s->chunk(h, 0, 20, span_of(content.substr(20)), b));
    CHECK(b.complete && b.held == content.size());
    CHECK(fs.has("root/store/blobs/" + hex(h)));
    Bytes got;
    CHECK_OK(o.s->read_blob(h, 4, 10, got));
    CHECK(std::string(got.begin(), got.end()) == content.substr(4, 10));
    CHECK_OK(o.s->chunk(h, 0, 0, span_of(content.substr(0, 3)), b));  // complete: nothing changes
    CHECK(b.complete);

    // a wrong byte: the hash check at the end discards the attempt; the next starts from zero
    std::string other = "another file";
    Hash h2 = hash_of(other);
    CHECK_OK(o.s->offer(h2, other.size(), b));
    std::string wrong = other;
    wrong[3] = '!';
    CHECK_OK(o.s->chunk(h2, 0, 0, span_of(wrong), b));
    CHECK(!b.complete && b.attempt == 1 && b.held == 0);
    CHECK(!fs.has("root/store/blobs/" + hex(h2) + ".0.part"));
    CHECK(o.s->read_blob(h2, 0, 10, got).code() == Status::Code::NotFound);
    CHECK_OK(o.s->chunk(h2, 0, 0, span_of(other), b));  // the old attempt: ignored
    CHECK(b.held == 0);
    CHECK_OK(o.s->chunk(h2, 1, 0, span_of(other), b));
    CHECK(b.complete && b.attempt == 1);
    o.s.reset();
    fs.crash(fs.plan_all(false));
    o = open_store(fs);
    CHECK(o.s->blob(h2).complete && o.s->blob(h2).attempt == 1);

    // applying an action with the blob places it for the tab's AI, in the same durable step, under a unique name
    ActResult r;
    std::vector<Action> acts = {{1, 4, Action::Kind::Send, "see attached", h, "my notes.txt"},
                                {2, 4, Action::Kind::Send, "and again", h, "my notes.txt"}};
    CHECK_OK(o.s->apply(client(1), acts, r));
    CHECK(r.applied == 2);
    CHECK(std::string(fs.data("root/incoming/4/my_notes.txt").begin(), fs.data("root/incoming/4/my_notes.txt").end()) ==
          content);
    CHECK(fs.has("root/incoming/4/my_notes-2.txt"));
    CHECK(o.s->snapshot(4, 10).entries[0].blob == h);
    // a blob not complete is applied without the file
    Hash h3 = hash_of("never sent");
    acts = {{3, 4, Action::Kind::Send, "missing", h3, "gone.txt"}};
    CHECK_OK(o.s->apply(client(1), acts, r));
    CHECK(!fs.has("root/incoming/4/gone.txt"));

    // a crash after the action is durable and before its file is placed: open places it
    std::uint64_t taken = 0;
    for (std::uint64_t point = 1; point < 40; ++point) {
        MemFs c = fs;
        Opened oc = open_store(c);
        CHECK_OK(oc.st);
        c.fail_at(point);
        std::vector<Action> a5 = {{4, 5, Action::Kind::Queue, "file", h, "f.txt"}};
        Status st = oc.s->apply(client(1), a5, r);
        if (!c.crashed())
            break;
        bool durable = false;
        {
            MemFs probe = c;
            probe.crash(probe.plan_all(false));
            Opened op = open_store(probe);
            durable = op.st.is_ok() && op.s->applied(client(1)) == 4;
        }
        c.crash(c.plan_all(false));
        Opened again = open_store(c);
        CHECK_OK(again.st);
        if (durable) {
            CHECK(again.s->applied(client(1)) == 4);
            CHECK(c.has("root/incoming/5/f.txt"));
            CHECK(c.data("root/incoming/5/f.txt") == bytes_of(content));
        } else {
            CHECK(again.s->applied(client(1)) == 3);
            CHECK(!c.has("root/incoming/5/f.txt"));
        }
        ++taken;
        (void)st;
    }
    CHECK(taken > 5);
}

// ---- session ----

void test_session() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    Store& s = *o.s;
    ClientId c = client(7);
    HelloResult h;
    CHECK_OK(s.hello(c, 3, 0, 1, h));
    CHECK(!h.superseded && h.applied == 0 && h.inc == kNow && h.gen == kNow);
    CHECK(s.seen(c) == 3);
    CHECK_OK(s.hello(c, 2, 0, 1, h));
    CHECK(h.superseded);
    CHECK(s.seen(c) == 3);
    CHECK_OK(s.hello(c, 3, kNow, 1, h));  // the same epoch again: welcome
    CHECK(!h.superseded);

    // exactly once: duplicates, overlaps and a gap
    ActResult r;
    std::vector<Action> acts = {{1, 1, Action::Kind::Send, "one", {}, {}}, {2, 1, Action::Kind::Send, "two", {}, {}}};
    CHECK_OK(s.apply(c, acts, r));
    CHECK(r.applied == 2);
    CHECK_OK(s.apply(c, acts, r));
    CHECK(r.applied == 2 && s.version(1) == 2);
    acts = {{2, 1, Action::Kind::Send, "two", {}, {}}, {3, 1, Action::Kind::Send, "three", {}, {}},
            {5, 1, Action::Kind::Send, "five", {}, {}}};
    CHECK_OK(s.apply(c, acts, r));
    CHECK(r.applied == 3);  // 5 is past a gap: not applied
    CHECK(texts(s.snapshot(1, 10).entries) == "[one][two][three]");
    acts = {{9, 1, Action::Kind::Send, "nine", {}, {}}};
    CHECK_OK(s.apply(c, acts, r));
    CHECK(r.applied == 3);

    // a queue action on a full queue is applied as a refusal: counted, never queued, never dropped later
    for (std::size_t i = 0; i < kQueueCap - 1; ++i)
        CHECK_OK(s.enqueue(2, "p"));
    acts = {{4, 2, Action::Kind::Queue, "fits", {}, {}}, {5, 2, Action::Kind::Queue, "refused", {}, {}},
            {6, 2, Action::Kind::Send, "sent", {}, {}}};
    CHECK_OK(s.apply(c, acts, r));
    CHECK(r.applied == 6);
    CHECK(r.refused == std::vector<std::uint64_t>{5});
    Snap sn = s.snapshot(2, 10);
    CHECK(sn.queue.size() == kQueueCap && sn.queue.back().text == "fits");
    CHECK(texts(sn.entries) == "[sent]");

    // durable: applied and seen survive a crash
    o.s.reset();
    fs.crash(fs.plan_all(false));
    o = open_store(fs);
    CHECK_OK(o.st);
    CHECK(o.s->applied(c) == 6);
    CHECK(o.s->seen(c) == 3);
    CHECK(o.s->applied(client(8)) == 0 && o.s->seen(client(8)) == 0);
    CHECK_OK(o.s->hello(c, 4, kNow, 7, h));
    CHECK(h.applied == 6 && h.inc == kNow + 1);
    CHECK(o.s->skipped(c).empty());
}

// ---- restore ----

void test_restore() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    ClientId c = client(1);
    HelloResult h;
    CHECK_OK(o.s->hello(c, 1, 0, 1, h));
    ActResult r;
    std::vector<Action> acts = {{1, 1, Action::Kind::Send, "before the backup", {}, {}}};
    CHECK_OK(o.s->apply(c, acts, r));
    MemFs backup = fs;  // a backup of everything, taken now
    acts = {{2, 1, Action::Kind::Send, "after the backup", {}, {}}, {3, 1, Action::Kind::Send, "and this", {}, {}}};
    CHECK_OK(o.s->apply(c, acts, r));
    CHECK_OK(o.s->hello(c, 2, kNow, 4, h));
    std::uint64_t gen1 = o.s->generation();
    o.s.reset();

    // Only the store is put back: detected, and the generation moves to the new incarnation.
    MemFs restored = fs;
    restored.restore_tree(backup, "root/store");
    Opened o2 = open_store(restored, kNow + 10);
    CHECK_OK(o2.st);
    CHECK(o2.report.restored);
    CHECK(o2.s->incarnation() == kNow + 10);
    CHECK(o2.s->generation() == kNow + 10 && o2.s->generation() > gen1);
    CHECK(o2.s->applied(c) == 1);
    CHECK(o2.s->seen(c) == 2);  // outside the store: never restored
    CHECK_OK(o2.s->hello(c, 1, gen1, 4, h));
    CHECK(h.superseded);
    // the client's Hello carries the old generation and the first number it still holds (4): numbers 2..3 were acked
    // by the store that is gone, and are skipped
    CHECK_OK(o2.s->hello(c, 3, gen1, 4, h));
    CHECK(!h.superseded && h.applied == 3 && h.gen == o2.s->generation());
    CHECK(o2.s->skipped(c) == (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{2, 3}}));
    acts = {{4, 1, Action::Kind::Send, "after the restore", {}, {}}};
    CHECK_OK(o2.s->apply(c, acts, r));
    Snap sn = o2.s->snapshot(1, 10);
    CHECK(texts(sn.entries) == "[before the backup][after the restore]");
    CHECK(sn.entries[0].gen == gen1 && sn.entries[1].gen == o2.s->generation());
    // a crash now is no restore
    o2.s.reset();
    restored.crash(restored.plan_all(false));
    Opened o3 = open_store(restored, kNow + 11);
    CHECK_OK(o3.st);
    CHECK(!o3.report.restored && o3.s->generation() == kNow + 10);
    o3.s.reset();

    // The pre-restore store, longer than the current one, copied back in: another generation, so detected too.
    MemFs again = restored;
    again.restore_tree(fs, "root/store");
    Opened o4 = open_store(again, kNow + 20);
    CHECK_OK(o4.st);
    CHECK(o4.report.restored && o4.s->generation() == kNow + 20);
    o4.s.reset();

    // An empty store directory (the store lost, the server record kept): a restore of nothing.
    MemFs empty;
    MemFs lost = fs;
    lost.restore_tree(empty, "root/store");
    Opened o5 = open_store(lost, kNow + 30);
    CHECK_OK(o5.st);
    CHECK(o5.report.restored && o5.s->generation() == kNow + 30 && o5.s->seen(c) == 2);
    o5.s.reset();

    // The server record lost, the store kept: the store cannot be vouched for, so a new generation.
    MemFs norecord = fs;
    norecord.restore_tree(empty, "root/server");
    Opened o6 = open_store(norecord, kNow + 40);
    CHECK_OK(o6.st);
    CHECK(o6.report.restored && o6.s->generation() == kNow + 40 && o6.s->incarnation() == kNow + 40);
    o6.s.reset();

    // The whole disk put back (the server record with the store): the incarnation still goes forward, by the clock.
    // The generation does not change; that limit is stated in store.h.
    Opened o7 = open_store(backup, kNow + 50);
    CHECK_OK(o7.st);
    CHECK(!o7.report.restored && o7.s->incarnation() == kNow + 50 && o7.s->generation() == gen1);
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"codec", test_codec},     {"memfs", test_memfs}, {"journal", test_journal},
    {"journal_crash", test_journal_crash}, {"store", test_store}, {"store_crash", test_store_crash},
    {"blob", test_blob},       {"session", test_session}, {"restore", test_restore},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0)
            continue;
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "store_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "store_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("store_test: ok\n");
    return 0;
}
