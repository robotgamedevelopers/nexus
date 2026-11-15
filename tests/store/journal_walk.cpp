// Drives the real nexus::store::Journal on MemFs with a seeded random walk and writes the trace as a TLA+ module
// (JournalTraceData.tla) for models/store/JournalTrace.tla, under which TLC checks that the trace is a behaviour of
// models/store/Journal.tla.
//
//   journal_walk <seed> <steps> [--fault accept-torn | cut-too-far | no-truncate | no-sync] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from the
// clock or the OS. Each step appends a record (1 to 8 bytes of payload), syncs and acknowledges what is durable,
// compacts (a snapshot of 0 to 8 bytes), crashes at once, or arms a crash at one of the next few filesystem calls and
// then appends and syncs or compacts, so that crashes land between every pair of calls, compaction's included. A
// crash takes a plan drawn from the seed (MemFs::plan: each pending directory change kept or not, each changed file
// reverted, cut short or damaged); the journal is then opened again, and a crash is sometimes armed inside that open
// too. Every filesystem call the journal makes that succeeds is a record of the trace, with its file and byte count
// (a write to the log, which holds every record appended since the last sync, also with each record's length: the
// spec's AppendRec once per record, bound to the one write); so are the recovery's decision (from Journal::Observer), each crash with its plan, and each acknowledgement. Each
// record carries the size of every file in the directory after it, and the highest number acknowledged.
//
// --fault walks the journal with a test-only fault (Journal::Fault); TLC must reject its trace.

#include "store/fs.h"
#include "store/journal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nexus::store::Bytes;
using nexus::store::Fs;
using nexus::store::Journal;
using nexus::store::MemFs;
using nexus::store::Rng;
using nexus::store::Status;

const char* kDir = "j";
const std::uint64_t kTooBig = 1000000000;

struct Step {
    std::string op;
    std::string name;
    std::uint64_t n = 0;
    std::vector<std::uint64_t> lens;    // a write to the log: each record's length
    std::uint64_t k = 0, s = 0, cut = 0;
    std::vector<bool> keep;
    std::map<std::string, MemFs::FileFate> fates;
    std::map<std::string, std::uint64_t> sizes;
    std::uint64_t acked = 0;
};

struct Walk {
    MemFs mem;
    std::vector<Step> steps;
    std::uint64_t acked = 0;
    std::set<std::uint64_t> rec_lens;
    std::set<std::uint64_t> snap_lens;

    void push(Step st) {
        st.sizes = mem.sizes(kDir);
        st.acked = acked;
        steps.push_back(std::move(st));
    }
};

// The lengths of the whole records in a write to the log, header included: a batch is several records back to back.
// Whatever is left at the end that is not a whole record is one length of its own, which the spec has no record for.
std::vector<std::uint64_t> record_lens(std::span<const std::uint8_t> d) {
    std::vector<std::uint64_t> out;
    std::uint64_t at = 0;
    while (d.size() - at >= Journal::kRecordHeader) {
        std::uint64_t len = Journal::kRecordHeader + nexus::store::get_u32(d.data() + at);
        if (len > d.size() - at) {
            break;
        }
        out.push_back(len);
        at += len;
    }
    if (at < d.size()) {
        out.push_back(d.size() - at);
    }
    return out;
}

