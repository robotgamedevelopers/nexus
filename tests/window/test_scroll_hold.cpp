// Unit tests of the sprint board's held Up and Down (src/scroll_hold.h): a press moves the page on the first frame
// and every frame after at a steady speed, key repeats change nothing, a release or the window losing the keyboard
// stops it, both keys at once stand still and a reversal follows the key left down. A reversed check shows that a
// scroll driven by key repeats, as before (src/scroll_glide.h), stands still between the press's glide and the first
// repeat. Time is an argument: no clock, no OS, no window.

#include <cmath>
#include <cstdio>
#include <vector>

#include "scroll_glide.h"
#include "scroll_hold.h"

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

bool near(double a, double b, double within = 1e-9) {
    return std::fabs(a - b) <= within;
}

constexpr double kFrame = 1.0 / 120;
constexpr double kStart = 1000;     // the frame before the press, s

// The page's moves at each of `n` frames after `start`.
std::vector<double> steps(Hold* h, double start, int n) {
    std::vector<double> moves;
    for (int i = 1; i <= n; ++i)
        moves.push_back(holdStep(h, start + i * kFrame, kHoldSpeed));
    return moves;
}

void aPressMovesOnTheFirstFrameAndEveryFrameAfter() {
    Hold h = holdPress(Hold{}, HoldKey::Down, kStart);
    CHECK(holdDirection(h) == 1);
    std::vector<double> moves = steps(&h, kStart, 60);
    // The first frame moves, by about half the speed's frame (the ease's start).
    CHECK(moves[0] > 0);
    CHECK(near(moves[0], holdDistance(kFrame, kHoldSpeed)));
    CHECK(moves[0] >= kHoldEaseStart * kHoldSpeed * kFrame);
    // The ease never slows, and once past it every frame moves the same.
    for (std::size_t i = 1; i < moves.size(); ++i)
        CHECK(moves[i] >= moves[i - 1] - 1e-9);
    const std::size_t eased = std::size_t(std::ceil(kHoldEaseSeconds / kFrame)) + 1;
    for (std::size_t i = eased; i < moves.size(); ++i)
        CHECK(near(moves[i], kHoldSpeed * kFrame, 1e-6));
    // Half a second in, the page has moved the speed's distance less what the ease held back.
    double total = 0;
    for (double m : moves)
        total += m;
    CHECK(near(total, kHoldSpeed * (0.5 - (1 - kHoldEaseStart) * kHoldEaseSeconds / 2), 1e-6));

    // Up moves the other way, as far.
    Hold up = holdPress(Hold{}, HoldKey::Up, kStart);
    CHECK(holdDirection(up) == -1);
    std::vector<double> back = steps(&up, kStart, 60);
    for (std::size_t i = 0; i < back.size(); ++i)
        CHECK(near(back[i], -moves[i]));
}

void repeatsChangeNothing() {
    Hold plain = holdPress(Hold{}, HoldKey::Down, kStart);
    Hold repeated = plain;
    double a = 0, b = 0;
    for (int i = 1; i <= 120; ++i) {
        const double now = kStart + i * kFrame;
        // Repeats of either key, at any rate, between frames.
        if (i % 4 == 0) {
            Hold before = repeated;
            repeated = holdRepeat(repeated, HoldKey::Down);
            CHECK(repeated.up == before.up && repeated.down == before.down && repeated.since == before.since &&
                repeated.stepped == before.stepped);
        }
        if (i % 7 == 0)
            repeated = holdRepeat(repeated, HoldKey::Up);
        a += holdStep(&plain, now, kHoldSpeed);
        b += holdStep(&repeated, now, kHoldSpeed);
        CHECK(a == b);
    }
    // A repeat with no key held starts nothing.
    Hold idle = holdRepeat(Hold{}, HoldKey::Down);
    CHECK(holdDirection(idle) == 0);
    CHECK(holdStep(&idle, kStart + 1, kHoldSpeed) == 0);
}

void aReleaseStopsIt() {
    Hold h = holdPress(Hold{}, HoldKey::Down, kStart);
    steps(&h, kStart, 10);
    const double at = kStart + 10 * kFrame;
    h = holdRelease(h, HoldKey::Down, at);
    CHECK(holdDirection(h) == 0);
    for (double m : steps(&h, at, 30))
        CHECK(m == 0);
    // A tap: down for 80 ms scrolls a short way, then nothing.
    Hold tap = holdPress(Hold{}, HoldKey::Down, kStart);
    double moved = 0;
    for (double m : steps(&tap, kStart, 10))
        moved += m;
    tap = holdRelease(tap, HoldKey::Down, kStart + 10 * kFrame);
    for (double m : steps(&tap, kStart + 10 * kFrame, 30))
        moved += m;
    CHECK(near(moved, holdDistance(10 * kFrame, kHoldSpeed)));
    CHECK(moved > 0 && moved < 100);
}

