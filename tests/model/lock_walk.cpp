// Drives the real nexus::ClientLock with a seeded random walk and writes the trace as a TLA+ module
// (ClientLockTraceData.tla) for models/ClientLockTrace.tla, under which TLC checks that the trace is a behaviour of
// models/ClientLock.tla.
//
//   lock_walk <seed> <steps> [--faulty-stale | --faulty-draw | --faulty-idle] <out.tla>
//
// Around the lock stands a stand-in for the system: the authenticator, which answers each attempt it was given (the
// current one or one a relock overtook) at a time drawn from the seed, granting, failing, cancelled or unavailable;
// and the OS, which sends relock events. The person's input and the frames come between. Time moves only by tick, in
// whole seconds, as the walk's clock; the lock's idle timeout is three seconds, as in ClientLockTrace.cfg. Each record
// holds the step (op, its argument, attempt, seconds and the code's answer: a frame's drawsContent(), an input's
// delivery) and the lock's state after it.
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from the
// clock or the OS.
//
// --faulty-stale walks a lock that takes a grant for any attempt still unanswered (an answer that came after the Mac
// slept unlocks it). --faulty-draw walks one whose frame draws the content while the system's prompt is up.
// --faulty-idle walks one whose idle timeout never fires. TLC must reject each one's traces; that is the negative check
// that shows the binding can fail.

#include "client_lock.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using nexus::ClientLock;
using nexus::LockAnswer;
using nexus::LockReason;
using nexus::LockState;

constexpr int kTimeout = 3;     // ClientLockTrace.cfg's Timeout

enum class Fault { None, Stale, Draw, Idle };

// The lock under test, with a fault laid over the real one when one is asked for.
struct Lock {
    ClientLock real{kTimeout};
    Fault fault = Fault::None;

    std::uint64_t prompt() { return real.prompt(); }
    void answer(std::uint64_t n, LockAnswer a, double now) {
        if (fault == Fault::Stale && a == LockAnswer::Granted && real.state() == LockState::Prompting &&
                n != real.attempt()) {
            // the faulty lock takes it as the current attempt's
            real.answer(real.attempt(), a, now);
            return;
        }
        real.answer(n, a, now);
    }
    void relock(LockReason why) { real.relock(why); }
    void tick(double now) {
        if (fault != Fault::Idle) real.tick(now);
    }
    bool input(double now) {
        if (fault == Fault::Idle) return real.state() == LockState::Unlocked;
        return real.input(now);
    }
    bool draws() const {
        return real.drawsContent() || (fault == Fault::Draw && real.state() == LockState::Prompting);
    }
};

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

const char* stateName(LockState s) {
    switch (s) {
    case LockState::Locked: return "locked";
    case LockState::Prompting: return "prompting";
    case LockState::Unlocked: return "unlocked";
    }
    return "?";
}

const char* whyName(LockReason r) {
    switch (r) {
    case LockReason::None: return "none";
    case LockReason::Launch: return "launch";
    case LockReason::Sleep: return "sleep";
    case LockReason::ScreenLock: return "screen";
    case LockReason::SessionSwitch: return "switch";
    case LockReason::Background: return "background";
    case LockReason::Idle: return "idle";
    case LockReason::Failed: return "failed";
    case LockReason::Cancelled: return "cancelled";
    case LockReason::Unavailable: return "unavailable";
    }
    return "?";
}

const char* answerName(LockAnswer a) {
    switch (a) {
    case LockAnswer::Granted: return "granted";
    case LockAnswer::Failed: return "failed";
    case LockAnswer::Cancelled: return "cancelled";
    case LockAnswer::Unavailable: return "unavailable";
    }
    return "?";
}

std::string record(const char* op, const char* arg, std::uint64_t n, int d, bool flag, const ClientLock& lock,
                   double now) {
    const double idle = lock.idleFor(now);
    return std::string("[op |-> \"") + op + "\", arg |-> \"" + arg + "\", n |-> " + std::to_string(n) +
        ", d |-> " + std::to_string(d) + ", flag |-> " + (flag ? "TRUE" : "FALSE") + ", state |-> \"" +
        stateName(lock.state()) + "\", why |-> \"" + whyName(lock.reason()) + "\", attempt |-> " +
        std::to_string(lock.attempt()) + ", idle |-> " + std::to_string(static_cast<long>(idle)) + "]";
}

