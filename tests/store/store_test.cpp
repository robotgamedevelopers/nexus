// Unit tests of the server's durable state (src/store/). Everything runs on MemFs, in memory: no time, no sockets, no
// network, no files. Crashes are injected at every mutating filesystem call of each scenario, and each crash is taken
// through several plans (MemFs::plan), so that every write and sync point is a crash point.
//
//   store_test            runs every group
//   store_test <group>    runs one: codec memfs journal journal_crash store ids store_crash driver migrate dequeue
//                         formats

#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "store/store.h"
#include "fixtures/main_store.h"

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

// What a session is: with `friend_name`, the harness "claude" and the given settings.
Change info(const std::string& friend_name, const std::string& model = std::string()) {
    Change c;
    c.kind = Change::Kind::SetInfo;
    c.info = Info{friend_name, "claude", "", model, ""};
    return c;
}

// The changes of a step, with the session's info first: the step that makes it.
std::vector<Change> made(const std::string& friend_name, std::vector<Change> rest = {}) {
    rest.insert(rest.begin(), info(friend_name));
    return rest;
}

std::string texts(const Session* t) {
    if (t == nullptr)
        return "-";
    std::string out;
    for (const Entry& e : t->transcript)
        out += "[" + std::to_string(e.kind) + ":" + e.text + (e.closed ? "" : "+") + "]";
    out += "|";
    for (const std::string& q : t->queue)
        out += "[" + q + "]";
    const Driver& d = t->driver;
    if (d.next_turn != 1 || d.open_turn != 0 || !d.session.empty() || !d.waiting.empty()) {
        out += "|turn " + std::to_string(d.next_turn) + " open " + std::to_string(d.open_turn) + " session " + d.session;
        for (const std::string& p : d.waiting)
            out += "<" + p + ">";
    }
    return out;
}

std::string about(const Info& i) {
    return i.friend_name + "/" + i.harness + "/" + i.account + "/" + i.model + "/" + i.effort;
}