// The filesystem the journal sees: MemFs, with every successful mutating call recorded as a step.
class RecordingFs : public Fs {
public:
    explicit RecordingFs(Walk& w) : w_(w) {}
    Status open_append(const std::string& p, std::unique_ptr<nexus::store::AppendFile>& out) override {
        std::unique_ptr<nexus::store::AppendFile> inner;
        Status s = w_.mem.open_append(p, inner);
        out.reset();
        if (s.is_ok()) {
            out = std::make_unique<Recording>(*this, p, std::move(inner));
        }
        return s;
    }
    // Not the journal's: the store takes its lock (Store::open) before the journal opens.
    Status lock(const std::string& p, nexus::store::LockMode m, std::unique_ptr<nexus::store::FileLock>& o) override {
        return w_.mem.lock(p, m, o);
    }
    Status make_dir(const std::string& d) override { return w_.mem.make_dir(d); }
    Status list(const std::string& d, std::vector<std::string>& n) override { return w_.mem.list(d, n); }
    Status size(const std::string& p, std::uint64_t& o) override { return w_.mem.size(p, o); }
    Status read(const std::string& p, std::uint64_t off, std::uint64_t n, Bytes& o) override {
        return w_.mem.read(p, off, n, o);
    }
    Status create(const std::string& p) override { return note(w_.mem.create(p), "create", p, 0); }
    Status append(const std::string& p, std::span<const std::uint8_t> d) override {
        Status s = w_.mem.append(p, d);
        std::vector<std::uint64_t> lens;
        if (nexus::store::name_of(p) == "log") {
            lens = record_lens(d);
        }
        if (s.is_ok()) {
            if (nexus::store::name_of(p) == "log") {
                w_.rec_lens.insert(lens.begin(), lens.end());
            } else {
                w_.snap_lens.insert(d.size());
            }
        }
        return note(s, "append", p, d.size(), lens);
    }
    Status truncate(const std::string& p, std::uint64_t n) override { return note(w_.mem.truncate(p, n), "truncate", p, n); }
    Status sync(const std::string& p) override { return note(w_.mem.sync(p), "sync", p, 0); }
    Status sync_dir(const std::string& d) override { return note(w_.mem.sync_dir(d), "syncdir", d + "/.", 0); }
    Status rename(const std::string& f, const std::string& t) override { return note(w_.mem.rename(f, t), "rename", f, 0); }
    Status remove(const std::string& p) override { return note(w_.mem.remove(p), "remove", p, 0); }
    // Not the journal's: the store never writes a file this way.
    Status write_new(const std::string& p, std::span<const std::uint8_t> d) override { return w_.mem.write_new(p, d); }
    Status private_dir(const std::string& d) override { return w_.mem.private_dir(d); }

private:
    // The log, open: one write of a batch is one step with each record's length (the spec's AppendRec, once per
    // record), and its flush is a sync of the log.
    class Recording : public nexus::store::AppendFile {
    public:
        Recording(RecordingFs& fs, std::string path, std::unique_ptr<nexus::store::AppendFile> inner)
            : fs_(fs), path_(std::move(path)), inner_(std::move(inner)) {}
        Status append(std::span<const std::uint8_t> d) override {
            Status s = inner_->append(d);
            std::vector<std::uint64_t> lens = record_lens(d);
            if (s.is_ok()) {
                fs_.w_.rec_lens.insert(lens.begin(), lens.end());
            }
            return fs_.note(s, "append", path_, d.size(), lens);
        }
        Status sync() override { return fs_.note(inner_->sync(), "sync", path_, 0); }

    private:
        RecordingFs& fs_;
        std::string path_;
        std::unique_ptr<nexus::store::AppendFile> inner_;
    };

    Status note(Status s, const char* op, const std::string& path, std::uint64_t n,
                std::vector<std::uint64_t> lens = {}) {
        if (s.is_ok()) {
            Step st;
            st.op = op;
            st.name = nexus::store::name_of(path);
            if (st.name == ".")
                st.name = "";
            st.n = n;
            st.lens = std::move(lens);
            w_.push(std::move(st));
        }
        return s;
    }
    Walk& w_;
};

class Recorder : public Journal::Observer {
public:
    explicit Recorder(Walk& w) : w_(w) {}
    void recovered(std::uint64_t k, std::uint64_t s, std::uint64_t cut) override {
        Step st;
        st.op = "recover";
        // TLC's integers stop at 2^31 - 1; a faulty journal can report a record number read from damaged bytes. Any
        // number past the walk's reach is written as kTooBig, which the spec never decides either.
        st.k = std::min<std::uint64_t>(k, kTooBig);
        st.s = s;
        st.cut = cut;
        w_.push(std::move(st));
    }

private:
    Walk& w_;
};

