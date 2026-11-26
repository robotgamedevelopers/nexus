#include "screen.h"

namespace nexus::watch {

Screen screenOf(const ScreenIn& in) {
    const bool down = in.phase == Phase::Disconnected;
    const Phase shown = down && !in.settled ? Phase::Idle : in.phase;
    Screen s;
    switch (shown) {
    case Phase::Idle: s.status = "Hold and talk to your AIs"; break;
    case Phase::Listening: s.status = "Listening…"; break;
    case Phase::Transcribing:
    case Phase::Choosing: s.status = "Hearing you…"; break;
    case Phase::Sending: s.status = "To " + in.current + "…"; break;
    case Phase::Waiting: s.status = in.current + " is thinking…"; break;
    case Phase::Speaking: s.status = in.replyFrom; break;
    case Phase::Interrupted: s.status = "Stopped"; break;
    case Phase::Disconnected: s.status = "No server"; break;
    }
    s.problem = down && in.settled && !in.why.empty() ? "No server: " + in.why : in.notice;
    if (!in.unlocked && !in.lockWhy.empty() && !s.problem.empty())
        s.problem += " (" + in.lockWhy + ")";
    return s;
}

} // namespace nexus::watch