// A whole-state digest, to compare a recovered store with the states a scenario passed through.
std::string digest(const Store& s, const std::vector<std::string>& clients) {
    std::string d = "next " + std::to_string(s.next_session()) + " ";
    for (const auto& [id, t] : s.sessions()) {
        d += std::to_string(id) + "(" + about(t.info) + ")=" + texts(&t) + " ";
    }
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
    CHECK(s.session(1) == nullptr && s.applied("w1") == 0 && s.next_session() == 1);

    // a session made by its first step, here with a send: the prompt and the reply, with the window's counter
    std::vector<Change> send = {append("hello", 0), append("(received)", 1)};
    CHECK_OK(s.commit(1, made("ada", send), "w1", 1));
    CHECK(texts(s.session(1)) == "[0:hello][1:(received)]|");
    CHECK(about(s.session(1)->info) == "ada/claude///");
    CHECK(s.applied("w1") == 1 && s.next_session() == 2);
    std::vector<Change> queue = {enqueue("later")};
    CHECK_OK(s.commit(1, queue, "w1", 2));
    CHECK_OK(s.commit(2, made("bo", {append("open", 1, false)})));  // no client: the server's own change
    CHECK(texts(s.session(2)) == "[1:open+]|");

    // a session is made only at the next id, and only by a step whose first change is its info
    CHECK(s.commit(4, made("cy")).code() == Status::Code::Refused);
    CHECK(s.commit(3, std::vector<Change>{append("x")}).code() == Status::Code::Refused);
    CHECK(s.commit(3, std::vector<Change>{append("x"), info("cy")}).code() == Status::Code::Refused);
    CHECK(s.session(3) == nullptr && s.next_session() == 3);
    // a held session's info is replaced whole, and is not a new session
    CHECK_OK(s.commit(2, std::vector<Change>{info("bo", "fast")}));
    CHECK(about(s.session(2)->info) == "bo/claude//fast/" && s.next_session() == 3);
    CHECK(texts(s.session(2)) == "[1:open+]|");

    // exactly once: an action applied before, or one past a gap, is refused and changes nothing
    Status again = s.commit(1, send, "w1", 2);
    CHECK(again.code() == Status::Code::Refused);
    CHECK(again.message().find("next action is 3, not 2") != std::string::npos);
    CHECK(s.commit(1, send, "w1", 4).code() == Status::Code::Refused);
    CHECK(texts(s.session(1)) == "[0:hello][1:(received)]|[later]");
    // a refused action (a limit the server keeps) still counts: the counter moves, no session changes
    CHECK_OK(s.commit(0, std::vector<Change>{}, "w1", 3));
    CHECK(s.applied("w1") == 3 && s.sessions().size() == 2);
    CHECK_OK(s.commit(1, std::vector<Change>{append("A notice.", 2)}, "w2", 1));
    CHECK(s.applied("w2") == 1 && s.applied("w1") == 3);
    CHECK(s.commit(0, std::vector<Change>{append("x")}).code() == Status::Code::Refused);
    CHECK(s.commit(1, std::vector<Change>{}).code() == Status::Code::Refused);

    // durable: a crash that keeps nothing volatile loses nothing committed, the next id included
    std::string before = digest(s, {"w1", "w2"});
    o.s.reset();
    fs.crash(fs.plan_all(false));
    Opened o2 = open_store(fs);
    CHECK_OK(o2.st);
    CHECK(!o2.report.fresh && o2.report.records == 6 && o2.report.cut_bytes == 0 && o2.report.migrated == 0);
    CHECK(digest(*o2.s, {"w1", "w2"}) == before);
    CHECK_OK(o2.s->commit(1, send, "w1", 4));
    CHECK(o2.s->next_session() == 3);

    // compaction: a small threshold compacts often; the state, counters and next id included, survives
    StoreOptions small;
    small.compact_bytes = 300;
    MemFs fs2;
    Opened c = open_store(fs2, small);
    CHECK_OK(c.st);
    CHECK_OK(c.s->commit(1, made("ada")));
    CHECK_OK(c.s->commit(2, made("bo")));
    for (std::uint64_t i = 1; i <= 40; ++i)
        CHECK_OK(c.s->commit(i % 3 == 0 ? 2 : 1, std::vector<Change>{append("message " + std::to_string(i)),
                                                                     enqueue("q")}, "w", i));
    CHECK(c.s->journal().log_bytes() <= 300);
    CHECK(c.s->journal().last_seq() == 42);
    std::string compacted = digest(*c.s, {"w"});
    c.s.reset();
    fs2.crash(fs2.plan_all(false));
    Opened c2 = open_store(fs2, small);
    CHECK_OK(c2.st);
    CHECK(digest(*c2.s, {"w"}) == compacted);
    CHECK(c2.s->applied("w") == 40 && c2.s->session(1)->transcript.size() == 27 && c2.s->next_session() == 3);

    // a record of a later format that declares it needs a reader of a later format (byte 2, ' ', 32): refused at
    // open, saying what to do
    MemFs fs3;
    {
        Journal j(fs3, "store");
        Journal::Recovery rec;
        CHECK_OK(j.open(rec));
        CHECK_OK(j.append(span_of("\x09 from a newer server")));
        CHECK_OK(j.sync());
    }
    Opened bad = open_store(fs3);
    CHECK(bad.st.code() == Status::Code::Failed);
    CHECK(bad.st.message().find("record 1 of its journal is in format 9") != std::string::npos);
    CHECK(bad.st.message().find("run the newer nexus-server") != std::string::npos);

    // a failed write: the step is not taken, and every later one is refused until the store is opened again
    MemFs fs4;
    Opened f = open_store(fs4);
    CHECK_OK(f.st);
    fs4.fail_at(2);  // the sync
    CHECK(f.s->commit(1, made("ada", send), "w1", 1).code() == Status::Code::Failed);
    CHECK(f.s->failed() && f.s->session(1) == nullptr && f.s->applied("w1") == 0 && f.s->next_session() == 1);
    fs4.crash(fs4.plan_all(true));
    Status later = f.s->commit(1, made("ada", send), "w1", 1);
    CHECK(later.code() == Status::Code::Failed && later.message().find("restart the server") != std::string::npos);
}

