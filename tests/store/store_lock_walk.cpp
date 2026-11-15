// Drives nexus-server's start (src/server/start_order.h: the real StartOrder, through the real take_step, the one
// dispatch Server::run takes its steps by) and the real store's lock on the real filesystem (src/store/posix_fs.cpp
// PosixFs::lock: flock on the store's directory, through Store::open) for two server processes and a reader on one
// store, with a seeded random walk, and writes the trace as a TLA+ module for models/store/StoreLockTrace.tla, under
// which TLC checks that it is a behaviour of models/store/StoreLock.tla.
//
//   store_lock_walk <seed> <steps> [--faulty-unlocked | --faulty-restore-first | --faulty-held-refused |
//                   --faulty-reader-busy | --faulty-no-gate] <out.tla>
//
// This is a functional test: the store is a real directory, <out.tla>.store beside the trace (emptied first), in the
// test's own directory, and its locks are the system's. Time is not: each process's clocks (StartSteps::now_ms for the
// wait on a held lock, PosixOptions::now_ms for the wait on readers) are counters the walk advances by each pause, so
// the walk is a pure function of its arguments (random numbers from the seed, splitmix64; nothing from the clock).
// The waits are short so a walk sees them end: readers are waited for kReaderWaitMs (three retries of kLockRetryMs,
// ReaderWait 3 in the trace instance), a writer's held lock kHeldWaitMs (three tries of kWaitRetryMs, HeldWait 3).
//
// The store starts with one session and one prompt queued (one record). Each step picks p1, p2 or the reader r1:
//   a writer down starts (a person), one that crashed (exit 1 or 3, or killed) is started again (the service); one
//   starting takes the step its StartOrder gives, through take_step and this walk's StartSteps:
//     probe    the control socket, which the walk keeps (whose file it is): Answered when that server serves (it is
//              running, and has not accepted `stop`)
//     open     Store::open through a filesystem that records its lock: the lock's tries while readers hold it (op
//              rwait, a pause during which the reader may let go, or a reader may try to start: refused at the
//              writer's gate, op rlock), then the lock (op lock: taken, held by a writer, or failed: readers that did
//              not let go), then the replay (op open)
//     wait     the pause and the probe of a start that found the lock held (op heldtick)
//     listen   the control socket: Answered when a server answers on it, else this process's file
//     restore  the drivers restored, as the server does: the queued prompt, when this process's store still has it
//              queued, dequeued and sent (counted); else the driver's state committed
//   a running server writes a step, or is stopped (op stopaccept: it answers nothing more, its socket still bound;
//   then op close: its socket closed and its file removed if it is still its own; then op end: its drivers' last
//   record, under the lock; then op exit: its lock released); a start that ended
//   exits (exit 0 when refused, 1 failed, 3 stuck); any process may die now and then (its Store destroyed, which
//   closes its descriptors: the system releases its lock; its socket's file stays, answering no one).
//   The reader takes the lock shared (op rlock: had when no writer holds it, else refused) and lets go (op rrelease).
// Every record carries, after it: each process's state, how many hold the store's lock exclusive and shared (asked of
// the system with flock on a descriptor of the walk's own), whose the socket's file is, the record numbers in the log
// as they lie on the disk, and the sends so far.
//
// The faults, each of which TLC must reject: --faulty-unlocked (StartOrder::Fault::Unlocked, the order before the
// lock), --faulty-restore-first (RestoreFirst), --faulty-held-refused (HeldRefused: a held lock taken as another
// server, exit 0), --faulty-reader-busy (readers that did not let go reported as a writer's lock: the start waits as
// if a writer held it), --faulty-no-gate (PosixOptions::test_no_gate, the build before the gate: a reader takes the lock
// while a writer waits on readers). A fault the walk never reached is an error (exit 1), so a rejection is the fault's.

#include "server/start_order.h"
#include "store/codec.h"
#include "store/fs.h"
#include "store/store.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nexus::server::StartOrder;
using nexus::server::StartSteps;
using nexus::store::Bytes;
using nexus::store::Change;
using nexus::store::FileLock;
using nexus::store::Fs;
using nexus::store::LockMode;
using nexus::store::PosixOptions;
using nexus::store::Rng;
using nexus::store::Status;
using nexus::store::Store;
using nexus::store::StoreOptions;

