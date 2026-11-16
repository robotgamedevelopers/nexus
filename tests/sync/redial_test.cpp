// Unit tests of the reconnect decision (src/sync/redial.h): when the window's link runs its bootstrap again, the
// backoff's lengths and bound, what a session heard, ended or quiet does, how long the link thread may sleep, and when
// an attempt may start a server. Logic only: time is an argument (from 1000 s), and nothing runs.

#include <cstdio>

#include "sync/client.h"
#include "sync/redial.h"

namespace {

using namespace nexus::sync;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr RedialMs kStart = 1000 * 1000;

void backoff() {
    CHECK(redialBackoffMs(0) == 0);
    CHECK(redialBackoffMs(1) == kRedialFirstMs);
    CHECK(redialBackoffMs(2) == 2 * kRedialFirstMs);
    CHECK(redialBackoffMs(3) == 4 * kRedialFirstMs);
    // Bounded: it never passes kRedialMaxMs, however many attempts.
    for (unsigned k = 1; k < 200; ++k) {
        CHECK(redialBackoffMs(k) <= kRedialMaxMs);
        CHECK(redialBackoffMs(k) >= redialBackoffMs(k - 1));
    }
    CHECK(redialBackoffMs(~0u) == kRedialMaxMs);
}

// At launch: due at once, and allowed to start a server.
void launch() {
    Redial r;
    CHECK(!r.session());
    CHECK(r.lost(kStart));
    CHECK(r.due(kStart));
    CHECK(r.waitMs(kStart) == 0);
    CHECK(r.mayStart());
    // The first attempt fails (ssh, say): the next waits kRedialFirstMs, then twice that.
    r.failed(kStart);
    CHECK(!r.due(kStart));
    CHECK(r.waitMs(kStart) == kRedialFirstMs);
    CHECK(!r.due(kStart + kRedialFirstMs - 1));
    CHECK(r.due(kStart + kRedialFirstMs));
    r.failed(kStart + kRedialFirstMs);
    CHECK(r.waitMs(kStart + kRedialFirstMs) == 2 * kRedialFirstMs);
    CHECK(r.mayStart());
}

// A session made and heard: nothing due, and the thread sleeps until it could turn quiet.
void heardSession() {
    Redial r;
    r.dialed(kStart);
    CHECK(r.session());
    CHECK(!r.lost(kStart));
    CHECK(!r.due(kStart));
    r.heard(kStart + 10);
    CHECK(!r.mayStart());
    CHECK(r.attempts() == 0);
    CHECK(r.timerDue(kStart + 10));
    CHECK(!r.due(kStart + 10));
    CHECK(r.waitMs(kStart + 10) == kQuietMs);
    // Heard again and again: never quiet while it is heard within kQuietMs.
    r.heard(kStart + 5000);
    CHECK(!r.quiet(kStart + 5000 + kQuietMs - 1));
    CHECK(r.waitMs(kStart + 6000) == kQuietMs - 1000);
    // A hearing older than the last one changes nothing.
    r.heard(kStart + 100);
    CHECK(!r.quiet(kStart + 5000 + kQuietMs - 1));
}

// The server ends the session (a clean stop): due at once, with no wait, and the attempt asks only a running server.
void endedByServer() {
    Redial r;
    r.dialed(kStart);
    r.heard(kStart + 20);
    r.ended();
    CHECK(r.serverEnded());
    CHECK(r.lost(kStart + 30));
    CHECK(r.due(kStart + 30));
    CHECK(r.waitMs(kStart + 30) == 0);
    CHECK(!r.mayStart());
    // The next server is not up yet: the attempt fails and the next waits; then it is, and the session made goes on.
    r.failed(kStart + 30);
    CHECK(!r.due(kStart + 30 + kRedialFirstMs - 1));
    CHECK(r.due(kStart + 30 + kRedialFirstMs));
    r.dialed(kStart + 30 + kRedialFirstMs);
    CHECK(!r.serverEnded());
    CHECK(!r.lost(kStart + 30 + kRedialFirstMs));
    r.heard(kStart + 400);
    CHECK(r.attempts() == 0);
    CHECK(!r.due(kStart + 400));
}

// A server that crashed tells nothing: the session is lost after kQuietMs of silence, not before.
void quietSession() {
    Redial r;
    r.dialed(kStart);
    r.heard(kStart);
    CHECK(!r.quiet(kStart + kQuietMs - 1));
    CHECK(!r.due(kStart + kQuietMs - 1));
    CHECK(r.waitMs(kStart + kQuietMs - 1) == 1);
    CHECK(r.quiet(kStart + kQuietMs));
    CHECK(r.due(kStart + kQuietMs));
    // A late hearing (the network came back) makes it not quiet again.
    r.heard(kStart + kQuietMs + 5);
    CHECK(!r.lost(kStart + kQuietMs + 5));
}

// A session made whose server is never heard (UDP blocked, say) is an attempt too: the attempts back off as failures
// do, and stay bounded.
void neverHeard() {
    Redial r;
    RedialMs now = kStart;
    for (unsigned k = 1; k <= 12; ++k) {
        CHECK(r.due(now));
        r.dialed(now);
        CHECK(r.attempts() == k);
        // Lost only once quiet; due only once the backoff has passed too.
        now += kQuietMs;
        CHECK(r.lost(now));
        CHECK(r.due(now) == (kQuietMs >= redialBackoffMs(k)));
        now += r.waitMs(now);
        CHECK(r.due(now));
    }
    CHECK(r.waitMs(now) == 0);
    CHECK(r.mayStart());
}

// No session: hearing and ending change nothing.
void noSession() {
    Redial r;
    r.heard(kStart);
    r.ended();
    CHECK(!r.session());
    CHECK(!r.serverEnded());
    CHECK(r.mayStart());
    CHECK(r.due(kStart));
}

// A failed attempt keeps the session held: still quiet, still due once the backoff passes.
void failedKeepsSession() {
    Redial r;
    r.dialed(kStart);
    r.heard(kStart);
    const RedialMs lostAt = kStart + kQuietMs;
    CHECK(r.due(lostAt));
    r.failed(lostAt);
    CHECK(r.session());
    CHECK(r.quiet(lostAt));
    CHECK(!r.due(lostAt));
    CHECK(r.waitMs(lostAt) == kRedialFirstMs);
    CHECK(r.due(lostAt + kRedialFirstMs));
}

// An ended session kept across a failed dial ignores any hearing reported for it: the backoff is never wiped.
void endedIgnoresHeard() {
    Redial r;
    r.dialed(kStart);
    r.heard(kStart + 20);
    r.ended();
    r.failed(kStart + 30);
    CHECK(r.attempts() == 1);
    CHECK(r.waitMs(kStart + 30) == kRedialFirstMs);
    r.heard(kStart + 20);
    r.heard(kStart + 35);
    CHECK(r.serverEnded());
    CHECK(r.attempts() == 1);
    CHECK(!r.due(kStart + 30));
    CHECK(r.waitMs(kStart + 30) == kRedialFirstMs);
}

// Reporting the already-heard state's timestamp again after a failed dial does not wipe the backoff; a newer state
// arriving on the kept session does.
void staleHeardKeepsBackoff() {
    Redial r;
    r.dialed(kStart);
    r.heard(kStart + 20);
    const RedialMs lostAt = kStart + 20 + kQuietMs;
    r.failed(lostAt);
    CHECK(r.attempts() == 1);
    CHECK(r.waitMs(lostAt) == kRedialFirstMs);
    r.heard(kStart + 20);
    CHECK(r.attempts() == 1);
    CHECK(!r.due(lostAt));
    CHECK(r.waitMs(lostAt) == kRedialFirstMs);
    r.heard(lostAt + 10);
    CHECK(r.attempts() == 0);
    CHECK(!r.lost(lostAt + 10));
}

} // namespace

int main() {
    backoff();
    launch();
    heardSession();
    endedByServer();
    quietSession();
    neverHeard();
    noSession();
    failedKeepsSession();
    endedIgnoresHeard();
    staleHeardKeepsBackoff();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("redial_test: ok\n");
    return 0;
}
