#include "finger_scroll.h"

#include <algorithm>
#include <cmath>

#include "scroll_glide.h"

namespace nexus {

const char* fingerPhaseName(FingerPhase phase) {
    switch (phase) {
        case FingerPhase::Rest: return "rest";
        case FingerPhase::Track: return "track";
        case FingerPhase::Drag: return "drag";
        case FingerPhase::Fling: return "fling";
        case FingerPhase::Spring: return "spring";
    }
    return "?";
}

namespace {

// ln of the speed kept each second.
double flingRate() { return 1000 * std::log(kFlingDecayPerMs); }

} // namespace

void FingerScroll::setRange(double max, double height) {
    max_ = std::max(0.0, max);
    height_ = std::max(1.0, height);
    if (phase_ == FingerPhase::Rest) offset_ = std::clamp(offset_, 0.0, max_);
}

double FingerScroll::raw(double y) const { return from_ - (y - y0_); }

double FingerScroll::banded(double r) const {
    if (r < 0) return -rubberBand(-r, height_);
    if (r > max_) return max_ + rubberBand(r - max_, height_);
    return r;
}

void FingerScroll::down(double y, double t, double now) {
    frame(now);
    // A finger on a moving list catches it where it shows. Past an end the drag goes on from there, rubber-banded.
    phase_ = FingerPhase::Track;
    y0_ = y;
    from_ = std::clamp(offset_, 0.0, max_);
    if (offset_ < 0 || offset_ > max_) {
        // The raw offset that draws where the list shows: the band's inverse, x = d c... solved by bisection on the
        // monotonic band, so the content does not jump under the finger.
        const bool top = offset_ < 0;
        const double past = top ? -offset_ : offset_ - max_;
        double lo = 0, hi = past * 64 + height_;
        for (int i = 0; i < 60; ++i) {
            const double mid = (lo + hi) / 2;
            (rubberBand(mid, height_) < past ? lo : hi) = mid;
        }
        from_ = top ? -lo : max_ + lo;
    }
    samples_ = 0;
    times_[0] = t;
    ys_[0] = y;
    samples_ = 1;
}

void FingerScroll::move(double y, double t) {
    if (phase_ != FingerPhase::Track && phase_ != FingerPhase::Drag) return;
    times_[samples_ % times_.size()] = t;
    ys_[samples_ % ys_.size()] = y;
    ++samples_;
    if (phase_ == FingerPhase::Track) {
        if (std::fabs(y - y0_) < kSlopPixels) return;
        phase_ = FingerPhase::Drag;
        // The content starts moving from here, not with a jump of the slop.
        y0_ = y;
    }
    offset_ = banded(raw(y));
}

double FingerScroll::speedAt(double t) const {
    // The finger's speed (px/s, down the screen) over its moves within kVelocityWindow of the last.
    const std::size_t n = std::min(samples_, times_.size());
    if (n < 2) return 0;
    const std::size_t last = (samples_ - 1) % times_.size();
    std::size_t first = last;
    for (std::size_t k = 1; k < n; ++k) {
        const std::size_t i = (samples_ - 1 - k) % times_.size();
        if (times_[last] - times_[i] > kVelocityWindow) break;
        first = i;
    }
    const double dt = times_[last] - times_[first];
    // A finger held still before it lifts has no speed.
    if (dt <= 0 || t - times_[last] > kVelocityWindow) return 0;
    return (ys_[last] - ys_[first]) / dt;
}

bool FingerScroll::up(double t, double now) {
    if (phase_ == FingerPhase::Track) {
        phase_ = FingerPhase::Rest;
        offset_ = std::clamp(offset_, 0.0, max_);
        return true;
    }
    if (phase_ != FingerPhase::Drag) return false;
    release(-speedAt(t), now);
    return false;
}

void FingerScroll::cancel(double now) {
    if (phase_ == FingerPhase::Track) {
        phase_ = FingerPhase::Rest;
        offset_ = std::clamp(offset_, 0.0, max_);
    } else if (phase_ == FingerPhase::Drag) {
        release(0, now);
    }
}

void FingerScroll::release(double v, double t) {
    if (offset_ < 0) return springFrom(0, v, t);
    if (offset_ > max_) return springFrom(max_, v, t);
    if (std::fabs(v) < kFlingMinSpeed) {
        phase_ = FingerPhase::Rest;
        return;
    }
    phase_ = FingerPhase::Fling;
    start_ = t;
    p0_ = offset_;
    v0_ = v;
}

void FingerScroll::springFrom(double end, double v, double t) {
    phase_ = FingerPhase::Spring;
    start_ = t;
    end_ = end;
    p0_ = offset_ - end;
    // Moving away from the end, the spring takes the speed; toward it, it would carry the list inside: the spring
    // starts from rest there, so it never crosses back in.
    v0_ = (p0_ > 0) == (v > 0) ? v : 0;
}

void FingerScroll::frame(double now) {
    if (phase_ == FingerPhase::Fling) {
        const double s = std::max(0.0, now - start_);
        const double k = flingRate();
        const double v = v0_ * std::exp(k * s);
        const double p = p0_ + v0_ * (std::exp(k * s) - 1) / k;
        if (p < 0 || p > max_) {
            // Reached an end: the time it got there, and its speed then, start the spring.
            const double end = p < 0 ? 0 : max_;
            // Solve p0 + v0 (e^(k s) - 1) / k = end for s.
            const double arg = 1 + (end - p0_) * k / v0_;
            const double at = arg > 0 ? start_ + std::log(arg) / k : now;
            offset_ = end;
            springFrom(end, v0_ * std::exp(k * std::max(0.0, at - start_)), std::min(at, now));
            frame(now);
            return;
        }
        offset_ = p;
        if (std::fabs(v) < kFlingMinSpeed) phase_ = FingerPhase::Rest;
        return;
    }
    if (phase_ == FingerPhase::Spring) {
        const double s = std::max(0.0, now - start_);
        double e = springOffset(p0_, v0_, s);
        const double v = springSpeed(p0_, v0_, s);
        // Never back inside the range: a spring rests on its end.
        if ((p0_ >= 0 && e < 0) || (p0_ <= 0 && e > 0)) e = 0;
        const bool settled = s >= kSpringMaxSeconds || (std::fabs(e) < kSettlePixels && std::fabs(v) < kSettleSpeed);
        offset_ = settled ? end_ : end_ + e;
        if (settled) phase_ = FingerPhase::Rest;
    }
}

} // namespace nexus
