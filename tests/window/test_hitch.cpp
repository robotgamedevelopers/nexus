// The hitch detector (src/hitch.h): the budget, phases, the ring and the log, on a fake clock. Pure logic: no real time,
// no OS, no sockets, nothing outside the test.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hitch.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

struct Fake {
    double ms = 1000;
    std::vector<std::string> lines;
};

double fakeNow(void* c) {
    return static_cast<Fake*>(c)->ms;
}

void fakeLog(void* c, const char* line) {
    static_cast<Fake*>(c)->lines.emplace_back(line);
}

struct Rig {
    Fake fake;
    HitchDetector d{HitchClock{fakeNow, &fake}, HitchLog{fakeLog, &fake}};

    // One frame of `work` ms, `gap` ms after the last one ended.
    void frame(double work, double gap = 0) {
        fake.ms += gap;
        d.begin();
        fake.ms += work;
        d.end();
    }
};

void budgetFromRefreshRate() {
    Rig r;
    CHECK(r.d.budget() > 8.33 && r.d.budget() < 8.34);       // 120 Hz when nothing is said
    r.d.setRefreshRate(60);
    CHECK(r.d.budget() > 16.66 && r.d.budget() < 16.67);
    r.d.setRefreshRate(0);
    CHECK(r.d.budget() > 8.33 && r.d.budget() < 8.34);
    r.d.setRefreshRate(-5);
    CHECK(r.d.budget() > 8.33 && r.d.budget() < 8.34);
}

void overBudgetIsAHitch() {
    Rig r;
    r.frame(8.0);
    r.frame(8.33);              // just under the budget
    CHECK(r.d.hitches() == 0);
    r.frame(8.4);
    CHECK(r.d.hitches() == 1);
    CHECK(r.d.frames() == 3);
    CHECK(r.d.held() == 1);
    CHECK(r.d.hitch(0).frame == 3);
    CHECK(r.d.hitch(0).work > 8.39 && r.d.hitch(0).work < 8.41);
    CHECK(r.d.worst() > 8.39);
}

void intervalIsFromTheFrameBefore() {
    Rig r;
    r.frame(1);
    r.frame(9, 7.333);          // began 8.333 ms after the first began
    CHECK(r.d.held() == 1);
    CHECK(r.d.hitch(0).interval > 8.33 && r.d.hitch(0).interval < 8.34);
    Rig first;
    first.frame(20);
    CHECK(first.d.hitch(0).interval == 0);
}

void phasesNamedAndSummed() {
    Rig r;
    r.d.begin();
    {
        HitchScope s(&r.d, Phase::SyncApply);
        r.fake.ms += 2;
    }
    {
        HitchScope s(&r.d, Phase::TerminalTab, 7);
        r.fake.ms += 3;
    }
    {
        HitchScope s(&r.d, Phase::TerminalTab, 7);
        r.fake.ms += 1;
    }
    {
        HitchScope s(&r.d, Phase::TerminalTab, 8);
        r.fake.ms += 4;
    }
    r.d.add(Phase::Draw, 0.5);
    r.d.end();
    CHECK(r.d.held() == 1);
    const Hitch& h = r.d.hitch(0);
    CHECK(h.phaseCount == 4);
    CHECK(h.phases[0].phase == Phase::SyncApply && h.phases[0].ms == 2);
    CHECK(h.phases[1].phase == Phase::TerminalTab && h.phases[1].tab == 7 && h.phases[1].ms == 4);
    CHECK(h.phases[2].phase == Phase::TerminalTab && h.phases[2].tab == 8 && h.phases[2].ms == 4);
    CHECK(h.phases[3].phase == Phase::Draw && h.phases[3].ms == 0.5f);
    // The next frame starts with no phases.
    r.frame(9);
    CHECK(r.d.hitch(1).phaseCount == 0);
}

