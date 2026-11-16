// Placement: installed builds are immutable, complete, and switched atomically (docs/builds.md;
// models/placement/Placement.tla). Layer 2 of the takeover (docs/roadmap.sexp): it places builds and switches between
// them, and never starts, stops or signals a server, writes a service file or touches the store.
//
// Under the server's directory (server/home.h):
//   builds/<hash>/          a build: bin/nexus-server, drivers/, formats, nexus.app/ (where placed) and manifest,
//                           named by the hash of everything in it but the manifest (build_manifest.h treeHash), and
//                           read-only once it has its name: never modified again
//   builds/pair-<c>-<p>/    a pair: the links current -> ../<c> and previous -> ../<p> (pair-<c>-none: no previous)
//   builds/active           the pointer: a link to the pair in force, replaced by one rename
//   builds/.tmp-*, .pair-*, .active-*, .gone-*
//                           what an interrupted run leaves; removed by the next run under the lock
//   current                 -> builds/active/current: the stable path (the program link on the PATH, the drivers
//                           file's driver path and the application link go through it)
//
// Every verb that changes anything takes the builds directory's lock (flock LOCK_EX on its own descriptor, released by
// the OS however the process ends, checked where it lives as the store's is: store/fs.h locks_kept), syncs the
// directory, removes the leftovers, and then takes its steps, each one named by its phase in the model. The readers
// (list, verify) take it shared. A running server holds its own build folder's lock shared for its life
// (holdBuild), and prune removes only a folder whose lock it can take exclusive.

#pragma once

#include "server/build_manifest.h"
#include "store/status.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nexus::server::builds {

// The verbs' exit statuses.
constexpr int kExitOk = 0;        // done, or nothing to do (said so)
constexpr int kExitFailed = 1;    // a filesystem call failed, a build on disk does not match its manifest, or the
                                  // builds directory is where locks cannot be trusted
constexpr int kExitUsage = 2;     // the command line does not read
constexpr int kExitRefused = 3;   // a rule refused it (untested, formats, not placed, in use, a tree a build cannot be)
constexpr int kExitBusy = 4;      // the builds lock was held by another run for the whole wait

constexpr std::uint64_t kLockWaitMs = 60000;     // a verb's wait for the builds lock
constexpr std::uint64_t kLockRetryMs = 10;
constexpr std::uint64_t kHoldWaitMs = 2000;      // a server's wait for its own build folder's shared lock
constexpr std::size_t kDefaultKeep = 2;          // prune keeps current, previous and this many more

// The reversed faults of the trace walk (tests/server/builds_walk.cpp), each a break a review of an earlier design
// found; never set but by the walk. NoLock: no builds lock. NoSync: nothing synced. TestedProgramOnly: the hash (the
// name and what evidence is checked against) leaves out the driver tree. ReuseTmp: a half-placed folder is kept and
// carried on with. PruneInUse: prune takes no lock on the folder it removes. TwoRenames: a switch renames previous,
// then current, as two pointer changes.
enum class Fault {
    None,
    NoLock,
    NoSync,
    TestedProgramOnly,
    ReuseTmp,
    PruneInUse,
    TwoRenames,
    BlindBack,
    MultiHopNoStoreCheck
};

struct Options {
    // Called before each step a mutating verb takes, with its name, which is then phase() (the model's pc: lock,
    // settle, clean, mktmp, write, manifest, sync, verify, rename, syncdir, drop, unlock, check, pmk, psync, prename,
    // pdsync, anew, arename, async, plist, ptry, pgone, pgsync, premove), and with "idle" once the verb has ended. The
    // tests kill the process there, or hold the thread.
    std::function<void(const std::string& step)> hook;
    // The clock and the pause of the lock's wait (the steady clock and a real sleep when empty). The pause follows
    // each try of a lock held by another run: the trace walk holds the thread there too.
    std::function<std::uint64_t()> now_ms;
    std::function<void(std::uint64_t)> sleep_ms;
    std::uint64_t lock_wait_ms = kLockWaitMs;
    // Seams: writes past this many bytes in one placement fail as a full disk does (ENOSPC); 0 never. The store lock's
    // filesystem seams (store/fs.h PosixOptions).
    std::uint64_t test_disk_full_after = 0;
    std::string test_fs_type;
    bool test_locks_ignored = false;
    Fault fault = Fault::None;
};

// What a verb said: lines for standard output and for standard error, and its exit status.
struct Result {
    int exit = kExitOk;
    std::vector<std::string> out;
    std::vector<std::string> err;
};

// The verbs on the builds under one server directory, for one process. Each returns its exit status (also in the
// Result). phase() is the step the run takes next ("idle" between verbs), for the trace walk.
class Builds {
public:
    Builds(std::string serverDir, Options options);
    ~Builds();
    Builds(const Builds&) = delete;
    Builds& operator=(const Builds&) = delete;