constexpr std::size_t kWriters = 2;
constexpr std::uint64_t kReaderWaitMs = 3 * nexus::store::kLockRetryMs;
constexpr std::uint64_t kHeldWaitMs = 3 * nexus::server::kWaitRetryMs;

enum class Fault { None, Unlocked, RestoreFirst, HeldRefused, ReaderBusy, NoGate };

struct Step {
    std::string op;
    std::string who;
    std::vector<std::string> pcs;
    std::uint64_t holders = 0;
    std::uint64_t readers = 0;
    std::string sock;
    std::vector<std::uint64_t> seqs;
    std::uint64_t sent = 0;
};

// A running server's phase, past its start.
enum class Phase { Running, Stopping, Closed, Ended };

struct Proc {
    bool up = false;
    bool crashed = false;
    bool bound = false;             // bound the socket in this run
    Phase phase = Phase::Running;
    std::string override_pc;        // the state while a step is part done (the lock had, the replay not yet)
    std::uint64_t clock = 0;        // StartSteps::now_ms
    std::uint64_t lock_clock = 0;   // PosixOptions::now_ms
    std::unique_ptr<StartOrder> order;
    std::unique_ptr<Fs> fs;
    std::unique_ptr<Store> store;
};

struct Walk;
void reader_release(Walk& w);

struct Walk {
    std::string dir;
    Fault fault = Fault::None;
    Rng rng{0};
    Proc procs[kWriters];
    std::unique_ptr<Fs> reader_fs;
    std::unique_ptr<FileLock> reader_lock;
    int sock = -1;                  // whose the socket's file is
    std::uint64_t sent = 0;
    std::uint64_t reached = 0;      // times the fault changed what happened
    std::uint64_t gave_way = 0;     // readers refused at the gate while a writer waited on readers
    std::vector<Step> steps;

    StartOrder::Fault order_fault() const {
        switch (fault) {
        case Fault::Unlocked:
            return StartOrder::Fault::Unlocked;
        case Fault::RestoreFirst:
            return StartOrder::Fault::RestoreFirst;
        case Fault::HeldRefused:
            return StartOrder::Fault::HeldRefused;
        case Fault::None:
        case Fault::ReaderBusy:
        case Fault::NoGate:
            break;
        }
        return StartOrder::Fault::None;
    }

    // The spec's name for where p is.
    std::string pc(std::size_t p) const {
        const Proc& q = procs[p];
        if (!q.up) {
            return q.crashed ? "crashed" : "down";
        }
        if (!q.override_pc.empty()) {
            return q.override_pc;
        }
        const bool late_listen = fault == Fault::Unlocked || fault == Fault::RestoreFirst;
        switch (q.order->next()) {
        case StartOrder::Step::Probe:
        case StartOrder::Step::OpenUnlocked:
            return "start";
        case StartOrder::Step::Open:
            return "probed";
        case StartOrder::Step::Wait:
            return "held";
        case StartOrder::Step::Listen:
            return late_listen ? "restored" : "open";
        case StartOrder::Step::Restore:
            return late_listen ? "open" : "listening";
        case StartOrder::Step::Serve:
            return q.phase == Phase::Running ? "running" : q.phase == Phase::Stopping ? "stopping" :
                   q.phase == Phase::Closed ? "closed" : "ended";
        case StartOrder::Step::Refused:
            return "refused";
        case StartOrder::Step::Stuck:
            return "stuck";
        case StartOrder::Step::Failed:
            return "failed";
        }
        return "failed";
    }

    // A server says it is serving on the socket: its file is a live server's in its serving loop, before it accepted
    // `stop` (the real question and answer, `status` and SERVING, are server_lock's: tests/link/server_lock.cpp
    // startDuringStop).
    bool answers() const {
        if (sock < 0) {
            return false;
        }
        return pc(std::size_t(sock)) == "running";
    }

    // Who holds the directory's lock, asked of the system on a descriptor of the walk's own: a shared lock refused means
    // a writer holds it; else an exclusive lock refused means readers do.
    void holders(std::uint64_t& exclusive, std::uint64_t& shared) const {
        exclusive = 0;
        shared = 0;
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
            exclusive = 1;
        } else {
            ::flock(fd, LOCK_UN);
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                shared = 1;
            } else {
                ::flock(fd, LOCK_UN);
            }
        }
        ::close(fd);
    }

    // The record numbers in the log, as they lie on the disk.
    std::vector<std::uint64_t> seqs() const {
        std::vector<std::uint64_t> out;
        std::ifstream in(dir + "/log", std::ios::binary);
        if (!in) {
            return out;
        }
        const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

    void push(const std::string& op, const std::string& who) {
        Step st;
        st.op = op;
        st.who = who;
        for (std::size_t p = 0; p < kWriters; ++p) {
            st.pcs.push_back(pc(p));
        }
        st.pcs.push_back(reader_lock ? "rheld" : "rdown");
        holders(st.holders, st.readers);
        st.sock = sock < 0 ? "none" : "p" + std::to_string(sock + 1);
        st.seqs = seqs();
        st.sent = sent;
        steps.push_back(std::move(st));
    }
};