// Ids are never reused: the next id is durable, across a crash, a compaction and a reopen, and a snapshot that names
// an id at or past its next id, or a next id of 0, is not one this store writes.
void test_ids() {
    StoreOptions small;
    small.compact_bytes = 150;
    MemFs fs;
    {
        Opened o = open_store(fs, small);
        CHECK_OK(o.st);
        for (std::uint64_t id = 1; id <= 3; ++id) {
            CHECK_OK(o.s->commit(id, made(id == 2 ? "bo" : "ada")));   // two sessions with ada
        }
        for (int i = 0; i < 10; ++i) {
            CHECK_OK(o.s->commit(3, std::vector<Change>{append("filler " + std::to_string(i))}));
        }
        CHECK(o.s->journal().log_bytes() <= 150);
        CHECK(o.s->sessions().size() == 3 && o.s->next_session() == 4);
        CHECK(o.s->session(1)->info.friend_name == "ada" && o.s->session(3)->info.friend_name == "ada");
    }
    fs.crash(fs.plan_all(false));
    {
        Opened o = open_store(fs, small);
        CHECK_OK(o.st);
        CHECK(o.s->next_session() == 4);
        // an info to a held session replaces what it is, and makes no new session
        CHECK_OK(o.s->commit(3, made("cy")));
        CHECK(o.s->session(3)->info.friend_name == "cy" && o.s->next_session() == 4);
        CHECK_OK(o.s->commit(4, made("dee")));
        CHECK(o.s->next_session() == 5);
    }
    fs.crash(fs.plan_all(false));
    {
        Opened o = open_store(fs, small);
        CHECK_OK(o.st);
        CHECK(o.s->next_session() == 5 && o.s->session(4)->info.friend_name == "dee");
    }
    // snapshots this store never writes
    auto snapshot_with = [](std::uint64_t next, std::uint64_t id) {
        Writer w;
        w.u8(3);
        w.u64(next);
        w.u32(1);
        w.u64(id);
        for (int i = 0; i < 5; ++i) {
            w.str("x");
        }
        w.u32(0);
        w.u32(0);
        w.u64(1);
        w.u64(0);
        w.str("");
        w.u32(0);
        w.u32(0);
        return w.out();
    };
    for (auto [next, id] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 1}, {2, 2}, {2, 0}, {3, 1}}) {
        MemFs b;
        {
            Opened first = open_store(b);
            CHECK_OK(first.st);
            CHECK_OK(first.s->commit(1, made("ada")));
        }
        {
            Journal j(b, "store");
            Journal::Recovery rec;
            CHECK_OK(j.open(rec));
            CHECK_OK(j.compact(snapshot_with(next, id)));
        }
        Opened o = open_store(b);
        CHECK((o.st.is_ok()) == (next == 3 && id == 1));
    }
}