int walk(std::uint64_t seed, std::uint64_t steps, Fault fault, const char* path) {
    Rng rng{seed};
    Lock lock;
    lock.fault = fault;
    double now = 0;
    std::vector<std::uint64_t> pending;     // attempts the authenticator has not answered yet
    std::vector<std::string> records{record("init", "", 0, 0, false, lock.real, now)};
    std::size_t unlocks = 0, stale = 0, relocks = 0, idles = 0;
    const LockReason events[] = {LockReason::Sleep, LockReason::ScreenLock, LockReason::SessionSwitch,
                                 LockReason::Background};
    const LockAnswer answers[] = {LockAnswer::Granted, LockAnswer::Granted, LockAnswer::Granted, LockAnswer::Failed,
                                  LockAnswer::Cancelled, LockAnswer::Unavailable};
    while (records.size() <= steps) {
        switch (rng.below(12)) {
        case 0:
        case 1: {
            const std::uint64_t n = lock.prompt();
            if (n != 0) pending.push_back(n);
            records.push_back(record("prompt", "", n, 0, false, lock.real, now));
            break;
        }
        case 2:
        case 3:
        case 4: {
            if (pending.empty()) continue;
            const std::size_t i = rng.below(pending.size());
            const std::uint64_t n = pending[i];
            pending.erase(pending.begin() + std::ptrdiff_t(i));
            const LockAnswer a = answers[rng.below(sizeof answers / sizeof answers[0])];
            const bool current = lock.real.state() == LockState::Prompting && n == lock.real.attempt();
            if (!current) ++stale;
            lock.answer(n, a, now);
            if (lock.real.state() == LockState::Unlocked && current) ++unlocks;
            records.push_back(record("answer", answerName(a), n, 0, false, lock.real, now));
            break;
        }
        case 5: {
            const LockReason why = events[rng.below(4)];
            lock.relock(why);
            ++relocks;
            records.push_back(record("relock", whyName(why), 0, 0, false, lock.real, now));
            break;
        }
        case 6:
        case 7: {
            const int d = 1 + int(rng.below(kTimeout));
            now += d;
            const bool was = lock.real.state() == LockState::Unlocked;
            lock.tick(now);
            if (was && lock.real.state() == LockState::Locked) ++idles;
            records.push_back(record("tick", "", 0, d, false, lock.real, now));
            break;
        }
        case 8:
        case 9: {
            const bool delivered = lock.input(now);
            records.push_back(record("input", "", 0, 0, delivered, lock.real, now));
            break;
        }
        default:
            records.push_back(record("frame", "", 0, 0, lock.draws(), lock.real, now));
            break;
        }
    }
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "lock_walk: cannot write %s\n", path);
        return 1;
    }
    out << "---- MODULE ClientLockTraceData ----\nEXTENDS Sequences\nTrace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "    " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "lock_walk: writing %s failed\n", path);
        return 1;
    }
    // A walk that never unlocked, never answered late, never relocked or never timed out checks too little.
    if (unlocks == 0 || stale == 0 || relocks == 0 || (idles == 0 && fault != Fault::Idle)) {
        std::fprintf(stderr, "lock_walk: seed %llu reached too little (unlocks %zu, stale answers %zu, relocks %zu, "
            "idle locks %zu): use more steps\n", static_cast<unsigned long long>(seed), unlocks, stale, relocks, idles);
        return 1;
    }
    std::printf("lock_walk: %zu steps; unlocks %zu, stale answers %zu, relocks %zu, idle locks %zu\n",
        records.size() - 1, unlocks, stale, relocks, idles);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Fault fault = Fault::None;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-stale") == 0) fault = Fault::Stale;
        else if (std::strcmp(argv[i], "--faulty-draw") == 0) fault = Fault::Draw;
        else if (std::strcmp(argv[i], "--faulty-idle") == 0) fault = Fault::Idle;
        else args.push_back(argv[i]);
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: lock_walk <seed> <steps> [--faulty-stale | --faulty-draw | --faulty-idle] "
            "<out.tla>\n");
        return 2;
    }
    char* end = nullptr;
    const auto seed = std::strtoull(args[0], &end, 10);
    if (*end != 0) {
        std::fprintf(stderr, "lock_walk: the seed must be a number: %s\n", args[0]);
        return 2;
    }
    const auto steps = std::strtoull(args[1], &end, 10);
    if (*end != 0 || steps == 0) {
        std::fprintf(stderr, "lock_walk: the steps must be a positive number: %s\n", args[1]);
        return 2;
    }
    return walk(seed, steps, fault, args[2]);
}
