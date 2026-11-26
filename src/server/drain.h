// Drain (server/drain.h): how a running nexus-server finishes its open turns before handing over to a placed build
// (docs/builds.md; models/handover/Handover.tla). Pure: no OS, no sockets, no clock.
//
// When `drain` is asked on the control socket, the server keeps serving its windows and letting every open AI turn
// (and any driver starting for one) run to its normal end, up to `limit_ms` (kDrainMs by default). As soon as no turn
// is open, or once `limit_ms` passes, the server closes its control socket, ends its drivers cleanly, and exits with
// kDrainExit (EX_TEMPFAIL, 75), which a service restarts on the current build and which `handover` waits for.
//
// While draining, `status` still answers `SERVING` on its first line (so no starting server mistakes a draining one
// with open turns for dead: models/store/StoreLock.tla) and adds ` draining` to its second line (`build <hash>
// draining`).

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace nexus::server {

// Exit status of a server that finished draining: EX_TEMPFAIL, distinct from `stop` (0), failure (1), usage (2), and a
// stuck store lock (3). A supervisor restarts EX_TEMPFAIL on `current/bin/nexus-server`, while leaving `stop` (0)
// stopped.
constexpr int kDrainExit = 75;

// Default bound on how long a draining server waits for open turns to finish before stopping anyway (10 minutes).
constexpr std::uint64_t kDrainMs = 10 * 60 * 1000;

// Environment seam for functional tests to shorten a server's drain deadline (milliseconds).
constexpr const char* kDrainLimitVariable = "NEXUS_DRAIN_MS";

class Drain {
public:
    explicit Drain(std::uint64_t limit_ms = kDrainMs) : limit_ms_(limit_ms) {}

    // Marks the server draining from `now_ms`. A second request keeps the first request's deadline.
    void request(std::uint64_t now_ms);
    bool draining() const { return draining_; }
    // Draining, and the deadline has passed.
    bool late(std::uint64_t now_ms) const { return draining_ && now_ms >= deadline_ms_; }
    // Whether the server stops now: draining, and either no turn is open (`busy` false) or the deadline has passed.
    bool stopNow(bool busy, std::uint64_t now_ms) const { return draining_ && (!busy || late(now_ms)); }
    // Milliseconds remaining until the deadline, or nothing when not draining.
    std::optional<std::uint64_t> untilDeadline(std::uint64_t now_ms) const;

private:
    std::uint64_t limit_ms_ = kDrainMs;
    bool draining_ = false;
    std::uint64_t deadline_ms_ = 0;
};

// What `status` on the control socket tells a caller about the running server.
struct ServerSeen {
    bool serving = false;   // `status` answered `SERVING`
    bool legacy = false;    // `status` answered one line only (a server from before builds were placed)
    std::string build;      // the placed build hash, or "" for `build none` or `legacy`
    bool draining = false;  // the server has accepted `drain` and is finishing its open turns
};

// The second line of `status` on the control socket: `build <hash|none>`, with ` draining` appended while draining.
std::string statusBuildLine(const std::string& build, bool draining);

// Parses the reply to `status`: `first` is the first line, and `second` (present when `hasSecond`) is the second.
// Returns true and fills `out` when `first` is `SERVING` and `second` is absent (`legacy`) or well-formed.
bool parseStatusLines(const std::string& first, const std::string& second, bool hasSecond, ServerSeen* out);

} // namespace nexus::server
