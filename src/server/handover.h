// Handover: a placed build replaces the running server under an OS lock, with durable step-by-step state on disk so a
// handover cut anywhere resumes from what is on disk (docs/builds.md; models/handover/Handover.tla). Layer 3 of the
// takeover (docs/roadmap.sexp, `takeover-handover`), on top of the store lock (layer 1, models/store/StoreLock.tla)
// and placement (layer 2, src/server/builds.h, models/placement/Placement.tla).
//
// Under the server's directory (server/home.h):
//   handover/               mode 0700; its exclusive flock (store/fs.h: unlockable_filesystem and locks_kept checked,
//                           released by the OS however the process ends) keeps at most one handover running at a time
//   handover/state          target, previous (the last working build) and stage (`switching`, `draining`, `starting`,
//                           `rolling-back`), written through a synced `.state-*` file, renamed over `state`, with
//                           `handover/` synced before the stage acts
//   handover/.state-*       what an interrupted state write left; removed under the lock after syncing `handover/`
//
// Any run acquiring the handover lock settles `handover/`, removes `.state-*` leftovers, and if `handover/state` is
// present, finishes resuming that interrupted handover before starting any new one, so `previous` (the last known
// working build) is never overwritten by a broken build that never served. If the target build fails to answer SERVING
// within `start_wait_ms`, handover records `rolling-back`, switches `builds/active` back to `previous`, starts
// `previous` until it answers SERVING, and only then clears `handover/state`.

#pragma once

#include "server/builds.h"
#include "server/drain.h"
#include "store/fs.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nexus::server::handover {

// Exit statuses of `nexus-server handover`.
constexpr int kExitOk = 0;          // handed over (or already serving; for `resume`, recovered to SERVING)
constexpr int kExitFailed = 1;      // a filesystem or OS call failed, locks cannot be trusted, or no build could serve
constexpr int kExitUsage = 2;       // the command line does not read
constexpr int kExitRefused = 3;     // a rule refused the target (not placed, untested, formats, no current build)
constexpr int kExitBusy = 4;        // another handover held the lock on handover/ for the whole wait
constexpr int kExitRolledBack = 5;  // the target build failed to reach SERVING; rolled back to `previous`, now serving

constexpr std::uint64_t kLockWaitMs = 60'000;
constexpr std::uint64_t kLockRetryMs = 10;
constexpr std::uint64_t kStartMs = 30'000;
constexpr std::uint64_t kDrainWaitMs = kDrainMs + 30'000;
constexpr std::uint64_t kPollMs = 20;

inline const char* const kHandoverDir = "handover";
inline const char* const kStateFile = "state";
inline const char* const kStateTmpPrefix = ".state-";

enum class Stage { Switching, Draining, Starting, RollingBack };

const char* stageName(Stage s);
bool parseStage(const std::string& text, Stage* out);

// The durable record in `<server dir>/handover/state`:
//   nexus-handover 1
//   target <hash>
//   previous <hash|none>
//   stage <switching|draining|starting|rolling-back>
//   end
struct StateRecord {
    std::string target;
    std::string previous;   // empty when none
    Stage stage = Stage::Switching;
    bool operator==(const StateRecord&) const = default;
};

std::string formatState(const StateRecord& rec);
bool readState(const std::string& text, StateRecord* out, std::string* why);

// Reversed faults of the trace walk (tests/server/handover_walk.cpp), matching `Bug` in models/handover/Handover.tla;
// never set outside tests.
enum class Fault {
    None,
    NoLock,
    NoSync,
    InMemoryOnly,
    OverwritePrev,
    NoRollback,
    StopCuts,
    AllowUntested,
    ServeBroken,
};

// Seam over layer 2 (`Builds`) so unit tests run in memory without touching disk.
class PlacementPort {
public:
    virtual ~PlacementPort() = default;
    virtual int active(std::string* current, std::string* previous, builds::Result& r) = 0;
    virtual int checkSwitch(const std::string& hash, builds::Result& r, bool allowUntested) = 0;
    virtual int makeCurrent(const std::string& hash, builds::Result& r, bool allowUntested) = 0;
    virtual int rollback(const std::string& hash, builds::Result& r) { return makeCurrent(hash, r, false); }
};