void test_store_crash() {
    // A scenario of two sessions made, sends, queues, refusals as counter-only steps and two windows, long enough to
    // compact. Each step's digest is recorded on a run without crashes; after a crash at any point the store must show
    // the digest of some step at or after the last one committed.
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
        for (std::uint64_t id = 1; id <= 2; ++id) {
            if (!s.commit(id, made(id == 1 ? "ada" : "bo")).is_ok()) {
                return;
            }
            if (reference) {
                digests.push_back(digest(s, clients));
            }
            ++acked;
        }
        std::uint64_t num[2] = {0, 0};
        for (int i = 0; i < 14; ++i) {
            std::size_t w = std::size_t(i % 3 == 2);
            std::vector<Change> ch;
            std::uint64_t id = i % 2 == 0 ? 1 : 2;
            if (i % 5 == 4) {
                id = 0;  // refused by the server's limits: only the counter moves
            } else if (i % 4 == 3) {
                ch.push_back(enqueue("q" + std::to_string(i)));
            } else {
                ch.push_back(append("p" + std::to_string(i), 0));
                ch.push_back(append("(received)", 1));
            }
            if (!s.commit(id, ch, clients[w], ++num[w]).is_ok()) {
                return;
            }
            if (reference)
                digests.push_back(digest(s, clients));
            ++acked;
        }
    };
    {
        MemFs ref;
        scenario(ref);
        reference = false;
        CHECK(acked == 16 && digests.size() == 17);
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

Change setDriver(std::uint64_t next, std::uint64_t open, const std::string& session,
                 std::vector<std::string> waiting = {}) {
    Change c;
    c.kind = Change::Kind::SetDriver;
    c.driver = Driver{next, open, session, std::move(waiting)};
    return c;
}

// A session's driver state: replaced whole in the same step as the entries beside it, durable across a crash and
// across compaction.
void test_driver() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    // a send: the prompt, and the turn it was written as, in one step with the window's counter
    CHECK_OK(o.s->commit(1, made("ada", {append("hello", 0), setDriver(2, 1, "", {})}), "w1", 1));
    CHECK_OK(o.s->commit(1, std::vector<Change>{append("hi back", 1), setDriver(2, 0, "s-1", {"next"})}));
    CHECK_OK(o.s->commit(1, std::vector<Change>{setDriver(3, 2, "s-1", {"a", "b"}), setDriver(3, 2, "s-2", {})}));
    CHECK(texts(o.s->session(1)) == "[0:hello][1:hi back]||turn 3 open 2 session s-2");
    const std::string before = digest(*o.s, {"w1"});
    o.s.reset();
    fs.crash(fs.plan_all(false));
    Opened o2 = open_store(fs);
    CHECK_OK(o2.st);
    CHECK(digest(*o2.s, {"w1"}) == before);

    // compaction writes the driver state into the snapshot
    StoreOptions small;
    small.compact_bytes = 200;
    MemFs fs2;
    Opened c = open_store(fs2, small);
    CHECK_OK(c.st);
    CHECK_OK(c.s->commit(1, made("ada")));
    for (std::uint64_t i = 1; i <= 20; ++i)
        CHECK_OK(c.s->commit(1, std::vector<Change>{append("m" + std::to_string(i)),
                                                    setDriver(i + 1, i, "s-" + std::to_string(i), {"w"})}));
    CHECK(c.s->journal().log_bytes() <= 200);
    const std::string compacted = digest(*c.s, {});
    c.s.reset();
    fs2.crash(fs2.plan_all(false));
    Opened c2 = open_store(fs2, small);
    CHECK_OK(c2.st);
    CHECK(digest(*c2.s, {}) == compacted);
    CHECK(c2.s->session(1)->driver == (Driver{21, 20, "s-20", {"w"}}));
}

// ---- the older formats, keyed by friend tab ----

void put_legacy_tab(Writer& w, const std::string& name, const std::vector<std::string>& entries,
                    const std::vector<std::string>& queue, bool driver) {
    w.str(name);
    w.u32(std::uint32_t(entries.size()));
    for (const std::string& e : entries) {
        w.u8(0);
        w.u8(1);
        w.str(e);
    }
    w.u32(std::uint32_t(queue.size()));
    for (const std::string& q : queue) {
        w.str(q);
    }
    if (driver) {
        w.u64(4);
        w.u64(0);
        w.str("s-" + name);
        w.u32(0);
    }
}

// A record of format 1: its tab by name, then the changes (appends of person entries, or enqueues), the client and
// its counter.
Bytes legacy_record(const std::string& tab, const std::vector<std::string>& appends,
                    const std::vector<std::string>& enqueues, const std::string& client, std::uint64_t num) {
    Writer w;
    w.u8(1);
    w.str(tab);
    w.u32(std::uint32_t(appends.size() + enqueues.size()));
    for (const std::string& a : appends) {
        w.u8(1);
        w.u8(0);
        w.u8(1);
        w.str(a);
    }
    for (const std::string& e : enqueues) {
        w.u8(2);
        w.str(e);
    }
    w.str(client);
    w.u64(num);
    return w.out();
}

