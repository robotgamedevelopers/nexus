// Unit tests of the takeover rules (src/server/takeover.h): the installer's step for everything it can see, the drain's
// stop with and without a turn open and at its deadline, the `build` reply both ways, the handover's acts through a
// drain, a start, a start that never answers and its rollback, and the build of a program's bytes. Logic only: time is
// an argument, and nothing runs.

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

Seen answering(const std::string& build, bool draining) {
    Seen s;
    s.answers = true;
    s.knowsBuild = true;
    s.build = build;
    s.draining = draining;
    return s;
}

void handover() {
    const Millis t = 1000 * 1000;
    const Seen nothing;
    // The whole way: drain the old build, start the placed one once, and done when it answers.
    {
        Handover h("b", "a");
        CHECK(h.step(answering("a", false), t) == Act::AskDrain);
        CHECK(h.step(answering("a", true), t) == Act::Wait);
        CHECK(h.step(nothing, t) == Act::Start);
        // Started and not yet answering: wait, and do not start it twice.
        CHECK(h.step(nothing, t + 10) == Act::Wait);
        CHECK(h.step(answering("b", false), t + 20) == Act::Done);
        CHECK(!h.failed() && !h.rolledBack() && h.placed() == "b");
    }
    // A server from before takeover is stopped, then the placed build started.
    {
        Handover h("b", "a");
        Seen old;
        old.answers = true;
        CHECK(h.step(old, t) == Act::AskStop);
        CHECK(h.step(nothing, t) == Act::Start);
    }
    // A placed build that never answers: failed at kStartMs (not a moment before), then rolled back to the previous
    // build, started like any placed build, and done when it answers.
    {
        Handover h("b", "a");
        CHECK(h.step(nothing, t) == Act::Start);
        CHECK(h.step(nothing, t + kStartMs - 1) == Act::Wait && !h.failed());
        CHECK(h.step(nothing, t + kStartMs) == Act::Wait && h.failed());
        CHECK(h.step(nothing, t + kStartMs) == Act::RollBack);
        CHECK(h.rolledBack() && !h.failed() && h.placed() == "a");
        CHECK(h.step(nothing, t + kStartMs + 1) == Act::Start);
        CHECK(h.step(answering("a", false), t + kStartMs + 2) == Act::Done);
        CHECK(std::string(actName(Act::RollBack)) == "roll-back" && std::string(actName(Act::Fail)) == "fail");
    }
    // A start is timed from the moment nothing answers: a server that drains slowly does not use up the new build's time.
    {
        Handover h("b", "a");
        CHECK(h.step(nothing, t) == Act::Start);
        CHECK(h.step(answering("a", true), t + kStartMs - 1) == Act::Wait);
        CHECK(h.step(nothing, t + kStartMs) == Act::Start);
        CHECK(h.step(nothing, t + 2 * kStartMs - 1) == Act::Wait && !h.failed());
    }
    // Rolled back to a server from before takeover: it answers no `build`, and that is done.
    {
        Handover h("b", "old");
        CHECK(h.step(nothing, t) == Act::Start);
        CHECK(h.step(nothing, t + kStartMs) == Act::Wait);
        CHECK(h.step(nothing, t + kStartMs) == Act::RollBack && h.placed() == "old");
        Seen old;
        old.answers = true;
        CHECK(h.step(old, t + kStartMs + 1) == Act::Done);
    }
    // No previous build (a first install), the same build, or a previous that fails too: Fail, said once and again.
    for (const std::string& previous : {std::string(), std::string("b")}) {
        Handover h("b", previous);
        CHECK(h.step(nothing, t) == Act::Start);
        CHECK(h.step(nothing, t + kStartMs) == Act::Wait);
        CHECK(h.step(nothing, t + kStartMs) == Act::Fail);
        CHECK(h.step(nothing, t + kStartMs + 1) == Act::Fail);
    }
    {
        Handover h("b", "a");
        CHECK(h.step(nothing, t) == Act::Start);
        CHECK(h.step(nothing, t + kStartMs) == Act::Wait);
        CHECK(h.step(nothing, t + kStartMs) == Act::RollBack);
        CHECK(h.step(nothing, t + kStartMs) == Act::Start);
        CHECK(h.step(nothing, t + 2 * kStartMs) == Act::Wait);
        CHECK(h.step(nothing, t + 2 * kStartMs) == Act::Fail);
    }
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
    handover();
    builds();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("takeover_test: ok\n");
    return 0;
}
