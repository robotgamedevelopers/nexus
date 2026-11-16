#include "terminal_watch.h"

#include <utility>

namespace nexus {

void takeForegroundWatch(const ForegroundSeen& shared, ForegroundSeen& seen) {
    if (shared.seq > seen.seq) {
        seen = shared;
    }
}

bool foregroundNeedsArguments(const ForegroundSeen& seen, int group) {
    return seen.seq == 0 || group != seen.group;
}

bool commitFrameForeground(ForegroundSeen& shared, ForegroundSeen& seen, int group, std::string remote) {
    const bool overlapped = shared.seq != seen.seq;
    const std::uint64_t next = (shared.seq > seen.seq ? shared.seq : seen.seq) + 1;
    seen = ForegroundSeen{next, group, std::move(remote)};
    shared = seen;
    return overlapped;
}

bool pollNeedsArguments(const ForegroundSeen& snapshot, int group) {
    return snapshot.seq == 0 || group != snapshot.group;
}

PollCommit commitPollForeground(ForegroundSeen& shared, const ForegroundSeen& snapshot, int group, std::string remote) {
    if (shared.seq != snapshot.seq) {
        return PollCommit{false, true};
    }
    if (!pollNeedsArguments(snapshot, group)) {
        return PollCommit{false, false};
    }
    shared = ForegroundSeen{snapshot.seq + 1, group, std::move(remote)};
    return PollCommit{true, false};
}

} // namespace nexus
