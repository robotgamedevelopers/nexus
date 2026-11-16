// Supervision: keeping nexus-server running across boots, drain handovers (exit 75), intentional stops (exit 0), and
// post-start build failures (docs/builds.md; models/supervision/Supervision.tla). Layer 4 of the takeover
// (docs/roadmap.sexp, `takeover-supervision`), sitting above the store lock (layer 1), placement (layer 2), and
// handover (layer 3).
//
// `launchd` (macOS LaunchAgent) and `systemd` (Linux/WSL2 user unit) run:
//   `<server dir>/current/bin/nexus-server supervise --dir <server dir>`
//
// Under the server's directory (server/home.h):
//   supervise/              mode 0700; its exclusive flock (store/fs.h: unlockable_filesystem and locks_kept checked,
//                           released by the OS however the process ends) keeps at most one supervisor running
//   supervise/state         durable supervision record: `build`, `crashes`, `halted`, `stopped`, `picked`, and `shun`
//                           (builds already rolled back from so a dual-broken pair never ping-pongs across restarts)
//   supervise/.state-*      interrupted state write, removed under the lock after syncing `supervise/`

#pragma once

#include "server/builds.h"
#include "server/drain.h"
#include "server/handover.h"
#include "server/supervise_state.h"
#include "store/fs.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nexus::server::supervision {

// Exit statuses of `nexus-server supervise`.
constexpr int kExitOk = 0;        // stopped cleanly (`nexus-server stop` or SIGTERM/SIGINT)
constexpr int kExitFailed = 1;    // filesystem or OS failure, or locks cannot be trusted
constexpr int kExitUsage = 2;     // malformed command line
constexpr int kExitRefused = 3;   // no current build placed or current build untested (`RestartPreventExitStatus=3 78`)
constexpr int kExitBusy = 4;      // another supervisor holds `<dir>/supervise` for the whole wait
constexpr int kExitHalted = 78;   // EX_CONFIG: rollback and restart bounds exhausted (`RestartPreventExitStatus=3 78`)

constexpr std::uint32_t kDefaultMaxCrashesBeforeRollback = 2;
constexpr std::uint32_t kDefaultMaxRestarts = 5;
constexpr std::uint64_t kLockWaitMs = 5'000;
constexpr std::uint64_t kLockRetryMs = 10;
constexpr std::uint64_t kStartWaitMs = 30'000;
constexpr std::uint64_t kStableMs = 60'000;
constexpr std::uint64_t kBackoffMs = 500;
constexpr std::uint64_t kPollMs = 20;

inline const char* const kDefaultLaunchdLabel = "com.robotgamedevelopers.nexus-server";
inline const char* const kDefaultSystemdUnit = "nexus-server";

// Deterministic OS service unit paths and file contents (never invokes launchctl or systemctl).
struct ServiceUnitPaths {
    std::string launchdLabel;
    std::string launchdPlistPath;
    std::string systemdUnitName;
    std::string systemdUnitPath;
    std::string programPath;
    std::string logPath;
};

ServiceUnitPaths serviceUnitPaths(const std::string& serverDir, const std::string& home,
                                  const std::string& defaultServerDir);

std::string launchdPlist(const std::string& label, const std::string& program, const std::string& serverDir,
                         const std::string& log, const std::string& home, const std::string& path);

std::string systemdUnit(const std::string& program, const std::string& serverDir, const std::string& home,
                        const std::string& path);

// Reversed faults of the trace walk (tests/server/supervision_walk.cpp), matching `Bug` in Supervision.tla; never set
// outside tests.
enum class Fault {
    None,
    NoSupLock,
    NoHandoverLock,
    DrainStops,
    StopRestarts,
    NoPostStartRollback,
    PingPongRollback,
    NoHaltBound,
    InMemoryState,
    ServeUntested,
};

enum class ChildState { Down, Serving, Exited };

struct ChildObs {
    ChildState state = ChildState::Down;
    std::string build;
    int exitCode = 0;
};

