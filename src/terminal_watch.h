// Synchronization of one terminal's foreground process group between the terminals' background thread (which polls four
// times a second) and the frame thread (which reads the group when the terminal's title changes): models/TerminalWatch.tla.
// Pure logic: no OS, no clock, no locks; PraxTerminals holds its mutex around the shared state and runs system calls
// outside it.

#pragma once

#include <cstdint>
#include <string>

namespace nexus {

struct ForegroundSeen {
    std::uint64_t seq = 0;      // 0 before any observation; rises at each committed observation
    int group = -1;             // foreground process group leader, or -1
    std::string remote;         // remoteLabel of that group's arguments; empty when not ssh
};

struct PollCommit {
    bool changed = false;       // shared took a new foreground observation: wake the window
    bool recheck = false;       // a frame-thread observation committed while the poll ran: poll again
};

// Frame thread, under the lock: copies a newer background observation into the frame thread's `seen`.
void takeForegroundWatch(const ForegroundSeen& shared, ForegroundSeen& seen);

// Frame thread, outside the lock, when the terminal's title changed: whether `group` needs its process arguments read.
bool foregroundNeedsArguments(const ForegroundSeen& seen, int group);

// Frame thread, under the lock, after reading `group` (and `remote` when foregroundNeedsArguments said so) for a new
// title: records the observation in both `seen` and `shared`. Returns true if `shared` changed while the frame thread
// read `group`, so the background thread should poll again against the new state.
bool commitFrameForeground(ForegroundSeen& shared, ForegroundSeen& seen, int group, std::string remote);

// Background thread, outside the lock: whether a polled `group` differs from `snapshot` and needs its arguments read.
bool pollNeedsArguments(const ForegroundSeen& snapshot, int group);

// Background thread, under the lock, after polling `group` against `snapshot`: commits when `shared` is still at
// `snapshot.seq` and the group changed; asks for a recheck without overwriting `shared` when `shared.seq` moved.
PollCommit commitPollForeground(ForegroundSeen& shared, const ForegroundSeen& snapshot, int group, std::string remote);

} // namespace nexus