std::string name(std::size_t p) {
    return "p" + std::to_string(p + 1);
}

bool reader_step(Walk& w);

void reader_release(Walk& w) {
    w.reader_lock.reset();
    w.push("rrelease", "r1");
}

// The filesystem one writer sees: the real one (PosixFs), with its lock's outcome kept for the walk, and under
// --faulty-reader-busy readers that did not let go reported as a writer's lock.
class ProcFs : public Fs {
public:
    ProcFs(Walk& w, std::unique_ptr<Fs> inner) : w_(w), inner_(std::move(inner)) {}

    bool tried = false;
    Status::Code result = Status::Code::Ok;

    Status lock(const std::string& dir, LockMode m, std::unique_ptr<FileLock>& o) override {
        Status s = inner_->lock(dir, m, o);
        if (w_.fault == Fault::ReaderBusy && s.code() == Status::Code::Failed &&
            s.message().find("held by readers only") != std::string::npos) {
            ++w_.reached;
            s = Status::busy(dir + " is locked by a writer (a reader taken as one: the walk's fault)");
        }
        tried = true;
        result = s.code();
        return s;
    }
    Status open_append(const std::string& p, std::unique_ptr<nexus::store::AppendFile>& o) override {
        return inner_->open_append(p, o);
    }
    Status make_dir(const std::string& d) override { return inner_->make_dir(d); }
    Status list(const std::string& d, std::vector<std::string>& n) override { return inner_->list(d, n); }
    Status size(const std::string& p, std::uint64_t& o) override { return inner_->size(p, o); }
    Status read(const std::string& p, std::uint64_t off, std::uint64_t n, Bytes& o) override {
        return inner_->read(p, off, n, o);
    }
    Status create(const std::string& p) override { return inner_->create(p); }
    Status append(const std::string& p, std::span<const std::uint8_t> d) override { return inner_->append(p, d); }
    Status truncate(const std::string& p, std::uint64_t n) override { return inner_->truncate(p, n); }
    Status sync(const std::string& p) override { return inner_->sync(p); }
    Status sync_dir(const std::string& d) override { return inner_->sync_dir(d); }
    Status rename(const std::string& f, const std::string& t) override { return inner_->rename(f, t); }
    Status remove(const std::string& p) override { return inner_->remove(p); }
    Status write_new(const std::string& p, std::span<const std::uint8_t> d) override {
        return inner_->write_new(p, d);
    }
    Status private_dir(const std::string& d) override { return inner_->private_dir(d); }

private:
    Walk& w_;
    std::unique_ptr<Fs> inner_;
};

Change driverChange(std::uint64_t n) {
    Change c;
    c.kind = Change::Kind::SetDriver;
    c.driver.next_turn = n;
    return c;
}

// The steps of writer p's start, for take_step: the walk's socket, the real store on the real filesystem.
class WalkSteps : public StartSteps {
public:
    WalkSteps(Walk& w, std::size_t p) : w_(w), p_(p) {}

    ProcFs* fs = nullptr;           // the open's filesystem, when the step was an open
    bool opened = false;

    StartOrder::Outcome probe(std::string*) override {
        return w_.answers() ? StartOrder::Outcome::Answered : StartOrder::Outcome::Ok;
    }

