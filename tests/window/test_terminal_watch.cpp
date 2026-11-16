// Unit tests of terminal foreground synchronization between the background poll thread and the frame thread
// (src/terminal_watch.h, models/TerminalWatch.tla). Pure logic: no OS, no clock, no threads, no sleeps.

#include <cstdio>
#include <string>

#include "terminal_watch.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

constexpr int kSh = 100;
constexpr int kSsh = 200;
constexpr int kNextSsh = 300;

// Reproduces the exact ABA race where ssh starts and exits within one 250 ms background poll window:
//   1. Background poll sees the shell (kSh) and records {kSh, ""}.
//   2. ssh (kSsh) starts and sets its initial title; the frame thread reads kSsh and observes "unknown.example".
//   3. ssh prints its exit title and exits; the frame thread reads the foreground just before /bin/sh calls
//      tcsetpgrp(kSh), so it still sees kSsh ("unknown.example").
//   4. /bin/sh reclaims kSh without setting another title.
//   5. The next background poll reads kSh: it must see that the UI was at kSsh (not stale kSh), wake the window, and
//      restore `seen` to {kSh, ""}.
void abaWithinOnePollWindowRestoresShell() {
    ForegroundSeen shared;
    ForegroundSeen seen;

    // 1. Initial background poll while /bin/sh is in the foreground.
    const ForegroundSeen snap0 = shared;
    CHECK(pollNeedsArguments(snap0, kSh));
    const PollCommit poll0 = commitPollForeground(shared, snap0, kSh, "");
    CHECK(poll0.changed && !poll0.recheck);
    takeForegroundWatch(shared, seen);
    CHECK(seen.group == kSh && seen.remote.empty());

    // 2. ssh starts and sets its initial OSC title; frame thread reads kSsh.
    takeForegroundWatch(shared, seen);
    CHECK(foregroundNeedsArguments(seen, kSsh));
    CHECK(!commitFrameForeground(shared, seen, kSsh, "unknown.example"));
    CHECK(seen.group == kSsh && seen.remote == "unknown.example");

    // 3. ssh prints its exit OSC title just before /bin/sh calls tcsetpgrp(kSh); frame thread still reads kSsh,
    // without re-reading process arguments.
    takeForegroundWatch(shared, seen);
    CHECK(!foregroundNeedsArguments(seen, kSsh));
    CHECK(!commitFrameForeground(shared, seen, kSsh, seen.remote));
    CHECK(seen.group == kSsh && seen.remote == "unknown.example");

    // 4. /bin/sh reclaims kSh with no further title change.
    // 5. Next background poll wakes at +250 ms and reads kSh.
    const ForegroundSeen snap1 = shared;
    CHECK(pollNeedsArguments(snap1, kSh));
    const PollCommit poll1 = commitPollForeground(shared, snap1, kSh, "");
    CHECK(poll1.changed && !poll1.recheck);

    // Next frame takes the background observation and clears the ssh remote label.
    takeForegroundWatch(shared, seen);
    CHECK(seen.group == kSh);
    CHECK(seen.remote.empty());
}