// Seam over `<dir>/handover` lock, interrupted handover recovery, and explicit handover target notice.
class HandoverGate {
public:
    virtual ~HandoverGate() = default;
    virtual bool isLockedByOther() = 0;
    virtual store::Status tryLock(std::unique_ptr<store::FileLock>& out) = 0;
    virtual store::Status resumeIfNeeded() = 0;
    virtual bool hasExplicitPick(const std::string& build) { (void)build; return false; }
    virtual void clearExplicitPick(const std::string& build) { (void)build; }
};

// Seam over spawning, watching, draining, stopping, and reaping the supervised `nexus-server serve` process.
class ChildPort {
public:
    virtual ~ChildPort() = default;
    virtual store::Status inspect(ChildObs* out) = 0;
    virtual store::Status spawn(const std::string& expectedBuild, std::uint64_t limit_ms, ChildObs* out) = 0;
    virtual store::Status drainToExit() = 0;
    virtual store::Status stopToExit() = 0;
    virtual int consumeExit() = 0;
};

struct Options {
    // Called before each step (matching `pc` in Supervision.tla: lock, load, check, spawn, watch, reap, backoff,
    // rb_lock, rollback, rb_unlock, unlock) and with "idle" when supervise ends.
    std::function<void(const std::string& step)> hook;
    std::function<std::uint64_t()> now_ms;
    std::function<void(std::uint64_t)> sleep_ms;
    std::function<bool()> stop_requested;
    std::uint32_t max_crashes_before_rollback = kDefaultMaxCrashesBeforeRollback;
    std::uint32_t max_restarts = kDefaultMaxRestarts;
    std::uint64_t lock_wait_ms = kLockWaitMs;
    std::uint64_t start_wait_ms = kStartWaitMs;
    std::uint64_t stable_ms = kStableMs;
    std::uint64_t backoff_ms = kBackoffMs;
    std::uint64_t poll_ms = kPollMs;
    bool clear_stopped_on_start = false;
    bool* service_stopped = nullptr;
    bool* service_halted = nullptr;
    std::string test_fs_type;
    bool test_locks_ignored = false;
    Fault fault = Fault::None;
};

struct Result {
    int exit = kExitOk;
    std::vector<std::string> out;
    std::vector<std::string> err;
};

class SupervisorLoop {
public:
    SupervisorLoop(store::Fs& fs, handover::PlacementPort& placement, HandoverGate& handoverGate, ChildPort& child,
                   std::string superviseDir, Options options = {});
    ~SupervisorLoop();
    SupervisorLoop(const SupervisorLoop&) = delete;
    SupervisorLoop& operator=(const SupervisorLoop&) = delete;

    int run(Result& r);
    int status(Result& r);

    const std::string& phase() const { return phase_; }
    bool holdsLock() const { return locked_; }
    bool holdsHandoverLock() const { return rbLocked_; }

    struct Run;

private:
    friend struct Run;
    store::Fs& fs_;
    handover::PlacementPort& placement_;
    HandoverGate& handoverGate_;
    ChildPort& child_;
    std::string superviseDir_;
    Options options_;
    std::string phase_ = "idle";
    bool locked_ = false;
    bool rbLocked_ = false;
};

std::unique_ptr<HandoverGate> makePosixHandoverGate(const std::string& serverDir,
                                                    const std::function<std::uint64_t()>& now_ms = {},
                                                    const std::function<void(std::uint64_t)>& sleep_ms = {});
std::unique_ptr<ChildPort> makePosixChildPort(const std::string& serverDir,
                                              const std::function<std::uint64_t()>& now_ms = {},
                                              const std::function<void(std::uint64_t)>& sleep_ms = {});

// `nexus-server supervise [--start | status | unit [--kind systemd|launchd] [--home HOME] [--path PATH] | help]`
int command(const std::string& serverDir, const std::string& home, const std::vector<std::string>& args,
            const Options& options, Result& r);
std::string help(const std::string& verb);

} // namespace nexus::server::supervision
