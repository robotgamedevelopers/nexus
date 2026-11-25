// Drives the client lock's iOS part (src/ios_lock.h) over the real nexus::ClientLock with a seeded random walk of what
// iOS does around it, and writes the ClientLock operations the glue made as a trace (ClientLockTraceData.tla) for
// models/ClientLockTrace.tla, under which TLC checks that the trace is a behaviour of models/ClientLock.tla. So every
// behaviour of the iOS glue (which operations it makes for which iOS events) is checked against the model, as
// lock_walk checks the lock's own rules.
//
//   ios_lock_walk <seed> <steps> [--faulty-stale | --faulty-draw | --faulty-idle] <out.tla>
//
// The walk's iOS: the app's lifecycle (active, inactive, background, in any order, the inactive of the app's own
// prompt among them), the device's screen lock, taps on the lock screen, the authenticator answering each attempt it
// was asked (granting, failing, cancelled or unavailable; an attempt the glue cancelled is answered cancelled), time in
// whole seconds, the person's input and frames. The idle timeout is three seconds, as in ClientLockTrace.cfg. Its
// random numbers come from the seed (splitmix64), never from the clock or the OS.
//
// Each fault is the glue's, laid over it here, and TLC must reject its traces (the negative check):
// --faulty-stale passes a late grant on as the current attempt's (an answer to a prompt the background overtook
// unlocks); --faulty-draw lets a frame draw the content while the prompt is up; --faulty-idle never gives the glue the
// time, so the idle timeout never fires.

#include "client_lock.h"
#include "ios_lock.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace nexus;

constexpr int kTimeout = 3;     // ClientLockTrace.cfg's Timeout

enum class Fault { None, Stale, Draw, Idle };

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

struct Walk {
    Fault fault = Fault::None;
    ClientLock lock{kTimeout};
    std::vector<std::uint64_t> pending;     // attempts the authenticator was asked and has not answered
    std::set<std::uint64_t> cancelled;      // of those, the ones the glue cancelled: answered cancelled
    std::vector<std::string> records;
    double now = 0;                         // the walk's clock
    double lastTick = 0;                    // the time of the last tick recorded
    bool hold = false;                      // the walk writes the next answer's record itself (--faulty-stale)
    std::size_t unlocks = 0, stale = 0, relocks = 0, idles = 0, ownPromptInactive = 0;
    std::size_t faulted = 0;                // late grants the faulty glue took (--faulty-stale)

    // The lock's time: the walk's clock, or none at all for a glue that never gets it (--faulty-idle).
    double lockTime() const { return fault == Fault::Idle ? 0 : now; }

    void record(const char* op, const char* arg, std::uint64_t n, int d, bool flag) {
        const double idle = lock.idleFor(lockTime());
        records.push_back(std::string("[op |-> \"") + op + "\", arg |-> \"" + arg + "\", n |-> " + std::to_string(n) +
            ", d |-> " + std::to_string(d) + ", flag |-> " + (flag ? "TRUE" : "FALSE") + ", state |-> \"" +
            stateName(lock.state()) + "\", why |-> \"" + whyName(lock.reason()) + "\", attempt |-> " +
            std::to_string(lock.attempt()) + ", idle |-> " + std::to_string(static_cast<long>(idle)) + "]");
    }

    // Each ClientLock operation the glue made, as the model names it.
    void step(const IosLockStep& s) {
        switch (s.op) {
        case IosLockStep::Op::Prompt: record("prompt", "", s.attempt, 0, false); break;
        case IosLockStep::Op::Answer:
            if (!hold) record("answer", answerName(s.answer), s.attempt, 0, false);
            break;
        case IosLockStep::Op::Relock:
            ++relocks;
            record("relock", whyName(s.why), 0, 0, false);
            break;
        case IosLockStep::Op::Tick: {
            const int d = static_cast<int>(s.now - lastTick);
            lastTick = s.now;
            record("tick", "", 0, d, false);
            break;
        }
        case IosLockStep::Op::Input: record("input", "", 0, 0, s.flag); break;
        case IosLockStep::Op::Frame:
            record("frame", "", 0, 0, s.flag || (fault == Fault::Draw && lock.state() == LockState::Prompting));
            break;
        }
    }

    IosLockSystem system() {
        IosLockSystem sys;
        sys.request = [this](std::uint64_t attempt) { pending.push_back(attempt); };
        sys.cancel = [this] {
            if (!pending.empty()) cancelled.insert(lock.attempt());
        };
        sys.cover = [](bool) {};
        sys.step = [this](const IosLockStep& s) { step(s); };
        return sys;
    }
};

