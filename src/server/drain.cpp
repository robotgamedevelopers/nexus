// Drain (server/drain.h): open-turn drain decisions and `status` second-line formatting/parsing.

#include "server/drain.h"

#include "server/build_manifest.h"

namespace nexus::server {

void Drain::request(std::uint64_t now_ms) {
    if (!draining_) {
        draining_ = true;
        deadline_ms_ = now_ms + limit_ms_;
    }
}

std::optional<std::uint64_t> Drain::untilDeadline(std::uint64_t now_ms) const {
    if (!draining_) {
        return std::nullopt;
    }
    return now_ms >= deadline_ms_ ? 0 : deadline_ms_ - now_ms;
}

std::string statusBuildLine(const std::string& build, bool draining) {
    return "build " + (build.empty() ? std::string("none") : build) + (draining ? " draining" : "");
}

bool parseStatusLines(const std::string& first, const std::string& second, bool hasSecond, ServerSeen* out) {
    *out = ServerSeen{};
    if (first != "SERVING") {
        return false;
    }
    if (!hasSecond) {
        out->serving = true;
        out->legacy = true;
        return true;
    }
    if (second.rfind("build ", 0) != 0) {
        return false;
    }
    const std::string rest = second.substr(6);
    const std::size_t space = rest.find(' ');
    const std::string token = space == std::string::npos ? rest : rest.substr(0, space);
    const std::string tail = space == std::string::npos ? std::string() : rest.substr(space + 1);
    if (!tail.empty() && tail != "draining") {
        return false;
    }
    if (token != "none" && !builds::isHash(token)) {
        return false;
    }
    out->serving = true;
    out->legacy = false;
    out->build = token == "none" ? std::string() : token;
    out->draining = (tail == "draining");
    return true;
}

} // namespace nexus::server
