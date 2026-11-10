#include "scroll_glide.h"

#include <algorithm>

namespace nexus {

double glideCurve(double s, double a) {
    s = std::clamp(s, 0.0, 1.0);
    return s * (a + s * ((3 - 2 * a) + s * (a - 2)));
}

double glideCurveSlope(double s, double a) {
    if (s < 0 || s >= 1)
        return 0;
    return a + s * (2 * (3 - 2 * a) + s * 3 * (a - 2));
}

namespace {

double phase(const Glide& g, double now) {
    if (g.pending)
        return 0;
    if (g.duration <= 0)
        return 1;
    return (now - g.start) / g.duration;
}

} // namespace

double glidePosition(const Glide& g, double now, double max) {
    double y = g.from + (g.to - g.from) * glideCurve(phase(g, now), g.slope);
    return std::clamp(y, 0.0, std::max(max, 0.0));
}

double glideSpeed(const Glide& g, double now) {
    if (!g.active || g.duration <= 0)
        return 0;
    return (g.to - g.from) / g.duration * glideCurveSlope(phase(g, now), g.slope);
}

bool glideDone(const Glide& g, double now) {
    return !g.active || (!g.pending && phase(g, now) >= 1);
}

Glide glideTo(const Glide& g, double now, double at, double target, double max, double duration, GlideKind kind) {
    target = std::clamp(target, 0.0, std::max(max, 0.0));
    Glide next;
    next.to = target;
    next.duration = duration;
    next.active = true;
    if (g.active && g.pending) {
        // Not shown yet: the same start and ease, further on.
        next.from = g.from;
        next.slope = g.slope;
        next.pending = true;
    } else if (g.active && !glideDone(g, now)) {
        next.from = glidePosition(g, now, max);
        next.start = now;
        next.slope = 3;
        double distance = target - next.from;
        if (kind == GlideKind::Continue && distance != 0)
            next.slope = std::clamp(glideSpeed(g, now) * duration / distance, 0.0, 3.0);
    } else {
        next.from = std::clamp(at, 0.0, std::max(max, 0.0));
        next.pending = true;
    }
    if (next.from == next.to)
        next.active = false;
    return next;
}

Glide glideFrame(const Glide& g, double now) {
    Glide next = g;
    if (next.active && next.pending) {
        next.pending = false;
        next.start = now;
    }
    return next;
}

} // namespace nexus
