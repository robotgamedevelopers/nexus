// Unit tests of the server's durable state (src/store/). Everything runs on MemFs, in memory: no time, no sockets, no
// network, no files. Crashes are injected at every mutating filesystem call of each scenario, and each crash is taken
// through several plans (MemFs::plan), so that every write and sync point is a crash point.
//
//   store_test            runs every group
//   store_test <group>    runs one: codec memfs journal journal_crash store store_crash

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

Bytes bytes_of(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

std::span<const std::uint8_t> span_of(const std::string& s) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// ---- codec ----

void test_codec() {
    CHECK(crc32c(span_of("123456789")) == 0xE3069283u);
    CHECK(crc32c(span_of("")) == 0);
    CHECK(crc32c(span_of("6789"), crc32c(span_of("12345"))) == 0xE3069283u);

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

Opened open_store(MemFs& fs, StoreOptions options = {}) {
    Opened o;
    o.st = Store::open(fs, "store", options, o.s, &o.report);
    return o;
}

Change append(const std::string& text, std::uint8_t kind = 0, bool closed = true) {
    Change c;
    c.kind = Change::Kind::Append;
    c.entry = Entry{kind, text, closed};
    return c;
}

Change enqueue(const std::string& prompt) {
    Change c;
    c.kind = Change::Kind::Enqueue;
    c.prompt = prompt;
    return c;
}

std::string texts(const Tab* t) {
    if (t == nullptr)
        return "-";
    std::string out;
    for (const Entry& e : t->transcript)
        out += "[" + std::to_string(e.kind) + ":" + e.text + (e.closed ? "" : "+") + "]";
    out += "|";
    for (const std::string& q : t->queue)
        out += "[" + q + "]";
    return out;
}

// A whole-state digest, to compare a recovered store with the states a scenario passed through.
std::string digest(const Store& s, const std::vector<std::string>& clients) {
    std::string d;
    for (const auto& [name, t] : s.tabs())
        d += name + "=" + texts(&t) + " ";
    for (const std::string& c : clients)
        d += c + ":" + std::to_string(s.applied(c)) + " ";
    return d;
}

void test_store() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    CHECK(o.report.fresh);
    Store& s = *o.s;
    CHECK(s.tab("ada") == nullptr && s.applied("w1") == 0);

    // a send: the prompt and the reply, with the window's counter, in one step
    std::vector<Change> send = {append("hello", 0), append("(received)", 1)};
    CHECK_OK(s.commit("ada", send, "w1", 1));
    CHECK(texts(s.tab("ada")) == "[0:hello][1:(received)]|");
    CHECK(s.applied("w1") == 1);
    std::vector<Change> queue = {enqueue("later")};
    CHECK_OK(s.commit("ada", queue, "w1", 2));
    CHECK_OK(s.commit("bo", std::vector<Change>{append("open", 1, false)}));  // no client: the server's own change
    CHECK(texts(s.tab("bo")) == "[1:open+]|");

    // exactly once: an action applied before, or one past a gap, is refused and changes nothing
    Status again = s.commit("ada", send, "w1", 2);
    CHECK(again.code() == Status::Code::Refused);
    CHECK(again.message().find("next action is 3, not 2") != std::string::npos);
    CHECK(s.commit("ada", send, "w1", 4).code() == Status::Code::Refused);
    CHECK(texts(s.tab("ada")) == "[0:hello][1:(received)]|[later]");
    // a refused action (a limit the server keeps) still counts: the counter moves, no tab changes
    CHECK_OK(s.commit("", std::vector<Change>{}, "w1", 3));
    CHECK(s.applied("w1") == 3 && s.tabs().size() == 2);
    CHECK_OK(s.commit("ada", std::vector<Change>{append("A notice.", 2)}, "w2", 1));
    CHECK(s.applied("w2") == 1 && s.applied("w1") == 3);
    CHECK(s.commit("", std::vector<Change>{append("x")}).code() == Status::Code::Refused);
    CHECK(s.commit("ada", std::vector<Change>{}).code() == Status::Code::Refused);

    // durable: a crash that keeps nothing volatile loses nothing committed
    std::string before = digest(s, {"w1", "w2"});
    o.s.reset();
    fs.crash(fs.plan_all(false));
    Opened o2 = open_store(fs);
    CHECK_OK(o2.st);
    CHECK(!o2.report.fresh && o2.report.records == 5 && o2.report.cut_bytes == 0);
    CHECK(digest(*o2.s, {"w1", "w2"}) == before);
    CHECK_OK(o2.s->commit("ada", send, "w1", 4));

    // compaction: a small threshold compacts often; the state, counters included, survives
    StoreOptions small;
    small.compact_bytes = 300;
    MemFs fs2;
    Opened c = open_store(fs2, small);
    CHECK_OK(c.st);
    for (std::uint64_t i = 1; i <= 40; ++i)
        CHECK_OK(c.s->commit(i % 3 == 0 ? "bo" : "ada",
                             std::vector<Change>{append("message " + std::to_string(i)), enqueue("q")}, "w", i));
    CHECK(c.s->journal().log_bytes() <= 300);
    CHECK(c.s->journal().last_seq() == 40);
    std::string compacted = digest(*c.s, {"w"});
    c.s.reset();
    fs2.crash(fs2.plan_all(false));
    Opened c2 = open_store(fs2, small);
    CHECK_OK(c2.st);
    CHECK(digest(*c2.s, {"w"}) == compacted);
    CHECK(c2.s->applied("w") == 40 && c2.s->tab("ada")->transcript.size() == 27);

    // a record the store cannot read: refused at open, saying what to do
    MemFs fs3;
    {
        Journal j(fs3, "store");
        Journal::Recovery rec;
        CHECK_OK(j.open(rec));
        CHECK_OK(j.append(span_of("\x02 from a newer server")));
        CHECK_OK(j.sync());
    }
    Opened bad = open_store(fs3);
    CHECK(bad.st.code() == Status::Code::Failed);
    CHECK(bad.st.message().find("record 1 is not one this store writes") != std::string::npos);

    // a failed write: the step is not taken, and every later one is refused until the store is opened again
    MemFs fs4;
    Opened f = open_store(fs4);
    CHECK_OK(f.st);
    fs4.fail_at(2);  // the sync
    CHECK(f.s->commit("ada", send, "w1", 1).code() == Status::Code::Failed);
    CHECK(f.s->failed() && f.s->tab("ada") == nullptr && f.s->applied("w1") == 0);
    fs4.crash(fs4.plan_all(true));
    Status later = f.s->commit("ada", send, "w1", 1);
    CHECK(later.code() == Status::Code::Failed && later.message().find("restart the server") != std::string::npos);
}

void test_store_crash() {
    // A scenario of sends, queues, refusals as counter-only steps and two windows, long enough to compact. Each
    // step's digest is recorded on a run without crashes; after a crash at any point the store must show the digest
    // of some step at or after the last one committed.
    StoreOptions options;
    options.compact_bytes = 250;
    std::vector<std::string> clients = {"w1", "w2"};
    std::vector<std::string> digests;
    std::size_t acked = 0;
    bool reference = true;
    auto scenario = [&](MemFs& fs) {
        acked = 0;
        Opened o = open_store(fs, options);
        if (!o.st.is_ok())
            return;
        Store& s = *o.s;
        if (reference)
            digests.push_back(digest(s, clients));
        std::uint64_t num[2] = {0, 0};
        for (int i = 0; i < 14; ++i) {
            std::size_t w = std::size_t(i % 3 == 2);
            std::vector<Change> ch;
            std::string tab = i % 2 == 0 ? "ada" : "bo";
            if (i % 5 == 4) {
                tab.clear();  // refused by the server's limits: only the counter moves
            } else if (i % 4 == 3) {
                ch.push_back(enqueue("q" + std::to_string(i)));
            } else {
                ch.push_back(append("p" + std::to_string(i), 0));
                ch.push_back(append("(received)", 1));
            }
            if (!s.commit(tab, ch, clients[w], ++num[w]).is_ok())
                return;
            if (reference)
                digests.push_back(digest(s, clients));
            ++acked;
        }
    };
    {
        MemFs ref;
        scenario(ref);
        reference = false;
        CHECK(acked == 14 && digests.size() == 15);
    }
    std::uint64_t taken = at_every_point(MemFs(), scenario, [&](MemFs& fs, std::uint64_t) {
        Opened o = open_store(fs, options);
        CHECK_OK(o.st);
        if (!o.st.is_ok())
            return;
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

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"codec", test_codec},
    {"memfs", test_memfs},
    {"journal", test_journal},
    {"journal_crash", test_journal_crash},
    {"store", test_store},
    {"store_crash", test_store_crash},
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
