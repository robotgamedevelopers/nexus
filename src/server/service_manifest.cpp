#include "server/service_manifest.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include "accounts/registry.h"
#include "pr/json.h"

namespace nexus::server {
namespace {

constexpr std::size_t kMaxBytes = 1024 * 1024;
constexpr std::size_t kMaxServices = 256;
constexpr std::size_t kMaxWords = 256;
constexpr std::size_t kMaxWordBytes = 32768;

using Index = PrJsonValueIndex;

Index member(const PrJson* doc, Index object, std::string_view name) {
    return prJsonMember(doc, object, {name.data(), name.size()});
}

bool stringAt(const PrJson* doc, Index index, std::string& text) {
    if (prJsonKind(doc, index) != PrJsonKind_String) return false;
    const auto s = prJsonString(doc, index);
    if (s.size > kMaxWordBytes || (s.size && std::memchr(s.data, 0, s.size))) return false;
    text.assign(s.data ? s.data : "", s.size);
    return true;
}

bool fields(const PrJson* doc, Index object, std::initializer_list<std::string_view> allowed) {
    if (prJsonKind(doc, object) != PrJsonKind_Object) return false;
    std::set<std::string> keys;
    for (Index key = prJsonFirst(doc, object); key != kPrJsonMissing; key = prJsonAfter(doc, object, key)) {
        std::string name;
        if (!stringAt(doc, key, name) || !keys.insert(name).second ||
            std::find(allowed.begin(), allowed.end(), name) == allowed.end()) return false;
    }
    return true;
}

bool isAbsolute(std::string_view path) {
    return path.starts_with('/');
}

// Every PATH directory is explicit: an empty segment would implicitly run from cwd.
bool absolutePathList(std::string_view path) {
    for (;;) {
        const auto colon = path.find(':');
        if (!isAbsolute(path.substr(0, colon))) return false;
        if (colon == path.npos) return true;
        path.remove_prefix(colon + 1);
    }
}

bool envName(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::string readCommand(const PrJson* doc, Index row, ManagedService& service) {
    if (!stringAt(doc, member(doc, row, "cwd"), service.cwd) || !isAbsolute(service.cwd) ||
        !stringAt(doc, member(doc, row, "log"), service.log) || !isAbsolute(service.log))
        return "give cwd and log as absolute paths";
    const Index args = member(doc, row, "argv");
    if (prJsonKind(doc, args) != PrJsonKind_Array) return "give argv as an array of strings";
    for (auto arg = prJsonFirst(doc, args); arg != kPrJsonMissing; arg = prJsonAfter(doc, args, arg)) {
        if (service.argv.size() == kMaxWords || !stringAt(doc, arg, service.argv.emplace_back()))
            return "give at most 256 argv strings of at most 32768 bytes each, without NUL";
    }
    if (service.argv.empty() || service.argv.front().empty() ||
        (!isAbsolute(service.argv.front()) && service.argv.front().find('/') != std::string::npos))
        return "give argv[0] as an absolute path or a program name on PATH";
    const auto env = member(doc, row, "env");
    if (prJsonKind(doc, env) != PrJsonKind_Object) return "give env as an object with HOME and PATH";
    for (auto key = prJsonFirst(doc, env); key != kPrJsonMissing; key = prJsonAfter(doc, env, key)) {
        std::string name;
        std::string value;
        if (service.env.size() == kMaxWords || !stringAt(doc, key, name) || !envName(name) ||
            !stringAt(doc, key + 1, value) || !service.env.emplace(name, value).second)
            return "give at most 256 distinct environment variable names with string values without NUL";
    }
    const auto home = service.env.find("HOME");
    const auto path = service.env.find("PATH");
    if (home == service.env.end() || !isAbsolute(home->second) || path == service.env.end() ||
        !absolutePathList(path->second)) return "give an absolute env.HOME and absolute directories in env.PATH";
    const auto every = member(doc, row, "every");
    if (every != kPrJsonMissing) {
        const double seconds = prJsonNumber(doc, every, -1);
        if (prJsonKind(doc, every) != PrJsonKind_Number || !std::isfinite(seconds) || seconds < 0 ||
            seconds > UINT32_MAX || seconds != std::floor(seconds)) return "give every as whole seconds from 0 to 4294967295";
        service.everySeconds = static_cast<std::uint32_t>(seconds);
    }
    return {};
}

} // namespace

bool parseServiceManifest(std::string_view text, std::vector<ManagedService>& out, std::string& why) {
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> json(prJsonCreate(), prJsonDestroy);
    auto* doc = json.get();
    if (text.size() > kMaxBytes) { why = "services.json exceeds 1 MiB"; return false; }
    if (!doc || !prJsonParse(doc, {text.data(), text.size()}, nullptr)) { why = "services.json must be valid JSON"; return false; }
    const auto rows = member(doc, 0, "services");
    const auto version = member(doc, 0, "version");
    if (!fields(doc, 0, {"version", "services", "sprint_enabled"}) || prJsonKind(doc, version) != PrJsonKind_Number ||
        prJsonNumber(doc, version, -1) != 1 ||
        prJsonKind(doc, rows) != PrJsonKind_Array) {
        why = "services.json requires version 1 and one services array";
        return false;
    }
    const Index sprint = member(doc, 0, "sprint_enabled");
    if (sprint != kPrJsonMissing && prJsonKind(doc, sprint) != PrJsonKind_True && prJsonKind(doc, sprint) != PrJsonKind_False) {
        why = "sprint_enabled must be true or false";
        return false;
    }
    const bool sprintEnabled = sprint == kPrJsonMissing || prJsonKind(doc, sprint) == PrJsonKind_True;
    std::vector<ManagedService> selected;
    std::set<std::string> names;
    for (auto row = prJsonFirst(doc, rows); row != kPrJsonMissing; row = prJsonAfter(doc, rows, row)) {
        ManagedService service;
        if (names.size() == kMaxServices || !fields(doc, row, {"name", "owner", "argv", "env", "cwd", "log", "enabled", "every", "feature"}) ||
            !stringAt(doc, member(doc, row, "name"), service.name) || !accounts::isName(service.name) ||
            !names.insert(service.name).second) {
            why = "services.json allows 256 distinct named service objects; check the names and fields";
            return false;
        }
        const Index owner = member(doc, row, "owner");
        if (owner == kPrJsonMissing) continue;
        std::string value;
        if (!stringAt(doc, owner, value) || (value != "nexus-server" && value != "host")) {
            why = "service " + service.name + " owner must be host or nexus-server";
            return false;
        }
        if (value == "host") continue;
        const Index enabled = member(doc, row, "enabled");
        if (prJsonKind(doc, enabled) == PrJsonKind_False) continue;
        if (enabled != kPrJsonMissing && prJsonKind(doc, enabled) != PrJsonKind_True) {
            why = "service " + service.name + " enabled must be true or false";
            return false;
        }
        const std::string problem = readCommand(doc, row, service);
        if (!problem.empty()) { why = "service " + service.name + ": " + problem; return false; }
        const auto feature = member(doc, row, "feature");
        if (feature != kPrJsonMissing) {
            std::string tag;
            if (!stringAt(doc, feature, tag) || tag != "sprint") { why = "service feature must be sprint or omitted"; return false; }
            if (!sprintEnabled) continue;
        }
        selected.push_back(std::move(service));
    }
    out = std::move(selected);
    why.clear();
    return true;
}

bool readServiceManifest(const std::string& path, std::vector<ManagedService>& out, std::string& why) {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) { out.clear(); why.clear(); return true; }
        why = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(fd);
        why = path + " must be a regular file owned by the server user and writable only by that user";
        return false;
    }
    std::string text;
    std::array<char, 8192> chunk{};
    while (text.size() <= kMaxBytes) {
        const ssize_t count = read(fd, chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            why = "cannot read " + path + ": " + std::strerror(errno);
            close(fd);
            return false;
        }
        if (count == 0) break;
        text.append(chunk.data(), static_cast<size_t>(count));
    }
    close(fd);
    return parseServiceManifest(text, out, why);
}

} // namespace nexus::server
