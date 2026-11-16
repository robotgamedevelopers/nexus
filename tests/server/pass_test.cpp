// One pass of the server's loop (src/server/pass.h): PassOrder and take_pass_step across normal passes, stopped and
// failed steps, every reversed-witness fault, and endPass. Logic only: no processes, files, sockets or clock.

#include "server/pass.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using nexus::server::PassEnd;
using nexus::server::PassOrder;
using nexus::server::PassSteps;
using nexus::server::endPass;
using nexus::server::take_pass_step;

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

struct FakePass : PassSteps {
    PassOrder::Outcome waitOut = PassOrder::Outcome::Ok;
    PassOrder::Outcome holdOut = PassOrder::Outcome::Ok;
    PassOrder::Outcome applyOut = PassOrder::Outcome::Changed;
    PassOrder::Outcome flushOut = PassOrder::Outcome::Ok;
    std::vector<std::string> log;

    void tick() override { log.push_back("tick"); }
    PassOrder::Outcome wait(std::string* why) override {
        log.push_back("wait");
        if (waitOut == PassOrder::Outcome::Failed) {
            *why = "poll failed";
        }
        return waitOut;
    }
    PassOrder::Outcome hold(bool holdDrivers, std::string* why) override {
        log.push_back(holdDrivers ? "hold" : "hold-unheld");
        if (holdOut != PassOrder::Outcome::Ok) {
            *why = "hold failed";
        }
        return holdOut;
    }
    PassOrder::Outcome apply(std::string* why) override {
        log.push_back("apply");
        if (applyOut == PassOrder::Outcome::Failed) {
            *why = "store failed in apply";
        }
        return applyOut;
    }
    PassOrder::Outcome flush(std::string* why) override {
        log.push_back("flush");
        if (flushOut != PassOrder::Outcome::Ok) {
            *why = "disk full";
        }
        return flushOut;
    }
    void release() override { log.push_back("release"); }
    void state() override { log.push_back("state"); }
};

PassOrder::Step runPass(PassOrder::Fault fault, FakePass& steps, std::string* why) {
    PassOrder order(fault);
    for (int guard = 0; guard < 16; ++guard) {
        take_pass_step(order, steps, why);
        const PassOrder::Step next = order.next();
        if (next == PassOrder::Step::Done || next == PassOrder::Step::Stopped || next == PassOrder::Step::Failed) {
            return next;
        }
    }
    return PassOrder::Step::Failed;
}

std::vector<std::string> runEnd(bool flushes, bool changed, bool* ended) {
    std::vector<std::string> done;
    const PassEnd end{
        [&](std::string* why) {
            done.push_back("flush");
            if (!flushes) {
                *why = "the disk is gone";
            }
            return flushes;
        },
        [&] { done.push_back("release"); },
        [&] { done.push_back("publish"); }};
    std::string why;
    *ended = endPass(end, changed, &why);
    if (!flushes) {
        CHECK(why == "the disk is gone");
    }
    return done;
}

} // namespace

int main() {
    // Normal pass with changes: tick, wait, hold, apply, flush, release, state.
    {
        FakePass s;
        std::string why;
        CHECK(runPass(PassOrder::Fault::None, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "flush", "release", "state"}));
    }
    // Normal pass with nothing changed: release runs, state is skipped.
    {
        FakePass s;
        s.applyOut = PassOrder::Outcome::Unchanged;
        std::string why;
        CHECK(runPass(PassOrder::Fault::None, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "flush", "release"}));
    }
    // Wait stops the loop cleanly before any batch is begun.
    {
        FakePass s;
        s.waitOut = PassOrder::Outcome::Stopped;
        std::string why;
        CHECK(runPass(PassOrder::Fault::None, s, &why) == PassOrder::Step::Stopped);
        CHECK((s.log == std::vector<std::string>{"tick", "wait"}));
    }
    // Failed apply stops before flush, release or state.
    {
        FakePass s;
        s.applyOut = PassOrder::Outcome::Failed;
        std::string why;
        CHECK(runPass(PassOrder::Fault::None, s, &why) == PassOrder::Step::Failed);
        CHECK(why == "store failed in apply");
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply"}));
    }
    // Failed flush stops before release or state.
    {
        FakePass s;
        s.flushOut = PassOrder::Outcome::Failed;
        std::string why;
        CHECK(runPass(PassOrder::Fault::None, s, &why) == PassOrder::Step::Failed);
        CHECK(why == "disk full");
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "flush"}));
    }
    // Fault modes for negative trace checks.
    {
        FakePass s;
        std::string why;
        CHECK(runPass(PassOrder::Fault::ReleaseBeforeFlush, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "release", "flush", "state"}));
    }
    {
        FakePass s;
        std::string why;
        CHECK(runPass(PassOrder::Fault::StateBeforeFlush, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "state", "flush", "release"}));
    }
    {
        FakePass s;
        std::string why;
        CHECK(runPass(PassOrder::Fault::TickBeforeFlush, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "tick", "flush", "release", "state"}));
    }
    {
        FakePass s;
        std::string why;
        CHECK(runPass(PassOrder::Fault::NoHold, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold-unheld", "apply", "flush", "release", "state"}));
    }
    {
        FakePass s;
        s.flushOut = PassOrder::Outcome::Failed;
        std::string why;
        CHECK(runPass(PassOrder::Fault::ReleaseOnFail, s, &why) == PassOrder::Step::Done);
        CHECK((s.log == std::vector<std::string>{"tick", "wait", "hold", "apply", "flush", "release", "state"}));
    }

    // endPass helper compatibility.
    bool ended = false;
    CHECK((runEnd(true, true, &ended) == std::vector<std::string>{"flush", "release", "publish"}));
    CHECK(ended);
    CHECK((runEnd(true, false, &ended) == std::vector<std::string>{"flush", "release"}));
    CHECK(ended);
    CHECK((runEnd(false, true, &ended) == std::vector<std::string>{"flush"}));
    CHECK(!ended);

    if (failures != 0) {
        std::fprintf(stderr, "pass_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pass_test: ok\n");
    return 0;
}