void blurStopsIt() {
    Hold h = holdPress(holdPress(Hold{}, HoldKey::Down, kStart), HoldKey::Up, kStart);
    h = holdRelease(h, HoldKey::Up, kStart);
    steps(&h, kStart, 10);
    h = holdBlur(h);
    CHECK(!h.up && !h.down && holdDirection(h) == 0);
    for (double m : steps(&h, kStart + 10 * kFrame, 30))
        CHECK(m == 0);
    // The key's KeyUp after the blur (Prax sends it) leaves it stopped.
    h = holdRelease(h, HoldKey::Down, kStart + 40 * kFrame);
    CHECK(holdDirection(h) == 0);
}

void bothKeysStandStillAndAReversalFollowsTheKeyLeft() {
    Hold h = holdPress(Hold{}, HoldKey::Down, kStart);
    steps(&h, kStart, 10);
    double now = kStart + 10 * kFrame;
    h = holdPress(h, HoldKey::Up, now);
    CHECK(holdDirection(h) == 0);
    for (double m : steps(&h, now, 10))
        CHECK(m == 0);
    now += 10 * kFrame;
    // Down let go: Up drives, from the start of its ease.
    h = holdRelease(h, HoldKey::Down, now);
    CHECK(holdDirection(h) == -1);
    std::vector<double> up = steps(&h, now, 10);
    CHECK(near(up[0], -holdDistance(kFrame, kHoldSpeed)));
    for (double m : up)
        CHECK(m < 0);
    now += 10 * kFrame;
    // Down again: still, then Up let go: Down drives.
    h = holdPress(h, HoldKey::Down, now);
    CHECK(holdDirection(h) == 0);
    h = holdRelease(h, HoldKey::Up, now);
    CHECK(holdDirection(h) == 1);
    CHECK(steps(&h, now, 1)[0] > 0);
}

void aSkippedSpanOwesNothing() {
    Hold h = holdPress(Hold{}, HoldKey::Down, kStart);
    steps(&h, kStart, 10);
    h = holdSkip(h, kStart + 30 * kFrame);
    const double m = holdStep(&h, kStart + 31 * kFrame, kHoldSpeed);
    CHECK(near(m, kHoldSpeed * kFrame, 1e-6));
}

// Reversed: the scroll as it was, driven by key repeats (a press glides a line, each repeat a line more), stands still
// while the key is held, from the end of the press's glide to the first repeat. The system's delay before the first
// repeat is 225 ms at macOS's shortest setting and longer by default (unmeasured here); repeats every 30 ms. The hold
// moves at every frame of the same span.
void aRepeatDrivenScrollWaitsForTheFirstRepeat() {
    constexpr double kDelay = 0.225;
    constexpr double kRepeat = 0.03;
    constexpr double kMax = 1e6;
    Glide g = glideTo(Glide{}, kStart, 0, 40, kMax, kGlideKeySeconds, GlideKind::Press);
    double target = 40;
    double nextRepeat = kStart + kDelay;
    double shown = 0;
    int still = 0;
    Hold h = holdPress(Hold{}, HoldKey::Down, kStart);
    int holdStill = 0;
    for (int i = 1; i <= int(0.5 / kFrame); ++i) {
        const double now = kStart + i * kFrame;
        while (nextRepeat <= now) {
            target += 40;
            g = glideTo(g, now - kFrame, shown, target, kMax, kGlideKeySeconds, GlideKind::Continue);
            nextRepeat += kRepeat;
        }
        g = glideFrame(g, now);
        const double next = glidePosition(g, now, kMax);
        if (next == shown)
            ++still;
        shown = next;
        if (holdStep(&h, now, kHoldSpeed) == 0)
            ++holdStill;
    }
    CHECK(still >= 3);          // 180 ms to 225 ms: five frames at 120 a second
    CHECK(holdStill == 0);
    std::printf("repeat-driven: %d of %d frames still in the first half second; held: %d\n", still, int(0.5 / kFrame),
        holdStill);
}

} // namespace

int main() {
    aPressMovesOnTheFirstFrameAndEveryFrameAfter();
    repeatsChangeNothing();
    aReleaseStopsIt();
    blurStopsIt();
    bothKeysStandStillAndAReversalFollowsTheKeyLeft();
    aSkippedSpanOwesNothing();
    aRepeatDrivenScrollWaitsForTheFirstRepeat();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("scroll hold: all checks passed\n");
    return 0;
}
