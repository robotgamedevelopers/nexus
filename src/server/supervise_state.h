// Durable supervision state in `<server dir>/supervise/` (docs/builds.md; models/supervision/Supervision.tla):
//   supervise/state    written under `<dir>/supervise`'s exclusive flock (`build`, `crashes`, `halted`, `shun`)
//   supervise/stopped  atomic marker created by `nexus-server stop` / `Reap(0)` and cleared only by `--start` or an
//                      explicit `handover`, so background `state` writes never clobber an intentional stop
//   supervise/picked   atomic marker (`<hash>\n`) written by an explicit `handover <target>` under `<dir>/handover`'s
//                      lock and consumed by `SupervisorLoop` once `<dir>/handover` is unlocked, so reaping the drained
//                      previous server never clobbers the un-shunned target pick

#pragma once

#include "store/fs.h"

#include <cstdint>
#include <set>
#include <string>

namespace nexus::server::supervision {

inline const char* const kSuperviseDir = "supervise";
inline const char* const kStateFile = "state";
inline const char* const kStateTmpPrefix = ".state-";
inline const char* const kStoppedFile = "stopped";
inline const char* const kStoppedTmpPrefix = ".stopped-";
inline const char* const kPickedFile = "picked";
inline const char* const kPickedTmpPrefix = ".picked-";

// Durable record in `<server dir>/supervise/state`:
//   nexus-supervise 1
//   build <hash|none>
//   crashes <n>
//   halted <0|1>
//   stopped <0|1>
//   picked <hash|none>
//   shun <hash>
//   ...
//   end
struct SuperviseState {
    std::string build;
    std::uint32_t crashes = 0;
    bool halted = false;
    bool stopped = false;
    std::string picked;
    std::set<std::string> shunned;
    bool operator==(const SuperviseState&) const = default;
};

std::string formatState(const SuperviseState& st);
bool readState(const std::string& text, SuperviseState* out, std::string* why);

store::Status writeStateFile(store::Fs& fs, const std::string& superviseDir, const SuperviseState& st);

store::Status writeStoppedMarker(store::Fs& fs, const std::string& superviseDir);
store::Status clearStoppedMarker(store::Fs& fs, const std::string& superviseDir);
bool hasStoppedMarker(store::Fs& fs, const std::string& superviseDir);

store::Status writePickedBuild(store::Fs& fs, const std::string& superviseDir, const std::string& target);
bool hasPickedBuild(store::Fs& fs, const std::string& superviseDir, const std::string& build);
store::Status clearPickedBuild(store::Fs& fs, const std::string& superviseDir, const std::string& build);

// Reads `supervise/state` (or default when absent) and overlays `supervise/stopped` and `supervise/picked`.
store::Status loadEffectiveState(store::Fs& fs, const std::string& superviseDir, SuperviseState* out);

// Cross-process updates under `<superviseDir>` when `superviseDir` already exists:
//   - noteHandoverBegin: clears `supervise/stopped` and `halted` at the start of an explicit `handover <target>`.
//   - noteHandoverTarget: clears `supervise/stopped`, writes `supervise/picked` = `target`, and updates `state`.
//   - noteIntentionalStop: writes `supervise/stopped` atomically without touching `supervise/state`.
store::Status noteHandoverBegin(store::Fs& fs, const std::string& superviseDir);
store::Status noteHandoverTarget(store::Fs& fs, const std::string& superviseDir, const std::string& target);
store::Status noteIntentionalStop(store::Fs& fs, const std::string& superviseDir);
void noteIntentionalStopAt(const std::string& superviseDir);
void noteIntentionalStop(const std::string& serverDir);

} // namespace nexus::server::supervision
