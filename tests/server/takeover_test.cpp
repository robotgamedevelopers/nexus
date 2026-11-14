// Unit tests of the takeover rules (src/server/takeover.h): the installer's step for everything it can see, the drain's
// stop with and without a turn open and at its deadline, the `build` reply both ways, and the build of a program's
// bytes. Logic only: time is an argument, and nothing runs.

#include <cstdio>
#include <string>

#include "server/takeover.h"

namespace {

using namespace nexus::server;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

void steps() {
    Seen s;
    s.placed = "b";
    // Nothing answers: start the placed program.
    CHECK(nextStep(s) == Step::Start);
    // A server from before takeover answers, but not `build`: stop it.
    s.answers = true;
    CHECK(nextStep(s) == Step::AskStop);
    // Another build: drain it; once draining, wait.
    s.knowsBuild = true;
    s.build = "a";
    CHECK(nextStep(s) == Step::AskDrain);
    s.draining = true;
    CHECK(nextStep(s) == Step::Wait);
    // The placed build runs: done, draining or not (a drained server comes back as the same build).
    s.build = "b";
    CHECK(nextStep(s) == Step::Done);
    s.draining = false;
    CHECK(nextStep(s) == Step::Done);
    // Older or newer does not matter: the placed build is the one that runs.
    s.placed = "0";
    CHECK(nextStep(s) == Step::AskDrain);
    CHECK(std::string(stepName(Step::AskDrain)) == "ask-drain" && std::string(stepName(Step::Done)) == "done");
}

void drain() {
    Drain d;
    const Millis t = 1000 * 1000;
    // Not asked: never stops, has no deadline.
    CHECK(!d.draining() && !d.stopNow(false, t) && !d.untilDeadline(t));
    d.request(t);
    CHECK(d.draining());
    // A turn open holds it until the deadline; with none, it stops at once.
    CHECK(!d.stopNow(true, t) && d.stopNow(false, t));
    CHECK(!d.stopNow(true, t + kDrainMs - 1) && !d.late(t + kDrainMs - 1));
    CHECK(*d.untilDeadline(t + 1) == kDrainMs - 1);
    // At the deadline it stops, the turn open or not.
    CHECK(d.late(t + kDrainMs) && d.stopNow(true, t + kDrainMs) && *d.untilDeadline(t + kDrainMs + 5) == 0);
    // A second request keeps the first deadline.
    d.request(t + 5000);
    CHECK(d.stopNow(true, t + kDrainMs));
}

void replies() {
    Seen s;
    CHECK(buildReply("0123456789abcdef", false) == "BUILD 0123456789abcdef");
    CHECK(buildReply("0123456789abcdef", true) == "BUILD 0123456789abcdef draining");
    CHECK(parseBuildReply("BUILD 0123456789abcdef", &s) && s.knowsBuild && s.build == "0123456789abcdef" &&
        !s.draining);
    CHECK(parseBuildReply("BUILD 0123456789abcdef draining", &s) && s.build == "0123456789abcdef" && s.draining);
    // What a server from before takeover answers, and anything else, is not a build.
    Seen old;
    CHECK(!parseBuildReply("ERROR unknown request; the requests are `new`", &old) && !old.knowsBuild);
    CHECK(!parseBuildReply("BUILD ", &old) && !parseBuildReply("BUILD a b", &old) && !parseBuildReply("OK", &old));
}

void builds() {
    // FNV-1a 64: the empty input is the offset basis; one byte changes the build.
    CHECK(buildOf("") == "cbf29ce484222325");
    CHECK(buildOf("a") == "af63dc4c8601ec8c");
    CHECK(buildOf("nexus-server 1") != buildOf("nexus-server 2"));
    CHECK(buildOf(std::string(1 << 16, 'x')).size() == 16);
}

} // namespace

int main() {
    steps();
    drain();
    replies();
    builds();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("takeover_test: ok\n");
    return 0;
}
