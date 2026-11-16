// Durable supervision state record in `<server dir>/supervise/state` (docs/builds.md;
// models/supervision/Supervision.tla), shared between `handover` (which clears `stopped`/`halted` and unshuns an
// explicitly chosen build) and `supervision` (which persists crash counts, shunned builds, and halt/stop status).

#pragma once

#include "store/fs.h"

#include <cstdint>
#include <set>
#include <string>

namespace nexus::server::supervision {

inline const char* const kSuperviseDir = "supervise";
inline const char* const kStateFile = "state";
inline const char* const kStateTmpPrefix = ".state-";

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

// Updates `<superviseDir>/state` on `fs` only when `superviseDir` already exists:
//   - noteHandoverBegin: clears `stopped` and `halted` at the start of an explicit `handover <target>`.
//   - noteHandoverTarget: sets `build = target`, `picked = target`, `crashes = 0`, `halted = false`,
//     `stopped = false`, and removes `target` from `shunned` when `handover <target>` reaches SERVING.
//   - noteIntentionalStop: sets `stopped = true` when `nexus-server stop` is accepted.
store::Status noteHandoverBegin(store::Fs& fs, const std::string& superviseDir);
store::Status noteHandoverTarget(store::Fs& fs, const std::string& superviseDir, const std::string& target);
store::Status noteIntentionalStop(store::Fs& fs, const std::string& superviseDir);
void noteIntentionalStop(const std::string& serverDir);

} // namespace nexus::server::supervision