    StartOrder::Outcome open(bool unlocked, std::string* why) override {
        Proc& q = w_.procs[p_];
        q.store.reset();
        PosixOptions options;
        options.reader_wait_ms = kReaderWaitMs;
        options.test_no_gate = w_.fault == Fault::NoGate;
        options.now_ms = [&q] { return q.lock_clock; };
        Walk& w = w_;
        const std::size_t p = p_;
        options.sleep_ms = [&w, &q, p](std::uint64_t ms) {
            // A try refused by readers alone: the pause, during which the reader may let go. The writer is waiting on
            // readers until its lock call returns (start_step clears it).
            q.lock_clock += ms;
            q.override_pc = "readers";
            w.push("rwait", name(p));
            if (w.reader_lock && w.rng.below(3) == 0) {
                reader_release(w);
            }
            if (!w.reader_lock && w.rng.below(2) == 0) {
                // A reader starting while the writer waits on readers: the writer holds the gate, so it must give way
                // (refused, the lock not taken); one that took the lock is a step the spec cannot take.
                reader_step(w);
                if (!w.reader_lock) {
                    ++w.gave_way;
                } else if (w.fault == Fault::NoGate) {
                    ++w.reached;
                }
            }
        };
        auto made = std::make_unique<ProcFs>(w_, nexus::store::make_posix_fs(std::move(options)));
        fs = made.get();
        q.fs = std::move(made);
        StoreOptions so;
        so.test_unlocked = unlocked;
        const Status s = Store::open(*q.fs, w_.dir, so, q.store);
        if (s.code() == Status::Code::Busy) {
            return StartOrder::Outcome::Held;
        }
        if (!s.is_ok()) {
            if (why != nullptr) {
                *why = s.message();
            }
            return StartOrder::Outcome::Failed;
        }
        opened = true;
        return StartOrder::Outcome::Ok;
    }

    StartOrder::Outcome listen(std::string*) override {
        if (w_.answers()) {
            return StartOrder::Outcome::Answered;
        }
        w_.sock = int(p_);           // a stale file (a dead server's) removed, this one bound
        w_.procs[p_].bound = true;
        return StartOrder::Outcome::Ok;
    }

    StartOrder::Outcome restore(std::string* why) override {
        Proc& q = w_.procs[p_];
        const nexus::store::Session* s = q.store->session(1);
        Status c;
        bool sends = false;
        if (s != nullptr && !s->queue.empty()) {
            Change d;
            d.kind = Change::Kind::Dequeue;
            c = q.store->commit(1, std::span<const Change>(&d, 1));
            sends = true;
        } else {
            Change d = driverChange(q.store->journal().last_seq());
            c = q.store->commit(1, std::span<const Change>(&d, 1));
        }
        if (!c.is_ok()) {
            if (why != nullptr) {
                *why = c.message();
            }
            return StartOrder::Outcome::Failed;
        }
        if (sends) {
            ++w_.sent;
        }
        return StartOrder::Outcome::Ok;
    }

    void pause(std::uint64_t ms) override { w_.procs[p_].clock += ms; }
    std::uint64_t now_ms() override { return w_.procs[p_].clock; }

private:
    Walk& w_;
    std::size_t p_;
};

// Writer p's process ends (exit, or killed): its store and descriptors closed, which releases its lock.
void end_process(Walk& w, std::size_t p, bool crashed, const std::string& op) {
    Proc& q = w.procs[p];
    const bool failed = q.up && q.order->next() == StartOrder::Step::Failed;
    q.store.reset();
    q.fs.reset();
    if (op == "exit" && failed && q.bound && w.sock == int(p)) {
        w.sock = -1;                // a failed start removes its own socket's file
    }
    q.order.reset();
    q.up = false;
    q.crashed = crashed;
    q.bound = false;
    q.phase = Phase::Running;
    w.push(op, name(p));
}

// One step of a starting writer, by take_step. False when the walk cannot go on (the reason on standard error).
bool start_step(Walk& w, std::size_t p) {
    Proc& q = w.procs[p];
    WalkSteps steps(w, p);
    std::string why;
    const StartOrder::Step step = nexus::server::take_step(*q.order, steps, &why);
    q.override_pc.clear();
    switch (step) {
    case StartOrder::Step::Probe:
        w.push("probe", name(p));
        return true;
    case StartOrder::Step::Wait:
        w.push("heldtick", name(p));
        return true;
    case StartOrder::Step::Open:
    case StartOrder::Step::OpenUnlocked:
        if (steps.fs != nullptr && steps.fs->tried) {
            if (steps.fs->result == Status::Code::Ok) {
                q.override_pc = "locked";
                w.push("lock", name(p));
                q.override_pc.clear();
            } else {
                if (steps.fs->result == Status::Code::Busy && w.fault == Fault::HeldRefused) {
                    ++w.reached;
                }
                w.push("lock", name(p));
            }
        }
        if (steps.opened) {
            w.push("open", name(p));
        } else if (steps.fs == nullptr || !steps.fs->tried || steps.fs->result == Status::Code::Ok) {
            std::fprintf(stderr, "store_lock_walk: %s's open failed after its lock: %s\n", name(p).c_str(), why.c_str());
            return false;
        }
        return true;
    case StartOrder::Step::Listen:
        w.push("listen", name(p));
        return true;
    case StartOrder::Step::Restore:
        if (q.order->next() == StartOrder::Step::Failed) {
            std::fprintf(stderr, "store_lock_walk: %s's restore failed: %s\n", name(p).c_str(), why.c_str());
            return false;
        }
        w.push("restore", name(p));
        return true;
    case StartOrder::Step::Serve:
    case StartOrder::Step::Refused:
    case StartOrder::Step::Stuck:
    case StartOrder::Step::Failed:
        break;
    }
    std::fprintf(stderr, "store_lock_walk: take_step was given an ended start\n");
    return false;
}

