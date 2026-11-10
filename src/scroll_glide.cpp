#include "scroll_glide.h"

#include <algorithm>
#include <cmath>

namespace nexus {

double coastRate() {
    return -std::log(kCoastDecayPerMs) * 1000;
}

double coastPosition(double y0, double target, double t) {
    return target + (y0 - target) * std::exp(-coastRate() * std::max(t, 0.0));
}

double coastSpeed(double y0, double target, double t) {
    return coastRate() * (target - coastPosition(y0, target, t));
}

// The page follows a target that starts at `base` and runs at `speed`: y' = k (base + speed t - y).
double holdPosition(double y0, double base, double speed, double t) {
    const double k = coastRate();
    t = std::max(t, 0.0);
    return base + speed * t - speed / k + (y0 - base + speed / k) * std::exp(-k * t);
}

double holdSpeed(double y0, double base, double speed, double t) {
    return coastRate() * (base + speed * std::max(t, 0.0) - holdPosition(y0, base, speed, t));
}

double springOffset(double e0, double v0, double t) {
    t = std::max(t, 0.0);
    return (e0 + (v0 + kSpringOmega * e0) * t) * std::exp(-kSpringOmega * t);
}

double springSpeed(double e0, double v0, double t) {
    t = std::max(t, 0.0);
    const double w = kSpringOmega;
    return (v0 - w * (v0 + w * e0) * t) * std::exp(-w * t);
}

double rubberBand(double x, double d) {
    if (d <= 0 || x == 0)
        return 0;
    const double a = std::fabs(x);
    const double banded = (1 - 1 / (a * kRubberBandCoefficient / d + 1)) * d;
    return x < 0 ? -banded : banded;
}

namespace {

double elapsed(const Glide& g, double now) {
    return g.pending ? 0 : now - g.start;
}

int sign(double x) {
    return x > 0 ? 1 : x < 0 ? -1 : 0;
}

} // namespace

double glidePosition(const Glide& g, double now) {
    const double t = elapsed(g, now);
    switch (g.kind) {
        case GlideKind::Coast: return coastPosition(g.y0, g.target, t);
        case GlideKind::Hold: return holdPosition(g.y0, g.target, g.speed, t);
        case GlideKind::Spring: return g.target + springOffset(g.y0 - g.target, g.speed, t);
        case GlideKind::Idle: break;
    }
    return g.y0;
}

double glideSpeed(const Glide& g, double now) {
    const double t = elapsed(g, now);
    switch (g.kind) {
        case GlideKind::Coast: return coastSpeed(g.y0, g.target, t);
        case GlideKind::Hold: return holdSpeed(g.y0, g.target, g.speed, t);
        case GlideKind::Spring: return springSpeed(g.y0 - g.target, g.speed, t);
        case GlideKind::Idle: break;
    }
    return 0;
}

bool GlideScroller::stale(GlidePage& page) const {
    return busy() && std::fabs(page.top() - applied_) > 1;
}

void GlideScroller::drop() {
    glide_ = Glide{};
    held_ = false;
    over_ = 0;
}

void GlideScroller::show(GlidePage& page, double y) {
    const double end = std::clamp(y, 0.0, std::max(page.max(), 0.0));
    page.setTop(end);
    applied_ = page.top();
    over_ = rubberBand(y - end, page.height());
}

void GlideScroller::coastTo(double now, double y, double target, int dir) {
    const bool pending = glide_.kind == GlideKind::Idle || glide_.pending;
    glide_ = Glide{GlideKind::Coast, y, target, 0, pending ? 0 : now, pending, dir};
}

bool GlideScroller::through(GlidePage& page, double now, int dir, const std::function<void()>& apply,
        double* landed) {
    const bool moving = glide_.kind != GlideKind::Idle;
    const double max = std::max(page.max(), 0.0);
    const double y = moving ? glidePosition(glide_, now) : page.top();
    const double shown = std::clamp(y, 0.0, max);
    // Onward from where the motion goes if the event goes the same way, so that keys and notches add up; from where
    // the page shows if it turns back.
    double aim = shown;
    if (moving && dir != 0 && glide_.dir == dir) {
        double goal = glide_.target;
        if (glide_.kind == GlideKind::Hold)
            goal += glide_.speed * elapsed(glide_, now);
        if (glide_.kind != GlideKind::Spring)
            aim = std::clamp(goal, 0.0, max);
    }
    page.setTop(aim);
    const double before = page.top();
    apply();
    const double after = page.top();
    page.setTop(shown);
    applied_ = page.top();
    if (after != before) {
        coastTo(now, y, after, sign(after - y) != 0 ? sign(after - y) : dir);
        *landed = after;
        return true;
    }
    // At the end it goes toward, and not past it already: a bump past it, which springs back.
    if (dir != 0 && y == shown && std::fabs(before - (dir > 0 ? max : 0.0)) < 0.5)
        coastTo(now, y, (dir > 0 ? max : 0.0) + dir * kBumpPixels, dir);
    return false;
}

void GlideScroller::release(double now) {
    if (!held_)
        return;
    held_ = false;
    if (glide_.kind != GlideKind::Hold)
        return;
    const double t = elapsed(glide_, now);
    const double y = glidePosition(glide_, now);
    double goal = glide_.target + glide_.speed * t;
    // A tap: exactly the line, unless the page already shows past it.
    if (t < kHoldTapSeconds && glide_.dir * (glide_.target - y) >= 0)
        goal = glide_.target;
    glide_.kind = GlideKind::Coast;
    glide_.y0 = y;
    glide_.target = goal;
    glide_.speed = 0;
    if (!glide_.pending)
        glide_.start = now;
}

bool GlideScroller::keyDown(GlidePage& page, double now, int arrow, int dir, bool repeat,
        const std::function<void()>& apply) {
    if (arrow != 0 && repeat)
        return false;
    if (stale(page))
        drop();
    release(now);
    double landed = 0;
    if (through(page, now, dir, apply, &landed) && arrow != 0) {
        glide_.kind = GlideKind::Hold;
        glide_.target = landed;
        glide_.speed = arrow * kHoldPixelsPerSecond;
        glide_.dir = arrow;
        held_ = true;
        heldDir_ = arrow;
    }
    return true;
}

void GlideScroller::keyUp(GlidePage& page, double now, int arrow) {
    if (stale(page))
        drop();
    if (held_ && arrow != 0 && arrow == heldDir_)
        release(now);
}

void GlideScroller::wheel(GlidePage& page, double now, int dir, const std::function<void()>& apply) {
    if (stale(page))
        drop();
    release(now);
    double landed = 0;
    through(page, now, dir, apply, &landed);
}

void GlideScroller::interrupt(GlidePage& page, double now) {
    if (stale(page)) {
        drop();
        return;
    }
    if (glide_.kind != GlideKind::Idle)
        show(page, std::clamp(glidePosition(glide_, now), 0.0, std::max(page.max(), 0.0)));
    glide_ = Glide{};
    held_ = false;
    over_ = 0;
}

void GlideScroller::hide(GlidePage& page, double now) {
    if (stale(page)) {
        drop();
        return;
    }
    if (glide_.kind != GlideKind::Idle) {
        double goal = glide_.target;
        if (glide_.kind == GlideKind::Hold)
            goal += glide_.speed * elapsed(glide_, now);
        show(page, std::clamp(goal, 0.0, std::max(page.max(), 0.0)));
    }
    glide_ = Glide{};
    held_ = false;
    over_ = 0;
}

bool GlideScroller::frame(GlidePage& page, double now) {
    if (!busy())
        return false;
    if (stale(page)) {
        drop();
        return false;
    }
    if (glide_.kind == GlideKind::Idle)
        return false;           // a hold parked at an end
    if (glide_.pending) {
        glide_.pending = false;
        glide_.start = now;
    }
    const double max = std::max(page.max(), 0.0);
    const double t = now - glide_.start;
    double y = glidePosition(glide_, now);
    const double v = glideSpeed(glide_, now);
    if (glide_.kind == GlideKind::Spring) {
        if ((std::fabs(y - glide_.target) < kSettlePixels && std::fabs(v) < kSettleSpeed) || t >= kSpringMaxSeconds) {
            y = glide_.target;
            glide_ = Glide{};
        }
    } else if ((y > max && v > 0) || (y < 0 && v < 0)) {
        // Carried past an end: the spring takes it back.
        glide_ = Glide{GlideKind::Spring, y, y > max ? max : 0.0, v, now, false, 0};
    } else if (glide_.kind == GlideKind::Coast &&
            ((std::fabs(glide_.target - y) < kSettlePixels && std::fabs(v) < kSettleSpeed) || t >= kCoastMaxSeconds)) {
        y = std::clamp(glide_.target, 0.0, max);
        glide_ = Glide{};
    }
    show(page, y);
    return true;
}

} // namespace nexus