Bytes payload(Rng& rng, std::uint64_t lo, std::uint64_t hi) {
    Bytes b(std::size_t(lo + rng.below(hi - lo + 1)));
    for (auto& c : b)
        c = std::uint8_t(rng.below(256));
    return b;
}

void crash(Walk& w, Rng& rng) {
    MemFs::CrashPlan plan = w.mem.plan(rng);
    Step st;
    st.op = "crash";
    st.keep = plan.dir_keep[kDir];
    std::string prefix = std::string(kDir) + "/";
    for (const auto& [path, fate] : plan.files) {
        if (path.compare(0, prefix.size(), prefix) == 0)
            st.fates[path.substr(prefix.size())] = fate;
    }
    w.mem.crash(plan);
    w.push(std::move(st));
}

// Opens the journal after a crash, crashing again inside open now and then, until it opens.
bool reopen(Walk& w, Journal& j, Rng& rng) {
    for (int tries = 0; tries < 100; ++tries) {
        if (rng.below(4) == 0)
            w.mem.fail_at(1 + rng.below(4));
        Journal::Recovery rec;
        Status s = j.open(rec);
        w.mem.fail_at(0);
        if (s.is_ok())
            return true;
        if (!w.mem.crashed()) {
            std::fprintf(stderr, "journal_walk: open refused the disk: %s\n", s.message().c_str());
            return false;
        }
        crash(w, rng);
    }
    std::fprintf(stderr, "journal_walk: open did not succeed in 100 tries\n");
    return false;
}

void ack(Walk& w, Journal& j) {
    if (j.durable_seq() > w.acked) {
        w.acked = j.durable_seq();
        Step st;
        st.op = "ack";
        st.n = w.acked;
        w.push(std::move(st));
    }
}

std::string fmt_fn(const std::map<std::string, std::uint64_t>& m) {
    if (m.empty())
        return "<<>>";
    std::ostringstream o;
    bool first = true;
    for (const auto& [k, v] : m) {
        o << (first ? "" : " @@ ") << "(\"" << k << "\" :> " << v << ")";
        first = false;
    }
    return o.str();
}

std::string fmt_set(const std::set<std::uint64_t>& s) {
    std::ostringstream o;
    o << "{";
    bool first = true;
    for (std::uint64_t v : s) {
        o << (first ? "" : ", ") << v;
        first = false;
    }
    o << "}";
    return o.str();
}

