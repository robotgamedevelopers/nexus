#include "server/takeover.h"

namespace nexus::server {

std::string buildOf(std::string_view bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[h & 0xf];
        h >>= 4;
    }
    return out;
}

void Drain::request(Millis now) {
    if (draining_) {
        return;
    }
    draining_ = true;
    deadline_ = now + kDrainMs;
}

std::optional<Millis> Drain::untilDeadline(Millis now) const {
    if (!draining_) {
        return std::nullopt;
    }
    return now >= deadline_ ? 0 : deadline_ - now;
}

Step nextStep(const Seen& seen) {
    if (!seen.answers) {
        return Step::Start;
    }
    if (!seen.knowsBuild) {
        return Step::AskStop;
    }
    if (seen.build == seen.placed) {
        return Step::Done;
    }
    return seen.draining ? Step::Wait : Step::AskDrain;
}

const char* stepName(Step step) {
    switch (step) {
        case Step::Done: return "done";
        case Step::Start: return "start";
        case Step::AskDrain: return "ask-drain";
        case Step::AskStop: return "ask-stop";
        case Step::Wait: return "wait";
    }
    return "?";
}

const char* actName(Act act) {
    switch (act) {
        case Act::Done: return "done";
        case Act::Wait: return "wait";
        case Act::AskStop: return "ask-stop";
        case Act::AskDrain: return "ask-drain";
        case Act::Start: return "start";
        case Act::RollBack: return "roll-back";
        case Act::Fail: return "fail";
    }
    return "?";
}

Act Handover::step(const Seen& seen, Millis now) {
    // A start that did not answer in time (noticed at the step before): roll back once, to a build that ran.
    if (failed_) {
        if (rolledBack_ || previous_.empty() || previous_ == placed_) {
            return Act::Fail;
        }
        placed_ = previous_;
        rolledBack_ = true;
        failed_ = false;
        startedAt_.reset();
        return Act::RollBack;
    }
    // Rolled back, whatever answers is the previous build started again (one from before takeover answers no `build`).
    if (rolledBack_ && seen.answers && (!seen.knowsBuild || seen.build == placed_)) {
        return Act::Done;
    }
    Seen s = seen;
    s.placed = placed_;
    if (seen.answers) {
        startedAt_.reset();     // a start is timed from the moment nothing answers, each time
    }
    switch (nextStep(s)) {
        case Step::Done: return Act::Done;
        case Step::AskStop: return Act::AskStop;
        case Step::AskDrain: return Act::AskDrain;
        case Step::Wait: return Act::Wait;
        case Step::Start: break;
    }
    if (!startedAt_) {
        startedAt_ = now;
        return Act::Start;
    }
    if (now - *startedAt_ >= startMs_) {
        failed_ = true;
    }
    return Act::Wait;
}

std::string buildReply(const std::string& build, bool draining) {
    return "BUILD " + build + (draining ? " draining" : "");
}

bool parseBuildReply(const std::string& line, Seen* seen) {
    const std::string kHead = "BUILD ";
    if (line.compare(0, kHead.size(), kHead) != 0) {
        return false;
    }
    std::string rest = line.substr(kHead.size());
    bool draining = false;
    const std::string kDraining = " draining";
    if (rest.size() > kDraining.size() && rest.compare(rest.size() - kDraining.size(), kDraining.size(), kDraining) == 0) {
        draining = true;
        rest.resize(rest.size() - kDraining.size());
    }
    if (rest.empty() || rest.find(' ') != std::string::npos) {
        return false;
    }
    seen->knowsBuild = true;
    seen->build = rest;
    seen->draining = draining;
    return true;
}

} // namespace nexus::server