    // Places the tree at `tree` (a directory holding bin/nexus-server, formats, and drivers/ and nexus.app/ where
    // they are placed), with the evidence file at `evidence` ("" for none: the build is placed untested). Prints
    // `placed <hash> tested|untested`, or `already placed <hash> ...`.
    int place(const std::string& tree, const std::string& evidence, Result& r);
    // Makes the build `hash` current, the current build previous, in one rename. `allowUntested` is only ever true for
    // handover_walk's reversed fault.
    int makeCurrent(const std::string& hash, Result& r, bool allowUntested = false);
    // Reads the pointer (`builds/active`) under the builds directory's shared lock: empty strings when no build is
    // current yet.
    int active(std::string* current, std::string* previous, Result& r);
    // Checks under the builds directory's shared lock, without changing anything, whether `hash` can be made current
    // (placed, whole, tested unless `allowUntested`, and compatible with the current build's formats).
    int checkSwitch(const std::string& hash, Result& r, bool allowUntested = false);
    // Makes previous current, and current previous.
    int back(Result& r);
    // Removes every build but current, previous, those in use and the `keep` most recently placed of the rest.
    int prune(std::size_t keep, Result& r);
    // Removes the one build `hash` (not current, previous or in use).
    int remove(const std::string& hash, Result& r);
    // One line per build, newest last: `<hash> current|previous|- tested|untested in-use|- <formats>`.
    int list(Result& r);
    // Every build (or the one `hash`) read back against its manifest, and the pointer; `ok ...` or `damaged ...`.
    int verify(const std::string& hash, Result& r);

    const std::string& phase() const { return phase_; }
    // Whether this run holds the builds lock, as it believes (the trace walk compares it with the system's answer).
    bool holdsLock() const { return locked_; }

    struct Run;

private:
    friend struct Run;
    std::string serverDir_;
    Options options_;
    std::string phase_ = "idle";
    bool locked_ = false;
};

// The hash of the tree at `dir` as place would name it (for writing evidence after its tests), with `fault`'s hash
// (Fault::TestedProgramOnly leaves out drivers/). Ok and `hash`, or why the tree cannot be a build.
store::Status hashTree(const std::string& dir, std::string* hash, Fault fault = Fault::None);

// The build `hash` under the builds directory `buildsDir` read back against its manifest (every entry's bytes, kind and
// mode, nothing more, and the manifest's tree its name), without the builds lock: what `verify` does under it, and what
// the trace walk observes between steps. Ok and the manifest; NotFound when it is not placed; else Failed, with why.
store::Status checkBuild(const std::string& buildsDir, const std::string& hash, Manifest* manifest,
                         Fault fault = Fault::None);

// The formats file of this build (`nexus-server builds formats`, which the tree's assembly writes to <tree>/formats):
// the store's record and snapshot formats (store/store.h), the config files' version (accounts/registry.h), the link's
// state format and oldest peer (sync/formats.h), and the driver protocol (driver/protocol.h).
std::string thisBuildFormats();

// `nexus-server builds <verb> ...` with the verbs' help: the whole command line after `builds`. Exit status as above.
int command(const std::string& serverDir, const std::vector<std::string>& args, const Options& options, Result& r);
// The help of one verb (`place`, `hash`, `list`, `verify`, `current`, `back`, `prune`, `remove`), or of all ("" or
// `help`): what it does step by step, what it prints and its exit statuses. Empty for anything else.
std::string help(const std::string& verb);

// A running server's hold on the build it was started from, for its life: the build folder's lock, shared.
class BuildHold {
public:
    BuildHold() = default;
    ~BuildHold();
    BuildHold(const BuildHold&) = delete;
    BuildHold& operator=(const BuildHold&) = delete;
    BuildHold(BuildHold&& other) noexcept;
    BuildHold& operator=(BuildHold&& other) noexcept;
    const std::string& hash() const { return hash_; }
    void release();

private:
    friend store::Status holdBuild(const std::string&, BuildHold&, const std::function<std::uint64_t()>&,
                                   const std::function<void(std::uint64_t)>&, std::uint64_t);
    int fd_ = -1;
    std::string hash_;
};

// Holds the build the program at `program` (its resolved path) is in: its folder opened (O_NOFOLLOW), its lock taken
// shared (tried every kLockRetryMs for `wait_ms`: prune holds it exclusive for the moment of renaming it aside), and
// the folder checked to be still the one under its hash name. Ok with an empty hash() when the program is not in a
// placed build (a build tree). Failed, in one line, when its build is being removed or was removed: the server does
// not start then. Clock and pause as in Options (the real ones when empty).
store::Status holdBuild(const std::string& program, BuildHold& out, const std::function<std::uint64_t()>& now_ms = {},
                        const std::function<void(std::uint64_t)>& sleep_ms = {}, std::uint64_t wait_ms = kHoldWaitMs);

} // namespace nexus::server::builds
