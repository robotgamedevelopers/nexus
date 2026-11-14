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