// One step of writer p, as its state gives. False when the walk cannot go on.
bool writer_step(Walk& w, std::size_t p) {
    Proc& q = w.procs[p];
    if (!q.up) {
        const bool restart = q.crashed;
        q.up = true;
        q.crashed = false;
        q.bound = false;
        q.phase = Phase::Running;
        q.order = std::make_unique<StartOrder>(w.order_fault(), kHeldWaitMs);
        w.push(restart ? "restart" : "start", name(p));
        return true;
    }
    const StartOrder::Step next = q.order->next();
    // Any process dies now and then; a running server more rarely, and mostly writes.
    if (w.rng.below(next == StartOrder::Step::Serve ? 12 : 10) == 0) {
        end_process(w, p, true, "die");
        return true;
    }
    if (next == StartOrder::Step::Refused || next == StartOrder::Step::Stuck || next == StartOrder::Step::Failed) {
        end_process(w, p, next != StartOrder::Step::Refused, "exit");
        return true;
    }
    if (next != StartOrder::Step::Serve) {
        return start_step(w, p);
    }
    if (q.phase == Phase::Running) {
        if (w.rng.below(15) == 0) {
            // `nexus-server stop` accepted: the loop answers nothing more; its socket is still bound.
            q.phase = Phase::Stopping;
            w.push("stopaccept", name(p));
            return true;
        }
        Change d = driverChange(q.store->journal().last_seq());
        const Status c = q.store->commit(1, std::span<const Change>(&d, 1));
        if (!c.is_ok()) {
            std::fprintf(stderr, "store_lock_walk: a step's commit refused: %s\n", c.message().c_str());
            return false;
        }
        w.push("write", name(p));
        return true;
    }
    if (q.phase == Phase::Stopping) {
        // The socket closed, its file removed if it is still this server's.
        if (w.sock == int(p)) {
            w.sock = -1;
        }
        q.phase = Phase::Closed;
        w.push("close", name(p));
        return true;
    }
    if (q.phase == Phase::Closed) {
        Change d = driverChange(q.store->journal().last_seq());
        const Status c = q.store->commit(1, std::span<const Change>(&d, 1));
        if (!c.is_ok()) {
            std::fprintf(stderr, "store_lock_walk: the drivers' last record refused: %s\n", c.message().c_str());
            return false;
        }
        q.phase = Phase::Ended;
        w.push("end", name(p));
        return true;
    }
    end_process(w, p, false, "exit");
    return true;
}

// The reader's step: its lock, shared, through PosixFs::lock (what Store::inspect takes first), or its release.
bool reader_step(Walk& w) {
    if (w.reader_lock) {
        reader_release(w);
        return true;
    }
    std::unique_ptr<FileLock> held;
    const Status s = w.reader_fs->lock(w.dir, LockMode::Shared, held);
    // Refused: busy (a writer holds the lock) or refused (a writer waits at the gate); read nothing.
    if (!s.is_ok() && s.code() != Status::Code::Busy && s.code() != Status::Code::Refused) {
        std::fprintf(stderr, "store_lock_walk: the reader's lock failed: %s\n", s.message().c_str());
        return false;
    }
    w.reader_lock = std::move(held);
    w.push("rlock", "r1");
    return true;
}

std::string quoted(const std::string& s) {
    return "\"" + s + "\"";
}

