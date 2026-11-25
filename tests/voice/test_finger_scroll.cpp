// Unit tests of nexus::FingerScroll (src/finger_scroll.h): the phone's list under the finger. Pure: the times are
// numbers the test counts; no clock, no OS.

#include "finger_scroll.h"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

using nexus::FingerPhase;
using nexus::FingerScroll;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

// A list 1000 px longer than its 500 px view.
FingerScroll list() {
    FingerScroll s;
    s.setRange(1000, 500);
    return s;
}

void aPressThatStaysIsATap() {
    auto s = list();
    s.down(300, 0, 0);
    s.move(305, 0.05);
    CHECK(s.phase() == FingerPhase::Track);
    CHECK(!s.dragging());
    CHECK(s.up(0.1, 0.1));
    CHECK(s.phase() == FingerPhase::Rest);
    CHECK(s.offset() == 0);
}

void theContentFollowsTheFinger() {
    auto s = list();
    s.down(400, 0, 0);
    s.move(390, 0.01); // past the slop: a drag, from here
    CHECK(s.dragging());
    CHECK(s.offset() == 0);
    s.move(290, 0.5);
    CHECK(s.offset() == 100);
    s.move(340, 1.0);
    CHECK(s.offset() == 50);
    // Held still, then lifted: no fling, and no tap.
    CHECK(!s.up(2.0, 2.0));
    CHECK(s.phase() == FingerPhase::Rest);
    CHECK(s.offset() == 50);
}

void aFlickFlingsOnAndSlows() {
    auto s = list();
    s.down(400, 0, 0);
    s.move(390, 0.00);
    s.move(340, 0.05);
    s.move(290, 0.10); // 1000 px/s up the screen: the content scrolls down the list
    CHECK(!s.up(0.10, 10));
    CHECK(s.phase() == FingerPhase::Fling);
    const double from = s.offset();
    s.frame(10.1);
    const double a = s.offset();
    s.frame(10.2);
    const double b = s.offset();
    CHECK(a > from && b > a);
    CHECK(b - a < a - from); // slowing
    s.frame(30);
    CHECK(s.phase() == FingerPhase::Rest);
    // UIScrollView's normal deceleration carries 1000 px/s some 500 px on.
    CHECK(std::fabs(s.offset() - (from + 1000 / (-1000 * std::log(nexus::kFlingDecayPerMs)))) < 30);
    CHECK(s.offset() <= s.max());
}

void aFlingStopsAtTheEndAndNeverComesBackInside() {
    auto s = list();
    s.down(400, 0, 0);
    s.move(390, 0.00);
    s.move(190, 0.05);
    s.move(-10, 0.10); // 4000 px/s: past the end
    s.up(0.10, 0);
    bool reached = false;
    for (double t = 0; t < 5; t += 1.0 / 120) {
        s.frame(t);
        if (s.offset() >= s.max()) reached = true;
        if (reached) CHECK(s.offset() >= s.max()); // once at the end, it only bounces past it and back
        CHECK(s.offset() < s.max() + 500);
    }
    CHECK(reached);
    CHECK(s.phase() == FingerPhase::Rest);
    CHECK(s.offset() == s.max());
}

void pastTheTopIsRubberBandedAndSpringsBack() {
    auto s = list();
    s.down(100, 0, 0);
    s.move(110, 0.01);
    s.move(410, 0.5); // 300 px down from the top
    CHECK(s.offset() < 0);
    CHECK(s.offset() > -300); // banded: less than the finger went
    s.up(2.0, 2.0);
    CHECK(s.phase() == FingerPhase::Spring);
    for (double t = 2.0; t < 4; t += 1.0 / 120) {
        s.frame(t);
        CHECK(s.offset() <= 0); // never back inside past the top
    }
    CHECK(s.phase() == FingerPhase::Rest);
    CHECK(s.offset() == 0);
}

void aPressCatchesAMovingListAndIsNoTap() {
    auto s = list();
    s.down(400, 0, 0);
    s.move(390, 0.00);
    s.move(290, 0.10);
    s.up(0.10, 0);
    s.frame(0.1);
    const double where = s.offset();
    s.down(300, 0.2, 0.1);
    CHECK(s.phase() == FingerPhase::Track);
    CHECK(s.offset() == where);
    CHECK(!s.up(0.25, 0.15));
    CHECK(s.phase() == FingerPhase::Rest);
    s.frame(1);
    CHECK(s.offset() == where);
}

void aListThatFitsDoesNotScroll() {
    FingerScroll s;
    s.setRange(-200, 500); // shorter than its view
    CHECK(s.max() == 0);
    s.down(300, 0, 0);
    s.move(310, 0.01);
    s.move(200, 0.5);
    CHECK(s.offset() > 0); // rubber-banded only
    s.up(2, 2);
    s.frame(4);
    CHECK(s.offset() == 0);
}

void aShorterRangeAtRestPullsIn() {
    auto s = list();
    s.down(400, 0, 0);
    s.move(390, 0.0);
    s.move(-210, 1.0);
    s.up(3, 3);
    CHECK(s.offset() == 600);
    s.setRange(100, 500);
    CHECK(s.offset() == 100);
}

void names() {
    CHECK(std::string_view(nexus::fingerPhaseName(FingerPhase::Spring)) == "spring");
}

} // namespace

int main() {
    aPressThatStaysIsATap();
    theContentFollowsTheFinger();
    aFlickFlingsOnAndSlows();
    aFlingStopsAtTheEndAndNeverComesBackInside();
    pastTheTopIsRubberBandedAndSpringsBack();
    aPressCatchesAMovingListAndIsNoTap();
    aListThatFitsDoesNotScroll();
    aShorterRangeAtRestPullsIn();
    names();
    if (failures) {
        std::fprintf(stderr, "%d finger-scroll check(s) failed\n", failures);
        return 1;
    }
    std::printf("finger_scroll: all checks passed\n");
    return 0;
}