// A background poll that snapped `shared` before a frame-thread title observation and finishes after it never
// overwrites the newer frame observation with its stale group, and asks for an immediate recheck.
void overlappedPollNeverClobbersNewerFrameObservation() {
    ForegroundSeen shared;
    ForegroundSeen seen;

    const PollCommit init = commitPollForeground(shared, shared, kSh, "");
    CHECK(init.changed && !init.recheck);
    takeForegroundWatch(shared, seen);

    // Background thread snapshots {seq=1, kSh} and reads kSsh before preemption.
    const ForegroundSeen staleSnap = shared;
    CHECK(pollNeedsArguments(staleSnap, kSsh));

    // Meanwhile kSsh exits, kNextSsh starts and sets a title; frame thread observes kNextSsh.
    takeForegroundWatch(shared, seen);
    CHECK(foregroundNeedsArguments(seen, kNextSsh));
    CHECK(!commitFrameForeground(shared, seen, kNextSsh, "space"));
    CHECK(shared.group == kNextSsh && shared.remote == "space");

    // Background thread resumes and tries to commit its stale kSsh read.
    const PollCommit overlapped = commitPollForeground(shared, staleSnap, kSsh, "hetzner2");
    CHECK(!overlapped.changed && overlapped.recheck);
    CHECK(shared.group == kNextSsh && shared.remote == "space");

    // Immediate recheck against the new shared state converges cleanly when kNextSsh is still running.
    const ForegroundSeen cleanSnap = shared;
    CHECK(!pollNeedsArguments(cleanSnap, kNextSsh));
    const PollCommit clean = commitPollForeground(shared, cleanSnap, kNextSsh, "space");
    CHECK(!clean.changed && !clean.recheck);
    takeForegroundWatch(shared, seen);
    CHECK(seen.group == kNextSsh && seen.remote == "space");
}

// If a background poll commits between the frame thread's `takeForegroundWatch` and `commitFrameForeground` (for
// example, frame read kSsh just before exit, then background poll committed kSh before frame re-locked), the frame
// keeps its title observation for this frame and requests an immediate background recheck so `shared` settles to kSh.
void overlappedFrameRequestsImmediateRecheck() {
    ForegroundSeen shared;
    ForegroundSeen seen;

    (void)commitPollForeground(shared, shared, kSsh, "space");
    takeForegroundWatch(shared, seen);
    CHECK(seen.group == kSsh && seen.remote == "space");

    // Frame thread took `seen` at kSsh and read kSsh on an exit title, then paused before committing.
    // Meanwhile /bin/sh reclaimed kSh and background poll committed kSh.
    const ForegroundSeen bgSnap = shared;
    const PollCommit bg = commitPollForeground(shared, bgSnap, kSh, "");
    CHECK(bg.changed && !bg.recheck);
    CHECK(shared.group == kSh && shared.remote.empty());

    // Frame thread commits its kSsh title observation: `overlapped` must return true so the background thread rechecks.
    const bool recheck = commitFrameForeground(shared, seen, kSsh, "space");
    CHECK(recheck);
    CHECK(seen.group == kSsh && seen.remote == "space");

    // Immediate recheck on the background thread sees kSh != shared.group (kSsh) and restores kSh.
    const ForegroundSeen recheckSnap = shared;
    CHECK(pollNeedsArguments(recheckSnap, kSh));
    const PollCommit fixed = commitPollForeground(shared, recheckSnap, kSh, "");
    CHECK(fixed.changed && !fixed.recheck);
    takeForegroundWatch(shared, seen);
    CHECK(seen.group == kSh && seen.remote.empty());
}

void steadyPollsAndFramesReadNoArgumentsAndDoNotWake() {
    ForegroundSeen shared;
    ForegroundSeen seen;

    CHECK(pollNeedsArguments(shared, -1));
    const PollCommit first = commitPollForeground(shared, shared, -1, "");
    CHECK(first.changed && !first.recheck);
    takeForegroundWatch(shared, seen);
    CHECK(seen.seq == 1 && seen.group == -1 && seen.remote.empty());

    // Subsequent poll of the same group (-1) needs no arguments and does not wake.
    CHECK(!pollNeedsArguments(shared, -1));
    const PollCommit second = commitPollForeground(shared, shared, -1, "");
    CHECK(!second.changed && !second.recheck);
    CHECK(shared.seq == 1);

    // Title change on the same group needs no arguments.
    CHECK(!foregroundNeedsArguments(seen, -1));
}

} // namespace

int main() {
    abaWithinOnePollWindowRestoresShell();
    overlappedPollNeverClobbersNewerFrameObservation();
    overlappedFrameRequestsImmediateRecheck();
    steadyPollsAndFramesReadNoArgumentsAndDoNotWake();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("terminal watch unit tests passed\n");
    return 0;
}
