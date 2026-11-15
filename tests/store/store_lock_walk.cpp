// Drives nexus-server's start-up order (src/server/start_order.h, the real StartOrder) and the real store
// (src/store/store.cpp Store::open, its lock through Fs::lock on MemFs) for several processes on one store, with a
// seeded random walk, and writes the trace as a TLA+ module (StoreLockTraceData.tla) for
// models/store/StoreLockTrace.tla, under which TLC checks that it is a behaviour of models/store/StoreLock.tla.
//
//   store_lock_walk <seed> <steps> [--faulty-unlocked | --faulty-restore-first] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from the
// clock or the OS. The store starts with one session and one prompt queued (one record). Each step picks a process:
// one that is down starts (a new StartOrder), one that is starting takes the step its StartOrder gives, one that runs
// commits a step, and any process dies now and then (its Store destroyed, which releases its lock on MemFs, as the end
// of a process does; its socket no longer answers). The steps:
//   probe    the control socket, which the walk keeps (who holds it): Held when another holds it
//   open     Store::open on the shared MemFs, through a filesystem that records the lock it takes (op lock, with
//            whether it was had) before anything else of the store is touched; Busy is Held
//   listen   the control socket: Held when another holds it, else this process's
//   restore  the drivers restored, as the server does: when the queued prompt is still in this process's store, a
//            dequeue (the prompt taken for its driver) and a send counted; else the driver's state committed
//   write    a running server's step, committed
// Every record carries, after it: each process's state (from its StartOrder), how many hold the store's lock (MemFs),
// who holds the socket, the record numbers in the log as they lie on the MemFs, and the sends so far.
//
// --faulty-unlocked walks StartOrder::Fault::Unlocked (the order before the lock: the store opened with no lock), and
// --faulty-restore-first StartOrder::Fault::RestoreFirst; TLC must reject both traces.

#include "server/start_order.h"
#include "store/codec.h"
#include "store/fs.h"
#include "store/store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nexus::server::StartOrder;
using nexus::store::Bytes;
using nexus::store::Change;
using nexus::store::FileLock;
using nexus::store::Fs;
using nexus::store::LockMode;
using nexus::store::MemFs;
using nexus::store::Rng;
using nexus::store::Status;
using nexus::store::Store;
using nexus::store::StoreOptions;

const char* kDir = "store";
constexpr std::size_t kProcs = 2;

struct Step {
    std::string op;
    std::size_t p = 0;
    bool ok = true;                     // lock: whether it was had
    std::vector<std::string> pcs;
    std::uint64_t holders = 0;
    int sock = -1;
    std::vector<std::uint64_t> seqs;
    std::uint64_t sent = 0;
};

struct Proc {
    bool up = false;
    std::unique_ptr<StartOrder> order;
    std::unique_ptr<Fs> fs;
    std::unique_ptr<Store> store;
};

struct Walk {
    MemFs mem;
    StartOrder::Fault fault = StartOrder::Fault::None;
    Proc procs[kProcs];
    int sock = -1;
    std::uint64_t sent = 0;
    std::vector<Step> steps;

    // The spec's name for where p is: what its StartOrder gives next, read by the order the fault selects.
    std::string pc(std::size_t p) const {
        const Proc& q = procs[p];
        if (!q.up) {
            return "down";
        }
        switch (q.order->next()) {
        case StartOrder::Step::Probe:
            return "start";
        case StartOrder::Step::Open:
            return "probed";
        case StartOrder::Step::OpenUnlocked:
            return "start";
        case StartOrder::Step::Listen:
            return fault == StartOrder::Fault::None ? "open" : "restored";
        case StartOrder::Step::Restore:
            return fault == StartOrder::Fault::None ? "listening" : "open";
        case StartOrder::Step::Serve:
            return "running";
        case StartOrder::Step::Refused:
            return "refused";
        case StartOrder::Step::Failed:
            return "failed";
        }
        return "failed";
    }

    // The record numbers in the log, as they lie on the disk.
    std::vector<std::uint64_t> seqs() const {
        std::vector<std::uint64_t> out;
        const std::string log = std::string(kDir) + "/log";
        if (!mem.has(log)) {
            return out;
        }
        const Bytes& b = mem.data(log);
        std::size_t at = 0;
        while (b.size() - at >= 16) {
            const std::uint32_t n = nexus::store::get_u32(b.data() + at);
            out.push_back(nexus::store::get_u64(b.data() + at + 8));
            at += 16 + n;
            if (at > b.size()) {
                break;
            }
        }
        return out;
    }

    void push(Step st) {
        for (std::size_t p = 0; p < kProcs; ++p) {
            st.pcs.push_back(pc(p));
        }
        st.holders = mem.lock_holders(std::string(kDir) + "/" + nexus::store::kLockName, LockMode::Exclusive);
        st.sock = sock;
        st.seqs = seqs();
        st.sent = sent;
        steps.push_back(std::move(st));
    }
};