int walk(std::uint64_t seed, std::uint64_t steps, Fault fault, const char* path) {
    Rng rng{seed};
    Walk w;
    w.fault = fault;
    IosLock glue(w.lock, w.system());
    w.record("init", "", 0, 0, false);
    const LockAnswer answers[] = {LockAnswer::Granted, LockAnswer::Granted, LockAnswer::Granted, LockAnswer::Failed,
                                  LockAnswer::Cancelled, LockAnswer::Unavailable};
    const IosLifecycle phases[] = {IosLifecycle::Active, IosLifecycle::Active, IosLifecycle::Inactive,
                                   IosLifecycle::Background};
    while (w.records.size() <= steps) {
        switch (rng.below(14)) {
        case 0:
        case 1: {
            const IosLifecycle to = phases[rng.below(4)];
            if (to == IosLifecycle::Inactive && w.lock.state() == LockState::Prompting) ++w.ownPromptInactive;
            glue.lifecycle(to);
            break;
        }
        case 2: glue.screenLocked(); break;
        case 3: glue.unlock(); break;
        case 4:
        case 5:
        case 6: {
            if (w.pending.empty()) continue;
            const std::size_t i = rng.below(w.pending.size());
            const std::uint64_t n = w.pending[i];
            w.pending.erase(w.pending.begin() + std::ptrdiff_t(i));
            // A cancelled attempt is answered cancelled, unless its answer was already on its way (a grant racing the
            // cancel among them).
            const bool wasCancelled = w.cancelled.erase(n) != 0;
            const LockAnswer a = wasCancelled && rng.below(2) == 0
                ? LockAnswer::Cancelled
                : answers[rng.below(sizeof answers / sizeof answers[0])];
            const bool current = w.lock.state() == LockState::Prompting && n == w.lock.attempt();
            if (!current) ++w.stale;
            std::deque<LockNotice> notice;
            if (fault == Fault::Stale && !current && a == LockAnswer::Granted &&
                    w.lock.state() == LockState::Prompting) {
                // the faulty glue takes a late grant as the current attempt's; the record keeps the attempt it answered
                notice.push_back({LockNotice::Kind::Answer, w.lock.attempt(), a, LockReason::None, {}});
                w.hold = true;
                glue.notices(std::move(notice), w.lockTime());
                w.hold = false;
                w.record("answer", answerName(a), n, 0, false);
                ++w.faulted;
            } else {
                notice.push_back({LockNotice::Kind::Answer, n, a, LockReason::None, {}});
                glue.notices(std::move(notice), w.lockTime());
            }
            if (current && w.lock.state() == LockState::Unlocked) ++w.unlocks;
            break;
        }
        case 7:
        case 8: {
            w.now += 1 + double(rng.below(kTimeout));
            const bool was = w.lock.state() == LockState::Unlocked;
            if (fault == Fault::Idle) {
                const int d = static_cast<int>(w.now - w.lastTick);
                w.lastTick = w.now;
                w.record("tick", "", 0, d, false);
            } else {
                glue.tick(w.now);
            }
            if (was && w.lock.state() == LockState::Locked) ++w.idles;
            break;
        }
        case 9:
        case 10: glue.input(w.lockTime()); break;
        default: glue.frame(); break;
        }
    }
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "ios_lock_walk: cannot write %s\n", path);
        return 1;
    }
    out << "---- MODULE ClientLockTraceData ----\nEXTENDS Sequences\nTrace == <<\n";
    for (std::size_t i = 0; i < w.records.size(); ++i) {
        out << "    " << w.records[i] << (i + 1 < w.records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "ios_lock_walk: writing %s failed\n", path);
        return 1;
    }
    // A walk that never unlocked, never answered late, never relocked, never saw the app's own prompt make it inactive
    // or never timed out checks too little.
    if (w.unlocks == 0 || w.stale == 0 || w.relocks == 0 || w.ownPromptInactive == 0 ||
            (w.idles == 0 && fault != Fault::Idle) || (w.faulted == 0 && fault == Fault::Stale)) {
        std::fprintf(stderr, "ios_lock_walk: seed %llu reached too little (unlocks %zu, stale answers %zu, relocks %zu, "
            "inactive while prompting %zu, idle locks %zu, late grants taken %zu): use more steps\n",
            static_cast<unsigned long long>(seed), w.unlocks, w.stale, w.relocks, w.ownPromptInactive, w.idles,
            w.faulted);
        return 1;
    }
    std::printf("ios_lock_walk: %zu steps; unlocks %zu, stale answers %zu, relocks %zu, inactive while prompting %zu, "
        "idle locks %zu\n", w.records.size() - 1, w.unlocks, w.stale, w.relocks, w.ownPromptInactive, w.idles);
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
        std::fprintf(stderr, "usage: ios_lock_walk <seed> <steps> [--faulty-stale | --faulty-draw | --faulty-idle] "
            "<out.tla>\n");
        return 2;
    }
    char* end = nullptr;
    const auto seed = std::strtoull(args[0], &end, 10);
    if (*end != 0) {
        std::fprintf(stderr, "ios_lock_walk: the seed must be a number: %s\n", args[0]);
        return 2;
    }
    const auto steps = std::strtoull(args[1], &end, 10);
    if (*end != 0 || steps == 0) {
        std::fprintf(stderr, "ios_lock_walk: the steps must be a positive number: %s\n", args[1]);
        return 2;
    }
    return walk(seed, steps, fault, args[2]);
}