void inputBetweenFramesCountsInTheNext() {
    Rig r;
    r.frame(1);
    {
        HitchScope s(&r.d, Phase::Input);
        r.fake.ms += 6;
    }
    r.frame(3);                 // 3 ms in the frame, 6 before it: 9 ms of work
    CHECK(r.d.hitches() == 1);
    CHECK(r.d.hitch(0).work > 8.99 && r.d.hitch(0).work < 9.01);
    CHECK(r.d.hitch(0).phaseCount == 1 && r.d.hitch(0).phases[0].phase == Phase::Input);
    r.frame(3);                 // the input counted once
    CHECK(r.d.hitches() == 1);
}

void tooManyPhasesGoToOther() {
    Rig r;
    r.d.begin();
    for (std::uint32_t t = 0; t < 40; ++t)
        r.d.add(Phase::TerminalTab, 1, t);
    r.fake.ms += 40;
    r.d.end();
    const Hitch& h = r.d.hitch(0);
    CHECK(h.phaseCount == kHitchPhases);
    CHECK(h.phases[kHitchPhases - 1].phase == Phase::Other);
    CHECK(h.phases[kHitchPhases - 1].ms == float(40 - (kHitchPhases - 1)));
}

void ringKeepsTheNewest() {
    Rig r;
    for (int i = 0; i < int(kHitchRing) + 10; ++i)
        r.frame(10 + i, 2000);
    CHECK(r.d.hitches() == kHitchRing + 10);
    CHECK(r.d.held() == kHitchRing);
    CHECK(r.d.hitch(0).frame == 11);                        // the oldest kept
    CHECK(r.d.hitch(kHitchRing - 1).frame == kHitchRing + 10);
}

void logIsRateLimited() {
    Rig r;
    r.frame(9);                 // logged
    r.frame(9, 100);            // within a second: counted, not logged
    r.frame(9, 100);
    CHECK(r.fake.lines.size() == 1);
    r.frame(9, 1000);           // a second on: logged, saying two were left out
    CHECK(r.fake.lines.size() == 2);
    CHECK(r.fake.lines[0].find("frame 1 took 9.00 ms") != std::string::npos);
    CHECK(r.fake.lines[1].find("and 2 more") != std::string::npos);
    r.frame(1, 5000);           // within budget: nothing
    CHECK(r.fake.lines.size() == 2);
}

void lineNamesThePhases() {
    Rig r;
    r.d.begin();
    r.d.add(Phase::Layout, 7.25);
    r.d.add(Phase::TerminalTab, 3, 4);
    r.fake.ms += 12;
    r.d.end();
    CHECK(r.fake.lines.size() == 1);
    const std::string& line = r.fake.lines[0];
    CHECK(line.find("layout 7.25 ms") != std::string::npos);
    CHECK(line.find("terminal tab 4 3.00 ms") != std::string::npos);
    CHECK(line.find("budget 8.33 ms") != std::string::npos);
    // A small buffer is cut, never overrun.
    char small[16];
    std::memset(small, 'x', sizeof small);
    HitchDetector::format(r.d.hitch(0), r.d.budget(), small, sizeof small);
    CHECK(std::strlen(small) == sizeof small - 1);
}

void beginWithoutEndEndsTheFrame() {
    Rig r;
    r.d.begin();
    r.fake.ms += 20;
    r.d.begin();                // the first frame ends here
    CHECK(r.d.frames() == 1);
    CHECK(r.d.hitches() == 1);
    r.d.end();
    r.d.end();                  // a second end does nothing
    CHECK(r.d.frames() == 2);
}

void nullScopeTimesNothing() {
    HitchScope s(nullptr, Phase::Input);
}

} // namespace

int main() {
    budgetFromRefreshRate();
    overBudgetIsAHitch();
    intervalIsFromTheFrameBefore();
    phasesNamedAndSummed();
    inputBetweenFramesCountsInTheNext();
    tooManyPhasesGoToOther();
    ringKeepsTheNewest();
    logIsRateLimited();
    lineNamesThePhases();
    beginWithoutEndEndsTheFrame();
    nullScopeTimesNothing();
    if (failures != 0) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::printf("hitch_unit: all passed\n");
    return 0;
}