// The filesystem one process sees: the shared MemFs, with the lock it takes recorded as a step of that process.
class ProcFs : public Fs {
public:
    ProcFs(Walk& w, std::size_t p) : w_(w), p_(p) {}
    Status lock(const std::string& path, LockMode m, std::unique_ptr<FileLock>& o) override {
        Status s = w_.mem.lock(path, m, o);
        Step st;
        st.op = "lock";
        st.p = p_;
        st.ok = s.is_ok();
        w_.push(std::move(st));
        return s;
    }
    Status open_append(const std::string& p, std::unique_ptr<nexus::store::AppendFile>& o) override {
        return w_.mem.open_append(p, o);
    }
    Status make_dir(const std::string& d) override { return w_.mem.make_dir(d); }
    Status list(const std::string& d, std::vector<std::string>& n) override { return w_.mem.list(d, n); }
    Status size(const std::string& p, std::uint64_t& o) override { return w_.mem.size(p, o); }
    Status read(const std::string& p, std::uint64_t off, std::uint64_t n, Bytes& o) override {
        return w_.mem.read(p, off, n, o);
    }
    Status create(const std::string& p) override { return w_.mem.create(p); }
    Status append(const std::string& p, std::span<const std::uint8_t> d) override { return w_.mem.append(p, d); }
    Status truncate(const std::string& p, std::uint64_t n) override { return w_.mem.truncate(p, n); }
    Status sync(const std::string& p) override { return w_.mem.sync(p); }
    Status sync_dir(const std::string& d) override { return w_.mem.sync_dir(d); }
    Status rename(const std::string& f, const std::string& t) override { return w_.mem.rename(f, t); }
    Status remove(const std::string& p) override { return w_.mem.remove(p); }
    Status write_new(const std::string& p, std::span<const std::uint8_t> d) override { return w_.mem.write_new(p, d); }
    Status private_dir(const std::string& d) override { return w_.mem.private_dir(d); }

private:
    Walk& w_;
    std::size_t p_;
};

Change driverChange(std::uint64_t n) {
    Change c;
    c.kind = Change::Kind::SetDriver;
    c.driver.next_turn = n;
    return c;
}

// One step of process p, as its StartOrder or its state gives. False when the walk cannot go on: a step the code
// failed (the store refused, which a faulty order can bring about), and the reason on standard error.
bool take(Walk& w, std::size_t p, bool* stopped) {
    Proc& q = w.procs[p];
    Step st;
    st.p = p;
    const StartOrder::Step next = q.order->next();
    StartOrder::Outcome outcome = StartOrder::Outcome::Ok;
    if (next == StartOrder::Step::Probe) {
        st.op = "probe";
        outcome = w.sock >= 0 ? StartOrder::Outcome::Held : StartOrder::Outcome::Ok;
    } else if (next == StartOrder::Step::Open || next == StartOrder::Step::OpenUnlocked) {
        StoreOptions options;
        options.test_unlocked = next == StartOrder::Step::OpenUnlocked;
        q.fs = std::make_unique<ProcFs>(w, p);
        Status s = Store::open(*q.fs, kDir, options, q.store);
        if (s.code() == Status::Code::Busy) {
            q.order->done(StartOrder::Outcome::Held);
            return true;        // the lock step, refused, is the record
        }
        if (!s.is_ok()) {
            std::fprintf(stderr, "store_lock_walk: open refused the store: %s\n", s.message().c_str());
            *stopped = true;
            return false;
        }
        st.op = "open";
    } else if (next == StartOrder::Step::Listen) {
        st.op = "listen";
        if (w.sock >= 0) {
            outcome = StartOrder::Outcome::Held;
        } else {
            w.sock = int(p);
        }
    } else if (next == StartOrder::Step::Restore) {
        st.op = "restore";
        const nexus::store::Session* s = q.store->session(1);
        Status c;
        if (s != nullptr && !s->queue.empty()) {
            Change d;
            d.kind = Change::Kind::Dequeue;
            c = q.store->commit(1, std::span<const Change>(&d, 1));
            if (c.is_ok()) {
                ++w.sent;
            }
        } else {
            Change d = driverChange(q.store->journal().last_seq());
            c = q.store->commit(1, std::span<const Change>(&d, 1));
        }
        if (!c.is_ok()) {
            std::fprintf(stderr, "store_lock_walk: restore's commit refused: %s\n", c.message().c_str());
            *stopped = true;
            return false;
        }
    } else if (next == StartOrder::Step::Serve) {
        st.op = "write";
        Change d = driverChange(q.store->journal().last_seq());
        Status c = q.store->commit(1, std::span<const Change>(&d, 1));
        if (!c.is_ok()) {
            std::fprintf(stderr, "store_lock_walk: a step's commit refused: %s\n", c.message().c_str());
            *stopped = true;
            return false;
        }
        w.push(std::move(st));
        return true;
    } else {
        std::fprintf(stderr, "store_lock_walk: a start failed\n");
        *stopped = true;
        return false;
    }
    q.order->done(outcome);
    w.push(std::move(st));
    return true;
}