// A store as an older server left it: a snapshot of `snapshot` (written over a first step, which it stands for) and
// the records after it.
MemFs legacy_store(const Bytes& snapshot, const std::vector<Bytes>& records) {
    MemFs fs;
    {
        Opened first = open_store(fs);
        CHECK_OK(first.st);
        CHECK_OK(first.s->commit(1, made("old")));
    }
    Journal j(fs, "store");
    Journal::Recovery rec;
    CHECK_OK(j.open(rec));
    if (!snapshot.empty()) {
        CHECK_OK(j.compact(snapshot));
    }
    for (const Bytes& r : records) {
        CHECK_OK(j.append(r));
    }
    CHECK_OK(j.sync());
    return fs;
}

// A friend-tab store opens as sessions: each tab a session with its friend and the legacy harness, transcript, queue
// and driver intact; the snapshot's tabs get ids 1, 2, ... in name order, a tab a record names first the next id;
// clients' counters kept. It is compacted into the sessions format at open, and reads the same from then on, with
// nothing migrated again; the next id goes on from there.
void test_migrate() {
    Writer snap;
    snap.u8(2);
    snap.u32(2);
    put_legacy_tab(snap, "ada", {"one", "two"}, {"queued"}, true);
    put_legacy_tab(snap, "bo", {"hi"}, {}, true);
    snap.u32(2);
    snap.str("w1");
    snap.u64(7);
    snap.str("w2");
    snap.u64(2);
    MemFs fs = legacy_store(snap.out(), {legacy_record("bo", {"later"}, {}, "w1", 8),
                                         legacy_record("cy", {"new"}, {"q"}, "w2", 3),
                                         legacy_record("", {}, {}, "w1", 9),
                                         legacy_record("ada", {"three"}, {}, "", 0)});
    StoreOptions options;
    options.legacy_harness = "claude";
    std::string after;
    {
        Opened o = open_store(fs, options);
        CHECK_OK(o.st);
        CHECK(o.report.migrated == 3);
        const Store& s = *o.s;
        CHECK(s.sessions().size() == 3 && s.next_session() == 4);
        CHECK(about(s.session(1)->info) == "ada/claude///" && about(s.session(2)->info) == "bo/claude///" &&
              about(s.session(3)->info) == "cy/claude///");
        CHECK(texts(s.session(1)) == "[0:one][0:two][0:three]|[queued]|turn 4 open 0 session s-ada");
        CHECK(texts(s.session(2)) == "[0:hi][0:later]||turn 4 open 0 session s-bo");
        CHECK(texts(s.session(3)) == "[0:new]|[q]");
        CHECK(s.applied("w1") == 9 && s.applied("w2") == 3);
        // compacted at open: the snapshot is of this format, the log empty
        CHECK(s.journal().log_bytes() == 0);
        after = digest(s, {"w1", "w2"});
    }
    {
        Journal j(fs, "store");
        Journal::Recovery rec;
        CHECK_OK(j.open(rec));
        CHECK(!rec.snapshot.empty() && rec.snapshot[0] == 4 && rec.records.empty());
    }
    fs.crash(fs.plan_all(false));
    {
        Opened o = open_store(fs, options);
        CHECK_OK(o.st);
        CHECK(o.report.migrated == 0);
        CHECK(digest(*o.s, {"w1", "w2"}) == after);
        CHECK_OK(o.s->commit(4, made("ada"), "w1", 10));     // a second session with ada: the next id
        CHECK(o.s->next_session() == 5);
    }

    // records alone (no snapshot): each tab the next id in the order the records name them
    MemFs logs = legacy_store(Bytes{}, {legacy_record("zed", {"z"}, {}, "w1", 1),
                                        legacy_record("amy", {"a"}, {}, "w1", 2),
                                        legacy_record("zed", {"z2"}, {}, "w1", 3)});
    {
        Opened o = open_store(logs);
        CHECK_OK(o.st);
        // the first step of legacy_store made session 1 in this format; the tabs follow it
        CHECK(o.report.migrated == 2 && o.s->next_session() == 4);
        CHECK(o.s->session(2)->info.friend_name == "zed" && texts(o.s->session(2)) == "[0:z][0:z2]|");
        CHECK(o.s->session(3)->info.friend_name == "amy" && o.s->applied("w1") == 3);
    }

    // a snapshot of format 1 (before drivers): read, every session's driver at its start
    Writer one;
    one.u8(1);
    one.u32(1);
    put_legacy_tab(one, "ada", {"old"}, {"queued"}, false);
    one.u32(1);
    one.str("w1");
    one.u64(1);
    MemFs fs1 = legacy_store(one.out(), {});
    {
        Opened o = open_store(fs1);
        CHECK_OK(o.st);
        CHECK(o.report.migrated == 1);
        CHECK(texts(o.s->session(1)) == "[0:old]|[queued]");
        CHECK(o.s->session(1)->driver == Driver{});
        CHECK(o.s->applied("w1") == 1);
        CHECK_OK(o.s->commit(1, std::vector<Change>{setDriver(2, 1, "")}, "w1", 2));
        CHECK(texts(o.s->session(1)) == "[0:old]|[queued]|turn 2 open 1 session ");
    }

    // a crash at any point of the migration's compaction leaves a store that opens to the same sessions
    const MemFs unmigrated = legacy_store(one.out(), {});
    auto opens = [](MemFs& f) { Opened o = open_store(f); };
    std::uint64_t taken = at_every_point(unmigrated, opens, [&](MemFs& f, std::uint64_t) {
        Opened o = open_store(f);
        CHECK_OK(o.st);
        if (o.st.is_ok()) {
            CHECK(texts(o.s->session(1)) == "[0:old]|[queued]");
            CHECK(o.s->next_session() == 2 && o.s->applied("w1") == 1);
        }
    });
    CHECK(taken > 5);

    // a format-1 record with changes but no tab is not one this store writes
    MemFs nameless = legacy_store(Bytes{}, {legacy_record("", {"x"}, {}, "w1", 1)});
    Opened bad = open_store(nameless);
    CHECK(bad.st.code() == Status::Code::Failed);
}

