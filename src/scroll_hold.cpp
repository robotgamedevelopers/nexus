#include "scroll_hold.h"

#include <algorithm>

namespace nexus {

int holdDirection(const Hold& h) {
    if (h.up == h.down)
        return 0;
    return h.down ? 1 : -1;
}

namespace {

Hold withKey(const Hold& h, HoldKey key, bool isDown, double now) {
    Hold next = h;
    (key == HoldKey::Up ? next.up : next.down) = isDown;
    if (holdDirection(next) != holdDirection(h)) {
        next.since = now;
        next.stepped = now;
    }
    return next;
}

} // namespace

Hold holdPress(const Hold& h, HoldKey key, double now) {
    return withKey(h, key, true, now);
}

Hold holdRelease(const Hold& h, HoldKey key, double now) {
    return withKey(h, key, false, now);
}

Hold holdRepeat(const Hold& h, HoldKey) {
    return h;
}

Hold holdBlur(const Hold&) {
    return Hold{};
}

double holdDistance(double elapsed, double speed) {
    if (elapsed <= 0)
        return 0;
    const double e = kHoldEaseSeconds;
    const double f = kHoldEaseStart;
    if (elapsed < e)
        return speed * (f * elapsed + (1 - f) * elapsed * elapsed / (2 * e));
    return speed * (elapsed - (1 - f) * e / 2);
}

double holdStep(Hold* h, double now, double speed) {
    const int direction = holdDirection(*h);
    const double from = std::max(h->stepped, h->since);
    double distance = 0;
    if (direction != 0 && now > from)
        distance = direction * (holdDistance(now - h->since, speed) - holdDistance(from - h->since, speed));
    h->stepped = std::max(h->stepped, now);
    return distance;
}

Hold holdSkip(const Hold& h, double now) {
    Hold next = h;
    next.stepped = std::max(next.stepped, now);
    return next;
}

} // namespace nexus
