// A newer build of nexus-server taking over from the one running, with no session lost (models/Takeover.tla).
//
// `nexus-server setup` (and `rebuild`, which ends in it) places the program it was built as at ~/.nexus/bin, keeping
// the one before as nexus-server.previous, then starts `nexus-server handover`, detached, which brings the running
// server to that build (Handover). It looks at the server (nextStep) and takes one step at a time:
//
//   nothing answers                  start the placed program (the service, or `serve` detached without one)
//   it answers with the placed build done
//   it answers with another build    ask it to drain (`takeover` on the control socket)
//   it does not know `build`         it is from before takeover: ask it to stop (`stop`), as a person would have
//   it is draining                   wait
//   started, no answer in kStartMs   put the previous program back and start that (a rollback)
//
// A draining server (Drain) stops as soon as no friend's turn is open, so no turn is cut; if turns keep it busy past
// kDrainMs, it stops anyway, and each turn still open is cut with a notice, as any stop cuts it. It exits with
// kTakeoverExit, which the service takes as "start me again"; the service, or the handover when there is none, starts
// the placed program, the new build, on the same store, which holds every session. A turn never runs on both: one
// server holds the control socket and the store at a time.
//
// The build of a program is the hash of its bytes (buildOf): any change at all is another build, and the placed
// program is the one that should run, whether it is newer or older than the running one. Pure: no OS, no time.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "server/supervisor.h"

namespace nexus::server {

// The exit status of a server that drained for a takeover: EX_TEMPFAIL, which the service (launchd's KeepAlive with
// SuccessfulExit false, systemd's Restart=on-failure) answers by starting the placed program again. A `stop` exits 0,
// and the service leaves it stopped.
constexpr int kTakeoverExit = 75;

// How long a draining server waits for the open turns to end before it stops anyway. A judgement, not measured: long
// enough for a friend's turn that is writing code to finish, short enough that a turn that never ends does not hold
// back a build the person asked for.
constexpr Millis kDrainMs = 10 * 60 * 1000;

// The build of a program: the 64-bit FNV-1a hash of its bytes, as 16 lowercase hex digits.
std::string buildOf(std::string_view bytes);

// The running server's side: whether a takeover was asked, and when it stops.
class Drain {
public:
    // A takeover asked at `now`: from here on the server stops as soon as no turn is open, or at the deadline. A second
    // request keeps the first one's deadline.
    void request(Millis now);
    bool draining() const { return draining_; }
    // Draining, and the deadline has passed.
    bool late(Millis now) const { return draining_ && now >= deadline_; }
    // Whether the server stops now: draining, and no turn is open (`busy` false) or the deadline has passed.
    bool stopNow(bool busy, Millis now) const { return draining_ && (!busy || late(now)); }
    // How long until the deadline, for the loop's wait; nothing when not draining.
    std::optional<Millis> untilDeadline(Millis now) const;

private:
    bool draining_ = false;
    Millis deadline_ = 0;
};

// What the installer sees of the running server.
struct Seen {
    bool answers = false;       // a server answers on the control socket
    bool knowsBuild = false;    // it answered `build`; one from before takeover answers "unknown request"
    std::string build;          // its build, when it knows
    bool draining = false;      // it said it is draining
    std::string placed;         // the placed program's build
};

enum class Step {
    Done,       // the placed build runs
    Start,      // nothing runs: start the placed program
    AskDrain,   // another build runs: ask it to drain
    AskStop,    // a server from before takeover runs: ask it to stop
    Wait,       // it is draining: look again
};

Step nextStep(const Seen& seen);

const char* stepName(Step step);

// How long a started program has to answer `build` before the handover takes it as failed: a judgement, not measured,
// many times what a server takes to open its store and listen (well under a second on the fleet's machines).
constexpr Millis kStartMs = 30 * 1000;

// What the handover does next (Handover::step).
enum class Act {
    Done,       // the placed build answers: the handover is over
    Wait,       // look again
    AskStop,    // a server from before takeover runs: ask it to stop
    AskDrain,   // another build runs: ask it to drain
    Start,      // nothing runs: switch the driver to the placed build's and start the placed program
    RollBack,   // the placed build did not answer in time: put the previous program (and driver) back, then start it
    Fail,       // the previous build did not answer either: nothing more to do but say so
};

const char* actName(Act act);

// The handover from the running server to the placed build (models/Takeover.tla), as one process sees it through to
// the end: `nexus-server handover`, which setup starts detached so that the handover finishes whatever setup's own wait,
// with a service or without one. It is the one that starts the placed program when nothing runs (a service may too),
// and the one that notices a placed build that never answers (one that crashes as it opens the store, say) and puts
// the previous build back. Pure: no OS, no time; the caller does what each Act says and calls step again.
//
// The previous build is the one placed before, which ran: setup keeps its program (and its driver) beside the new one.
// A previous build of "" (a first install) cannot be rolled back to: a failed start then ends in Fail.
class Handover {
public:
    // `startMs`: how long a started program has to answer (kStartMs; setup's --start-wait, which tests shorten).
    Handover(std::string placed, std::string previous, Millis startMs = kStartMs)
        : placed_(std::move(placed)), previous_(std::move(previous)), startMs_(startMs) {}

    Act step(const Seen& seen, Millis now);

    const std::string& placed() const { return placed_; }       // after a RollBack: the previous build
    bool failed() const { return failed_; }                     // a start did not answer in time, not yet rolled back
    bool rolledBack() const { return rolledBack_; }

private:
    std::string placed_;
    std::string previous_;
    Millis startMs_;
    std::optional<Millis> startedAt_;
    bool failed_ = false;
    bool rolledBack_ = false;
};

// The control socket's reply to `build`: "BUILD <build>", then " draining" while it drains.
std::string buildReply(const std::string& build, bool draining);
// Reads a `build` reply into seen->build and seen->draining (and knowsBuild true). False for any other line.
bool parseBuildReply(const std::string& line, Seen* seen);

} // namespace nexus::server