Change dequeue() {
    Change c;
    c.kind = Change::Kind::Dequeue;
    return c;
}

void test_dequeue() {
    MemFs fs;
    Opened o = open_store(fs);
    CHECK_OK(o.st);
    CHECK_OK(o.s->commit(1, made("ada")));
    // a dequeue from an empty queue, or one more than the step leaves, is refused and changes nothing
    Status none = o.s->commit(1, std::vector<Change>{dequeue()});
    CHECK(none.code() == Status::Code::Refused);
    CHECK(none.message().find("a dequeue from a queue with nothing left in it") != std::string::npos);
    CHECK(o.s->commit(1, std::vector<Change>{enqueue("one"), dequeue(), dequeue()}, "w1", 1).code() ==
          Status::Code::Refused);
    CHECK(texts(o.s->session(1)) == "|" && o.s->applied("w1") == 0);
    // queued, then taken at a turn's end with the person's entry, in one step
    CHECK_OK(o.s->commit(1, std::vector<Change>{enqueue("one")}, "w1", 1));
    CHECK_OK(o.s->commit(1, std::vector<Change>{enqueue("two")}, "w1", 2));
    CHECK_OK(o.s->commit(1, std::vector<Change>{dequeue(), append("one", 0)}));
    CHECK(texts(o.s->session(1)) == "[0:one]|[two]");
    // replayed after a crash as it was: each taken once
    const std::string before = digest(*o.s, {"w1"});
    o.s.reset();
    fs.crash(fs.plan_all(false));
    Opened o2 = open_store(fs);
    CHECK_OK(o2.st);
    CHECK(digest(*o2.s, {"w1"}) == before);
    CHECK_OK(o2.s->commit(1, std::vector<Change>{dequeue(), append("two", 0)}));
    CHECK(texts(o2.s->session(1)) == "[0:one][0:two]|");
    // and kept by compaction
    StoreOptions small;
    small.compact_bytes = 120;
    MemFs fs2;
    Opened c = open_store(fs2, small);
    CHECK_OK(c.st);
    CHECK_OK(c.s->commit(1, made("ada")));
    for (int i = 0; i < 10; ++i) {
        CHECK_OK(c.s->commit(1, std::vector<Change>{enqueue("q" + std::to_string(i))}));
        CHECK_OK(c.s->commit(1, std::vector<Change>{dequeue(), append("q" + std::to_string(i), 0)}));
    }
    CHECK(c.s->session(1)->queue.empty() && c.s->session(1)->transcript.size() == 10);
    const std::string compacted = digest(*c.s, {});
    c.s.reset();
    fs2.crash(fs2.plan_all(false));
    Opened c2 = open_store(fs2, small);
    CHECK_OK(c2.st);
    CHECK(digest(*c2.s, {}) == compacted);

    // a journaled dequeue from an empty queue is not one this store writes: the store does not open
    for (int format = 1; format <= 2; ++format) {
        MemFs fs3;
        {
            Opened first = open_store(fs3);
            CHECK_OK(first.st);
            CHECK_OK(first.s->commit(1, made("ada")));
        }
        {
            Writer w;
            w.u8(std::uint8_t(format));
            if (format == 1) {
                w.str("ada");
            } else {
                w.u64(1);
            }
            w.u32(1);
            w.u8(4);
            w.str("");
            w.u64(0);
            Journal j(fs3, "store");
            Journal::Recovery rec;
            CHECK_OK(j.open(rec));
            CHECK_OK(j.append(w.out()));
            CHECK_OK(j.sync());
        }
        Opened bad = open_store(fs3);
        CHECK(bad.st.code() == Status::Code::Failed);
        CHECK(bad.st.message().find("record") != std::string::npos);
    }
}