void die(Walk& w, std::size_t p) {
    Proc& q = w.procs[p];
    q.store.reset();
    q.fs.reset();
    q.order.reset();
    q.up = false;
    if (w.sock == int(p)) {
        w.sock = -1;
    }
    Step st;
    st.op = "die";
    st.p = p;
    w.push(std::move(st));
}

std::string quoted(const std::string& s) {
    return "\"" + s + "\"";
}

std::string fmt_step(const Step& st) {
    std::ostringstream o;
    o << "[op |-> " << quoted(st.op) << ", p |-> \"p" << (st.p + 1) << "\", ok |-> " << (st.ok ? "TRUE" : "FALSE")
      << ", pcs |-> <<";
    for (std::size_t i = 0; i < st.pcs.size(); ++i) {
        o << (i ? ", " : "") << quoted(st.pcs[i]);
    }
    o << ">>, holders |-> " << st.holders << ", sock |-> ";
    if (st.sock < 0) {
        o << "{}";
    } else {
        o << "{\"p" << (st.sock + 1) << "\"}";
    }
    o << ", seqs |-> <<";
    for (std::size_t i = 0; i < st.seqs.size(); ++i) {
        o << (i ? ", " : "") << st.seqs[i];
    }
    o << ">>, sent |-> " << st.sent << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: store_lock_walk <seed> <steps> [--faulty-unlocked | --faulty-restore-first] "
                             "<out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const std::uint64_t count = std::strtoull(argv[2], nullptr, 10);
    Walk w;
    if (argc == 5) {
        if (std::strcmp(argv[3], "--faulty-unlocked") == 0) {
            w.fault = StartOrder::Fault::Unlocked;
        } else if (std::strcmp(argv[3], "--faulty-restore-first") == 0) {
            w.fault = StartOrder::Fault::RestoreFirst;
        } else {
            std::fprintf(stderr, "store_lock_walk: unknown fault %s\n", argv[3]);
            return 2;
        }
    }
    const char* out_path = argv[argc - 1];

    // The store before the walk: one session, one prompt queued, one record.
    {
        std::unique_ptr<Store> s;
        if (!Store::open(w.mem, kDir, StoreOptions{}, s).is_ok()) {
            std::fprintf(stderr, "store_lock_walk: the first open failed\n");
            return 1;
        }
        Change info;
        info.kind = Change::Kind::SetInfo;
        info.info.friend_name = "ada";
        info.info.harness = "claude";
        Change queued;
        queued.kind = Change::Kind::Enqueue;
        queued.prompt = "queued";
        const Change both[] = {info, queued};
        if (!s->commit(1, std::span<const Change>(both, 2)).is_ok()) {
            std::fprintf(stderr, "store_lock_walk: the first commit failed\n");
            return 1;
        }
    }
    Step init;
    init.op = "init";
    w.push(init);

    Rng rng(seed);
    bool stopped = false;
    std::uint64_t starts = 0, refused = 0, deaths = 0, writes = 0;
    for (std::uint64_t i = 0; i < count && !stopped; ++i) {
        const std::size_t p = std::size_t(rng.below(kProcs));
        Proc& q = w.procs[p];
        if (!q.up) {
            q.up = true;
            q.order = std::make_unique<StartOrder>(w.fault);
            ++starts;
            Step st;
            st.op = "start";
            st.p = p;
            w.push(std::move(st));
            continue;
        }
        const StartOrder::Step next = q.order->next();
        // A refused start only ends; any process dies now and then; a running server mostly writes.
        if (next == StartOrder::Step::Refused) {
            ++refused;
            ++deaths;
            die(w, p);
            continue;
        }
        if (rng.below(next == StartOrder::Step::Serve ? 6 : 10) == 0) {
            ++deaths;
            die(w, p);
            continue;
        }
        if (next == StartOrder::Step::Serve) {
            ++writes;
        }
        if (!take(w, p, &stopped)) {
            break;
        }
    }
    if (stopped && w.fault == StartOrder::Fault::None) {
        return 1;
    }

    std::ofstream out(out_path);
    out << "---------------------------- MODULE StoreLockTraceData ----------------------------\n";
    out << "\\* Written by tests/store/store_lock_walk " << seed << " " << count << " (fault " << int(w.fault) << ").\n";
    out << "Trace == <<\n";
    for (std::size_t i = 0; i < w.steps.size(); ++i) {
        out << "    " << fmt_step(w.steps[i]) << (i + 1 < w.steps.size() ? ",\n" : "\n");
    }
    out << ">>\n";
    out << "=============================================================================\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "store_lock_walk: could not write %s\n", out_path);
        return 1;
    }
    std::printf("store_lock_walk: %zu trace steps, %llu starts, %llu refused, %llu deaths, %llu writes, %llu sends%s\n",
                w.steps.size(), (unsigned long long)starts, (unsigned long long)refused, (unsigned long long)deaths,
                (unsigned long long)writes, (unsigned long long)w.sent,
                stopped ? "; stopped where the faulty order's store refused" : "");
    return 0;
}
