// What the watch's one screen shows (watch/app/NexusWatchApp.swift draws it): the status line and the problem line,
// from the turn's state. Decided here, not in SwiftUI, so the unit tests hold it, the still-frames rule included.
//
// Still frames: a fresh launch whose phone reaches nexus-server settles at once. Until the watch has reached the
// server once, or kSettleMs have passed since launch, a disconnected turn is drawn as the idle screen, with no "No
// server" line: on a healthy start the first frame is already the last, and nothing flashes while the phone's session
// and link start. A link lost after that, or still down after kSettleMs, shows at once.

#ifndef NEXUS_WATCH_SCREEN_H
#define NEXUS_WATCH_SCREEN_H

#include <cstdint>
#include <string>

#include "voice_turn.h"

namespace nexus::watch {

// How long a fresh launch may take to reach the server before "No server" is shown: a few of the app's half-second
// exchanges with the phone, and the phone's own link coming up.
inline constexpr std::uint64_t kSettleMs = 4000;

struct ScreenIn {
    Phase phase = Phase::Disconnected;
    bool settled = false;       // reached the server once since launch, or kSettleMs passed
    std::string current;        // the AI talked to
    std::string replyFrom;      // the AI whose reply is read
    std::string why;            // Disconnected: why
    std::string notice;         // the last notice, until a reply or a new turn
    std::string lockWhy;        // why the owner's check refused, if it did
    bool unlocked = false;
};

struct Screen {
    std::string status;
    std::string problem;
    bool operator==(const Screen&) const = default;
};

Screen screenOf(const ScreenIn& in);

} // namespace nexus::watch

#endif