// A store of `records` alone, journaled as they are (no snapshot).
MemFs journaled(const std::vector<Bytes>& records, const Bytes& snapshot = Bytes{}) {
    MemFs fs;
    Journal j(fs, "store");
    Journal::Recovery rec;
    CHECK_OK(j.open(rec));
    if (!snapshot.empty()) {
        CHECK_OK(j.append(Bytes{0}));       // a record for the snapshot to stand for
        CHECK_OK(j.compact(snapshot));
    }
    for (const Bytes& r : records) {
        CHECK_OK(j.append(r));
    }
    CHECK_OK(j.sync());
    return fs;
}

template <std::size_t N>
Bytes fixture(const std::uint8_t (&bytes)[N]) {
    return Bytes(bytes, bytes + N);
}

// What main's steps leave (fixtures/main_store_fixture.cpp).
void check_main_steps(const Store& s) {
    CHECK(s.session(1) != nullptr && about(s.session(1)->info) == "ada/claude/work/opus/high");
    CHECK(texts(s.session(1)) ==
          "[0:Why did the nightly go red?][1:Looking at the log.][2:Stopped.]||turn 2 open 0 session s-1");
    CHECK(s.applied("w1") == 2 && s.next_session() == 2);
}

// A record of a later format than this build's (5), written the way a framed format is: `reader` the oldest format its
// reader must know; a change of a kind this build does not know (9), an entry of a kind it does not know (7) with a
// field after its text, and a field after the record's counter.
Bytes later_record(std::uint8_t reader) {
    Writer w;
    w.u8(5);
    w.u8(reader);
    w.u64(1);
    w.u32(3);
    w.u8(9);
    {
        Writer frame;
        frame.str("a change a later format adds");
        w.bytes(frame.out());
    }
    w.u8(std::uint8_t(Change::Kind::Append));
    {
        Writer entry;
        entry.u8(7);
        entry.u8(1);
        entry.str("from a later server");
        entry.u64(42);              // a later field of an entry
        Writer frame;
        frame.bytes(entry.out());
        w.bytes(frame.out());
    }
    w.u8(std::uint8_t(Change::Kind::Enqueue));
    {
        Writer frame;
        frame.str("later");
        frame.u32(7);               // a later field of a change
        w.bytes(frame.out());
    }
    w.str("w1");
    w.u64(3);
    w.str("a later field of the record");
    return w.out();
}