std::string fmt_step(const Step& st) {
    std::ostringstream o;
    o << "[op |-> " << quoted(st.op) << ", p |-> " << quoted(st.who) << ", pcs |-> <<";
    for (std::size_t i = 0; i < st.pcs.size(); ++i) {
        o << (i ? ", " : "") << quoted(st.pcs[i]);
    }
    o << ">>, holders |-> " << st.holders << ", readers |-> " << st.readers << ", sock |-> " << quoted(st.sock)
      << ", seqs |-> <<";
    for (std::size_t i = 0; i < st.seqs.size(); ++i) {
        o << (i ? ", " : "") << st.seqs[i];
    }
    o << ">>, sent |-> " << st.sent << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: store_lock_walk <seed> <steps> [--faulty-unlocked | --faulty-restore-first | "
                             "--faulty-held-refused | --faulty-reader-busy | --faulty-no-gate] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const std::uint64_t count = std::strtoull(argv[2], nullptr, 10);
    Walk w;
    if (argc == 5) {
        if (std::strcmp(argv[3], "--faulty-unlocked") == 0) {
            w.fault = Fault::Unlocked;
        } else if (std::strcmp(argv[3], "--faulty-restore-first") == 0) {
            w.fault = Fault::RestoreFirst;
        } else if (std::strcmp(argv[3], "--faulty-held-refused") == 0) {
            w.fault = Fault::HeldRefused;
        } else if (std::strcmp(argv[3], "--faulty-reader-busy") == 0) {
            w.fault = Fault::ReaderBusy;
        } else if (std::strcmp(argv[3], "--faulty-no-gate") == 0) {
            w.fault = Fault::NoGate;
        } else {
            std::fprintf(stderr, "store_lock_walk: unknown fault %s\n", argv[3]);
            return 2;
        }
    }
    const char* out_path = argv[argc - 1];
    w.dir = std::string(out_path) + ".store";
    std::error_code ec;
    std::filesystem::remove_all(w.dir, ec);
    w.rng = Rng(seed);
    {
        PosixOptions reader;
        reader.test_no_gate = w.fault == Fault::NoGate;
        w.reader_fs = nexus::store::make_posix_fs(std::move(reader));
    }

    // The store before the walk: one session, one prompt queued, one record.
    {
        std::unique_ptr<Fs> fs = nexus::store::make_posix_fs();
        std::unique_ptr<Store> s;
        const Status opened = Store::open(*fs, w.dir, StoreOptions{}, s);
        if (!opened.is_ok()) {
            std::fprintf(stderr, "store_lock_walk: the first open failed: %s\n", opened.message().c_str());
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
    w.push("init", "p1");

    bool stopped = false;
    for (std::uint64_t i = 0; i < count && !stopped; ++i) {
        const std::size_t who = std::size_t(w.rng.below(kWriters + 1));
        const bool ok = who < kWriters ? writer_step(w, who) : reader_step(w);
        stopped = !ok;
    }
    for (Proc& q : w.procs) {
        q.store.reset();
        q.fs.reset();
    }
    w.reader_lock.reset();
    if (stopped && w.fault == Fault::None) {
        return 1;
    }
    const bool needs_reach = w.fault == Fault::HeldRefused || w.fault == Fault::ReaderBusy ||
                            w.fault == Fault::NoGate;
    if (needs_reach && w.reached == 0) {
        std::fprintf(stderr, "store_lock_walk: the walk never reached its fault; give it another seed or more steps\n");
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
    std::uint64_t counts[17] = {};
    const char* ops[] = {"start", "restart", "probe", "rwait", "lock", "heldtick", "open", "listen", "restore",
                         "write", "stopaccept", "close", "end", "exit", "die", "rlock", "rrelease"};
    for (const Step& st : w.steps) {
        for (std::size_t k = 0; k < 17; ++k) {
            if (st.op == ops[k]) {
                ++counts[k];
            }
        }
    }
    std::printf("store_lock_walk: %zu trace steps, %llu sends, %llu readers gave way, fault reached %llu times;",
                w.steps.size(), static_cast<unsigned long long>(w.sent),
                static_cast<unsigned long long>(w.gave_way), static_cast<unsigned long long>(w.reached));
    for (std::size_t k = 0; k < 17; ++k) {
        std::printf(" %s %llu", ops[k], static_cast<unsigned long long>(counts[k]));
    }
    std::printf("%s\n", stopped ? "; stopped where the faulty order's store refused" : "");
    std::filesystem::remove_all(w.dir, ec);
    return 0;
}
