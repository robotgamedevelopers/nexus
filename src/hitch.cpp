#include "hitch.h"

#include <cstdio>

namespace nexus {

namespace {

// At most one log line a second.
constexpr double kLogIntervalMs = 1000;

} // namespace

const char* phaseName(Phase phase) {
    switch (phase) {
        case Phase::Input:
            return "input";
        case Phase::Terminals:
            return "terminals";
        case Phase::Keep:
            return "keep";
        case Phase::SyncApply:
            return "sync apply";
        case Phase::Board:
            return "board";
        case Phase::Layout:
            return "layout";
        case Phase::Draw:
            return "draw";
        case Phase::Present:
            return "present";
        case Phase::TerminalTab:
            return "terminal tab";
        case Phase::Other:
        case Phase::Count:
            break;
    }
    return "other";
}

HitchDetector::HitchDetector(HitchClock clock, HitchLog log) : clock_(clock), log_(log) {}

void HitchDetector::setRefreshRate(double hz) {
    budget_ = 1000.0 / (hz > 0 ? hz : kDefaultRefreshHz);
}

void HitchDetector::begin() {
    if (inFrame_) {
        end();
    }
    const double now = clock_.read();
    began_ = now;
    current_.frame = frames_ + 1;
    current_.interval = lastBegan_ >= 0 ? now - lastBegan_ : 0;
    lastBegan_ = now;
    inFrame_ = true;
}

void HitchDetector::add(Phase phase, double ms, std::uint32_t tab) {
    if (!inFrame_) {
        between_ += ms;
    }
    Hitch& h = current_;
    for (std::uint8_t i = 0; i < h.phaseCount; ++i) {
        PhaseTime& p = h.phases[i];
        if (p.phase == phase && p.tab == tab) {
            p.ms += float(ms);
            return;
        }
    }
    if (h.phaseCount < kHitchPhases - 1 || (h.phaseCount == kHitchPhases - 1 && phase == Phase::Other)) {
        h.phases[h.phaseCount++] = PhaseTime{phase, tab, float(ms)};
        return;
    }
    // Full: the last slot sums the rest.
    PhaseTime& last = h.phases[kHitchPhases - 1];
    if (h.phaseCount == kHitchPhases - 1) {
        last = PhaseTime{Phase::Other, 0, 0};
        h.phaseCount = kHitchPhases;
    }
    last.ms += float(ms);
}

void HitchDetector::end() {
    if (!inFrame_) {
        return;
    }
    inFrame_ = false;
    const double now = clock_.read();
    current_.work = now - began_ + between_;
    ++frames_;
    if (current_.work > worst_) {
        worst_ = current_.work;
    }
    if (current_.work > budget_) {
        ++hitches_;
        ring_[next_] = current_;
        next_ = (next_ + 1) % kHitchRing;
        if (now - loggedAt_ >= kLogIntervalMs) {
            loggedAt_ = now;
            log(current_);
            unlogged_ = 0;
        } else {
            ++unlogged_;
        }
    }
    between_ = 0;
    current_.phaseCount = 0;
}

std::size_t HitchDetector::held() const {
    return hitches_ < kHitchRing ? std::size_t(hitches_) : kHitchRing;
}

const Hitch& HitchDetector::hitch(std::size_t i) const {
    const std::size_t oldest = hitches_ < kHitchRing ? 0 : next_;
    return ring_[(oldest + i) % kHitchRing];
}

const char* HitchDetector::format(const Hitch& h, double budget, char* out, std::size_t size) {
    if (size == 0) {
        return out;
    }
    int n = std::snprintf(out, size, "nexus: hitch: frame %llu took %.2f ms of main-thread work (budget %.2f ms, %.2f ms "
        "since the frame before):", static_cast<unsigned long long>(h.frame), h.work, budget, h.interval);
    for (std::uint8_t i = 0; i < h.phaseCount && n >= 0 && std::size_t(n) < size; ++i) {
        const PhaseTime& p = h.phases[i];
        if (p.phase == Phase::TerminalTab) {
            n += std::snprintf(out + n, size - std::size_t(n), " %s %u %.2f ms;", phaseName(p.phase), p.tab,
                double(p.ms));
        } else {
            n += std::snprintf(out + n, size - std::size_t(n), " %s %.2f ms;", phaseName(p.phase), double(p.ms));
        }
    }
    return out;
}

void HitchDetector::log(const Hitch& h) {
    if (log_.write == nullptr) {
        return;
    }
    char line[1024];
    format(h, budget_, line, sizeof line);
    if (unlogged_ != 0) {
        char more[1100];
        std::snprintf(more, sizeof more, "%s (and %llu more since the last line)", line,
            static_cast<unsigned long long>(unlogged_));
        log_.write(log_.context, more);
        return;
    }
    log_.write(log_.context, line);
}

} // namespace nexus
