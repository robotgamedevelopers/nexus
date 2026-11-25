// Layer 5 of replacing a running server (docs/roadmap.sexp, takeover-migration; models/migration/Migration.tla):
//
//   1. Old-layout migration: a store, drivers file, config and server.log left at the top of ~/.nexus (before
//      ~/.nexus/server existed) move once into ~/.nexus/server, under ~/.nexus/server's exclusive OS lock, refusing
//      while an old server still serves on ~/.nexus/server.sock or holds ~/.nexus/store, refusing rather than
//      overwriting an already-populated ~/.nexus/server/store or conflicting drivers/config, and resuming cleanly after
//      a process kill or machine crash at any filesystem step without losing, tearing or duplicating any file.
//
//   2. Store and config reader inspection on disk: checks whether a placed target build can read what `<serverDir>/store`
//      and `<serverDir>/nexus-config` hold on disk (`checkDiskFormats`, used by `builds back`, write-downgrading
//      `builds current`, and `nexus-server store check --for <build>`).
//
// Pure of sockets and real time: `migrateOldLayout` runs over `store::Fs` (`MemFs` in unit tests, `PosixFs` on disk),
// and asks `Options::old_server_live` whether a pre-lock server answers on `<top>/server.sock`.

#pragma once

#include "server/build_manifest.h"
#include "store/fs.h"
#include "store/status.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace nexus::server::migration {

inline const char* const kPlanFile = ".migrating";
inline const char* const kPlanTmpFile = ".migrating.tmp";
inline const char* const kStagePrefix = ".mig-";

// Reversed faults for the trace walk (tests/server/migration_walk.cpp), each rejected by MigrationTrace.tla; never set
// by the server or tools.
enum class Fault {
    None,
    NoLock,
    IgnoreLiveOld,
    ClobberNew,
    NoSync,
    NoPlan,
    LeaveOld
};

struct Options {
    // True when a server of the old layout answers on `<top>/server.sock` (a server from before StoreLock holds no
    // flock on `<top>/store`, so only its socket shows it).
    std::function<bool()> old_server_live;
    // Called before each phase of `migrateOldLayout` with the phase name (`lock`, `probe`, `plan`, `plan_sync`,
    // `stage`, `sync_stg`, `rename`, `sync_placed`, `clean_top`, `sync_top`, `finish`, `unlock`, and `idle` when done)
    // and the part name when applicable (`snap`, `log`, `drivers`, `config`, `server.log`, or `none`).
    std::function<void(const std::string& phase, const std::string& part)> hook;
    Fault fault = Fault::None;
};

struct Report {
    bool migrated = false;              // old-layout items were moved or cleaned in this call
    bool resumed = false;               // `.migrating` from an interrupted run was resumed
    bool conflict = false;              // refused because both `top` and `dir` held conflicting data
    std::vector<std::string> moved;     // items moved into `dir` (`store`, `drivers`, `config`, `server.log`)
};

// Moves any server state left at the top of `top` (`~/.nexus`) into `dir` (`~/.nexus/server`). When `top` is empty (an
// explicit `--dir` outside `~/.nexus/server`), or holds nothing of the old server layout and `dir/.migrating` is
// absent, returns Ok immediately without locking or writing anything. Busy when another migrator holds `dir`, an old
// writer holds `top/store`, or a running server holds `dir/store`; Failed when an old server answers on
// `top/server.sock`, when both `top` and `dir` hold conflicting store/drivers/config data (leaving both untouched), or
// on an I/O or lock check failure.
store::Status migrateOldLayout(store::Fs& fs, const std::string& top, const std::string& dir,
                               const Options& options = {}, Report* report = nullptr);

// Parses the `.migrating` plan text into item names (`snap`, `log`, `drivers`, `config`, `server.log`).
std::set<std::string> parsePlan(const std::string& text);
std::string formatPlan(const std::vector<std::string>& parts);

// Reads the format versions required by `<serverDir>/store` and `<serverDir>/nexus-config/{accounts,ais}.json` over
// `fs` (without mutating anything on disk). Busy when a writer holds `<serverDir>/store`; Ok with `out` populated (0
// where nothing is on disk yet); Failed when the store or config JSON header is unreadable.
store::Status readDiskFormats(store::Fs& fs, const std::string& serverDir, builds::DiskFormats* out,
                              const std::function<std::uint64_t()>& now_ms = {});

#ifndef _WIN32
// Asks `socketPath` (`~/.nexus/server.sock`) whether a server of the old layout still serves on it: true when a live
// server answers `status` with `SERVING` or the pre-StoreLock `ERROR unknown request` refusal within `timeoutMs`; false
// when the socket is absent, stale (ECONNREFUSED), closed unanswered, or silent past `timeoutMs`.
bool probeOldServerSocket(const std::string& socketPath, int timeoutMs = 3000);
#endif

} // namespace nexus::server::migration