enum class DrainReply {
    Accepted,        // the server answered `OK` to `drain`
    UnknownRequest,  // a server from before `drain` answered `ERROR unknown request;...`
    Closed,          // the server closed or vanished before answering
    Failed,          // an unexpected socket or protocol failure
};

enum class StartReply {
    Waiting,  // child started (or starting); poll `probe` again
    Failed,   // child exited non-zero, could not be spawned, or `limit_ms` elapsed without SERVING
};

// Seam over the running server's control socket and process launch so unit tests and trace walks run without real
// sockets, processes or sleeps.
class ServerPort {
public:
    virtual ~ServerPort() = default;
    virtual store::Status probe(ServerSeen* out) = 0;
    virtual DrainReply requestDrain(std::string* why) = 0;
    virtual store::Status stop() = 0;
    virtual StartReply stepStart(const std::string& expectedBuild, std::uint64_t elapsed_ms, std::uint64_t limit_ms,
                                 std::string* why) = 0;
    virtual void keepStarted() = 0;
    virtual void abortStarted() = 0;
};

struct Options {
    // Called before each step (matching `pc` in Handover.tla: lock, settle, clean, inspect, wswitch, sswitch, switch,
    // wdrain, sdrain, drain, wstart, sstart, start, wrollback, srollback, rollback, start_rb, clear, sclear, unlock),
    // and with "idle" when the command ends.
    std::function<void(const std::string& step)> hook;
    std::function<std::uint64_t()> now_ms;
    std::function<void(std::uint64_t)> sleep_ms;
    std::uint64_t lock_wait_ms = kLockWaitMs;
    std::uint64_t start_wait_ms = kStartMs;
    std::uint64_t drain_wait_ms = kDrainWaitMs;
    std::uint64_t poll_ms = kPollMs;
    std::string test_fs_type;
    bool test_locks_ignored = false;
    bool lock_already_held = false;
    pid_t* adopted_pid_out = nullptr;
    Fault fault = Fault::None;
};

struct Result {
    int exit = kExitOk;
    std::vector<std::string> out;
    std::vector<std::string> err;
};

// The handover state machine over `fs`, `placement`, and `server`.
class Handover {
public:
    Handover(store::Fs& fs, PlacementPort& placement, ServerPort& server, std::string handoverDir,
             Options options = {});
    ~Handover();
    Handover(const Handover&) = delete;
    Handover& operator=(const Handover&) = delete;

    // Hands over to placed build `hash`: switches `builds/active`, drains the running server, starts `hash`, and rolls
    // back to `previous` if `hash` fails to reach SERVING within `start_wait_ms`.
    int to(const std::string& hash, Result& r);
    // Finishes any interrupted handover in `handover/state`, or ensures `builds/active`'s current build is serving.
    int resume(Result& r);
    // Reports `handover/state`, `builds/active`, and the running server without changing anything.
    int status(Result& r);

    const std::string& phase() const { return phase_; }
    bool holdsLock() const { return locked_; }

    struct Run;

private:
    friend struct Run;
    store::Fs& fs_;
    PlacementPort& placement_;
    ServerPort& server_;
    std::string handoverDir_;
    Options options_;
    std::string phase_ = "idle";
    bool locked_ = false;
};

std::unique_ptr<PlacementPort> makeBuildsPlacement(const std::string& serverDir,
                                                   const builds::Options& options = {});
std::unique_ptr<ServerPort> makePosixServerPort(const std::string& serverDir,
                                                const std::function<std::uint64_t()>& now_ms = {},
                                                const std::function<void(std::uint64_t)>& sleep_ms = {},
                                                pid_t* adopted_pid_out = nullptr);

// `nexus-server handover [<hash>|resume|status|help]`: exit status as above.
int command(const std::string& serverDir, const std::vector<std::string>& args, const Options& options, Result& r);
std::string help(const std::string& verb);

} // namespace nexus::server::handover
