// When the window's link runs its bootstrap again (link/client.h): the decision alone, apart from the link, so that it
// runs in unit tests and in the walk TLC checks against models/Reconnect.tla. Time is an argument: milliseconds of any
// one clock that never goes back. No OS, no time of its own.
//
// The link holds one session at a time, of the server its last bootstrap reached. That session is lost when the server
// ends it (mosh's shutdown, which a server sends as it stops or hands over) or when it has not been heard for
// kQuietMs (a server that crashed, or a network that is down: the cursor is grey from the same moment, sync/client.h).
// A link with no session, or a lost one, runs its bootstrap again as soon as the backoff allows: at once after a session
// it heard, then kRedialFirstMs after an attempt that reached no server, doubling to kRedialMaxMs and staying there, so
// that a server that comes up is reached within kRedialMaxMs and a machine with none is asked at most once per
// kRedialMaxMs. A session made is an attempt too until its server is heard, so a server that answers the bootstrap but
// is never heard (UDP blocked) is asked no faster.
//
// Only a link that has never heard a server starts one (nexus-server new starts the server when none runs: the window's
// launch). Once it has, it asks only a running server: a replacement build comes up by the service or handover, and a
// server stopped on purpose stays stopped.

#pragma once

#include <cstdint>

namespace nexus::sync {

using RedialMs = std::uint64_t;

// The wait after the first attempt that reached no server, and the longest wait (models/Reconnect.tla's bounded
// backoff). Chosen, not measured: a restarted or replacement server answers within about a second of the old one's
// exit, so the longest wait bounds how late the window is after that.
constexpr RedialMs kRedialFirstMs = 250;
constexpr RedialMs kRedialMaxMs = 4000;

// The wait after `attempts` attempts in a row that heard no server: 0 for none, else kRedialFirstMs doubled for each
// attempt after the first, at most kRedialMaxMs.
RedialMs redialBackoffMs(unsigned attempts);

class Redial {
public:
    // A bootstrap made a new session at `now`, not yet heard: its silence counts from now. Any earlier session is gone.
    void dialed(RedialMs now);
    // A bootstrap failed at `now` (ssh, no server running, a CONNECT line that does not parse): the session held, if
    // any, is kept.
    void failed(RedialMs now);
    // The session's server was heard at `at` (its newest state arrived then): no wait before the next attempt, and the
    // attempts start again from none. Ignored with no session, an ended session, or a state no newer than the last one
    // heard.
    void heard(RedialMs at);
    // The server ended the session (mosh's shutdown): lost at once. Ignored with no session.
    void ended();

    bool session() const { return session_; }
    bool serverEnded() const { return ended_; }
    // A session held and not heard for kQuietMs.
    bool quiet(RedialMs now) const;
    // No session, or the one held is lost (ended, or quiet).
    bool lost(RedialMs now) const;
    // The backoff after the latest attempt has passed.
    bool timerDue(RedialMs now) const { return now >= nextAt_; }
    // Run the bootstrap now.
    bool due(RedialMs now) const { return lost(now) && timerDue(now); }
    // How long until due() holds with time alone: 0 when it holds now. With a session that is neither lost nor heard
    // again, the moment it turns quiet, or the backoff's end if that is later.
    RedialMs waitMs(RedialMs now) const;
    // The next attempt may start a server: only until a server has been heard.
    bool mayStart() const { return !everHeard_; }
    unsigned attempts() const { return attempts_; }

private:
    void attempted(RedialMs now);

    bool session_ = false;
    bool heard_ = false;
    bool ended_ = false;
    bool everHeard_ = false;
    RedialMs heardAt_ = 0;     // the session's last hearing, or its making
    RedialMs nextAt_ = 0;      // the earliest the next attempt runs
    unsigned attempts_ = 0;    // in a row, since a server was last heard
};

} // namespace nexus::sync