std::string fmt_step(const Step& st) {
    std::ostringstream o;
    o << "[op |-> \"" << st.op << "\", name |-> \"" << st.name << "\", n |-> " << st.n << ", lens |-> <<";
    for (std::size_t i = 0; i < st.lens.size(); ++i) {
        o << (i ? ", " : "") << st.lens[i];
    }
    o << ">>, k |-> " << st.k
      << ", s |-> " << st.s << ", cut |-> " << st.cut << ", keep |-> <<";
    for (std::size_t i = 0; i < st.keep.size(); ++i)
        o << (i ? ", " : "") << (st.keep[i] ? "TRUE" : "FALSE");
    o << ">>, fates |-> ";
    if (st.fates.empty()) {
        o << "<<>>";
    } else {
        bool first = true;
        for (const auto& [name, f] : st.fates) {
            o << (first ? "" : " @@ ") << "(\"" << name << "\" :> [revert |-> " << (f.revert ? "TRUE" : "FALSE")
              << ", keep |-> " << f.keep << ", bf |-> " << f.bad_from << ", bt |-> " << f.bad_to << "])";
            first = false;
        }
    }
    o << ", sizes |-> " << fmt_fn(st.sizes) << ", acked |-> " << st.acked << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        std::fprintf(stderr, "usage: journal_walk <seed> <steps> [--fault accept-torn|cut-too-far|no-truncate|no-sync] "
                             "<out.tla>\n");
        return 2;
    }
    std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    std::uint64_t count = std::strtoull(argv[2], nullptr, 10);
    Journal::Fault fault = Journal::Fault::None;
    if (argc == 6) {
        std::string f = argv[4];
        if (std::strcmp(argv[3], "--fault") != 0) {
            std::fprintf(stderr, "journal_walk: expected --fault, got %s\n", argv[3]);
            return 2;
        }
        if (f == "accept-torn")
            fault = Journal::Fault::AcceptTorn;
        else if (f == "cut-too-far")
            fault = Journal::Fault::CutTooFar;
        else if (f == "no-truncate")
            fault = Journal::Fault::NoTruncate;
        else if (f == "no-sync")
            fault = Journal::Fault::NoSync;
        else {
            std::fprintf(stderr, "journal_walk: unknown fault %s\n", f.c_str());
            return 2;
        }
    }
    const char* out_path = argv[argc - 1];

    Walk w;
    RecordingFs fs(w);
    Recorder obs(w);
    Journal j(fs, kDir, fault, &obs);
    Rng rng(seed);
    Journal::Recovery rec;
    if (!j.open(rec).is_ok()) {
        std::fprintf(stderr, "journal_walk: the first open failed\n");
        return 1;
    }
    w.steps.clear();
    w.rec_lens.clear();
    Step init;
    init.op = "init";
    w.push(init);

    // A faulty journal can leave a disk its own open refuses (a gap, a log behind its snapshot): the walk stops there
    // and the trace so far, which holds the faulty step, goes to TLC.
    bool stopped = false;
    std::uint64_t crashes = 0, compactions = 0, torn = 0, longest = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t r = rng.below(100);
        bool armed = false;
        if (r >= 88) {
            armed = true;
            r = rng.below(2) == 0 ? 0 : 70;
            // An append and sync is two calls (the batch's write, its flush): the crash lands on one of them, so that
            // a written batch is often torn by the crash before its flush. A compaction is up to nine.
            w.mem.fail_at(1 + rng.below(r == 0 ? 2 : 9));
        }
        if (r < 50) {
            j.append(payload(rng, 1, 8));
            if (armed && j.sync().is_ok())
                ack(w, j);
        } else if (r < 70) {
            if (j.sync().is_ok())
                ack(w, j);
        } else if (r < 78) {
            if (j.compact(payload(rng, 0, 8)).is_ok())
                ++compactions;
        } else {
            crash(w, rng);
            ++crashes;
            if (!reopen(w, j, rng)) {
                if (fault == Journal::Fault::None)
                    return 1;
                stopped = true;
                break;
            }
            continue;
        }
        w.mem.fail_at(0);
        if (w.mem.crashed() || j.failed()) {
            crash(w, rng);
            ++crashes;
            if (!reopen(w, j, rng)) {
                if (fault == Journal::Fault::None)
                    return 1;
                stopped = true;
                break;
            }
        }
        longest = std::max<std::uint64_t>(longest, j.log_bytes());
    }
    for (const Step& st : w.steps) {
        if (st.op == "recover" && st.cut < st.sizes.at("log"))
            ++torn;
    }

    std::ofstream out(out_path);
    out << "---------------------------- MODULE JournalTraceData ----------------------------\n";
    out << "\\* Written by tests/store/journal_walk " << seed << " " << count << " (fault " << int(fault) << ").\n";
    out << "EXTENDS TLC\n";
    out << "TraceRecLens == " << fmt_set(w.rec_lens) << "\n";
    out << "TraceSnapLens == " << fmt_set(w.snap_lens) << "\n";
    out << "Trace == <<\n";
    for (std::size_t i = 0; i < w.steps.size(); ++i)
        out << "    " << fmt_step(w.steps[i]) << (i + 1 < w.steps.size() ? ",\n" : "\n");
    out << ">>\n";
    out << "=============================================================================\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "journal_walk: could not write %s\n", out_path);
        return 1;
    }
    std::printf("journal_walk: %zu trace steps, %llu crashes, %llu torn tails cut, %llu compactions, last record %llu, "
                "longest log %llu bytes%s\n",
                w.steps.size(), (unsigned long long)crashes, (unsigned long long)torn,
                (unsigned long long)compactions, (unsigned long long)j.last_seq(), (unsigned long long)longest,
                stopped ? "; stopped where the faulty journal refused its own disk" : "");
    return 0;
}