// Across versions (store.h, docs/formats.md): main's stores (records of format 2, a snapshot of format 3) are read and
// go on in this format; an unframed record holding a kind its format does not have is refused, saying so; a record or
// snapshot of a later format is read, past what it adds, unless it declares a reader this build is not.
void test_formats() {
    // main's two records, then a step of this build's, then reopened
    {
        MemFs fs = journaled({fixture(kMainRecord2First), fixture(kMainRecord2Second)});
        Opened o = open_store(fs);
        CHECK_OK(o.st);
        if (o.s != nullptr) {
            check_main_steps(*o.s);
            CHECK_OK(o.s->commit(1, std::vector<Change>{append("ls -la", 3)}, "w1", 3));
            o.s.reset();
            Opened o2 = open_store(fs);
            CHECK_OK(o2.st);
            CHECK(o2.s != nullptr && texts(o2.s->session(1)).find("[3:ls -la]|") != std::string::npos);
        }
    }
    // main's snapshot
    {
        MemFs fs = journaled({}, fixture(kMainSnapshot3));
        Opened o = open_store(fs);
        CHECK_OK(o.st);
        if (o.s != nullptr) {
            check_main_steps(*o.s);
        }
    }
    // a format 2 record with an entry of kind 3, which format 2 never had: refused, naming the record and the kind
    {
        Writer w;
        w.u8(2);
        w.u64(1);
        w.u32(1);
        w.u8(std::uint8_t(Change::Kind::Append));
        w.u8(3);
        w.u8(1);
        w.str("tool");
        w.str("w1");
        w.u64(3);
        MemFs fs = journaled({fixture(kMainRecord2First), fixture(kMainRecord2Second), w.out()});
        Opened o = open_store(fs);
        CHECK(o.st.code() == Status::Code::Failed);
        CHECK(o.st.message().find("record 3 of its journal (an entry of kind 3, which format 2 does not have") !=
              std::string::npos);
    }
    // a later format this build can read: what it adds is skipped, the unknown entry kind kept as it is
    {
        MemFs fs = journaled({fixture(kMainRecord2First), fixture(kMainRecord2Second), later_record(3)});
        Opened o = open_store(fs);
        CHECK_OK(o.st);
        if (o.s != nullptr) {
            CHECK(texts(o.s->session(1)) == "[0:Why did the nightly go red?][1:Looking at the log.][2:Stopped.]"
                                            "[7:from a later server]|[later]|turn 2 open 0 session s-1");
            CHECK(o.s->applied("w1") == 3);
        }
    }
    // a later format that declares this build cannot read it: refused, saying which nexus-server can
    {
        MemFs fs = journaled({fixture(kMainRecord2First), fixture(kMainRecord2Second), later_record(5)});
        Opened o = open_store(fs);
        CHECK(o.st.code() == Status::Code::Failed);
        CHECK(o.st.message().find("record 3 of its journal is in format 5") != std::string::npos);
        CHECK(o.st.message().find("run the newer nexus-server") != std::string::npos);
    }
    // a later snapshot format this build can read: a later field of a session, of an entry and of the snapshot
    // skipped; one that declares a later reader refused
    for (std::uint8_t reader : {std::uint8_t(4), std::uint8_t(6)}) {
        Writer w;
        w.u8(6);
        w.u8(reader);
        w.u64(2);
        w.u32(1);
        Writer session;
        session.u64(1);
        for (const char* s : {"ada", "claude", "", "", ""}) {
            session.str(s);
        }
        session.u32(1);
        {
            Writer entry;
            entry.u8(3);
            entry.u8(1);
            entry.str("ls");
            entry.str("a later field of an entry");
            session.bytes(entry.out());
        }
        session.u32(0);
        session.u64(1);
        session.u64(0);
        session.str("");
        session.u32(0);
        session.str("a later field of a session");
        w.bytes(session.out());
        w.u32(1);
        w.str("w1");
        w.u64(4);
        w.str("a later field of the snapshot");
        MemFs fs = journaled({}, w.out());
        Opened o = open_store(fs);
        if (reader == 4) {
            CHECK_OK(o.st);
            CHECK(o.s != nullptr && texts(o.s->session(1)) == "[3:ls]|" && o.s->applied("w1") == 4);
        } else {
            CHECK(o.st.code() == Status::Code::Failed);
            CHECK(o.st.message().find("its snapshot is in format 6") != std::string::npos);
        }
    }
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
    {"ids", test_ids},
    {"driver", test_driver},
    {"migrate", test_migrate},
    {"dequeue", test_dequeue},
    {"formats", test_formats},
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
