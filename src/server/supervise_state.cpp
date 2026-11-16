// Durable supervision state codec and atomic updates (server/supervise_state.h).

#include "server/supervise_state.h"

#include "server/build_manifest.h"

#include <atomic>
#include <charconv>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace nexus::server::supervision {

using store::Status;

namespace {

constexpr const char* kStateHeader = "nexus-supervise 1";

std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    return std::to_string(::getpid()) + "-" + std::to_string(++counter);
}

store::Bytes toBytes(const std::string& s) {
    return store::Bytes(reinterpret_cast<const std::uint8_t*>(s.data()),
                        reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
}

std::string fromBytes(const store::Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

bool parseUint32(const std::string& text, std::uint32_t* out) {
    if (text.empty()) {
        return false;
    }
    std::uint32_t val = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, val);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    *out = val;
    return true;
}

bool parseBool01(const std::string& text, bool* out) {
    if (text == "0") {
        *out = false;
        return true;
    }
    if (text == "1") {
        *out = true;
        return true;
    }
    return false;
}

bool superviseDirExists(store::Fs& fs, const std::string& superviseDir) {
    std::vector<std::string> entries;
    return fs.list(superviseDir, entries).is_ok();
}

Status loadExistingOrDefault(store::Fs& fs, const std::string& superviseDir, SuperviseState* out) {
    *out = SuperviseState{};
    store::Bytes raw;
    const Status rs = fs.read_all(superviseDir + "/" + kStateFile, raw);
    if (!rs.is_ok()) {
        if (rs.code() == Status::Code::NotFound) {
            return Status::ok();
        }
        return rs;
    }
    std::string why;
    if (!readState(fromBytes(raw), out, &why)) {
        *out = SuperviseState{};
    }
    return Status::ok();
}

} // namespace

std::string formatState(const SuperviseState& st) {
    std::string out = std::string(kStateHeader) + "\n";
    out += "build " + (st.build.empty() ? std::string("none") : st.build) + "\n";
    out += "crashes " + std::to_string(st.crashes) + "\n";
    out += "halted " + std::string(st.halted ? "1" : "0") + "\n";
    out += "stopped " + std::string(st.stopped ? "1" : "0") + "\n";
    if (!st.picked.empty()) {
        out += "picked " + st.picked + "\n";
    }
    for (const std::string& h : st.shunned) {
        out += "shun " + h + "\n";
    }
    out += "end\n";
    return out;
}

bool readState(const std::string& text, SuperviseState* out, std::string* why) {
    *out = SuperviseState{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) {
        *why = "the supervise state file is empty";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != kStateHeader) {
        *why = "expected header `" + std::string(kStateHeader) + "`, found `" + line + "`";
        return false;
    }
    bool haveBuild = false;
    bool haveCrashes = false;
    bool haveHalted = false;
    bool haveStopped = false;
    bool havePicked = false;
    bool haveEnd = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (haveEnd) {
            if (!line.empty()) {
                *why = "unexpected line after `end`";
                return false;
            }
            continue;
        }
        if (line == "end") {
            haveEnd = true;
            continue;
        }
        if (line.rfind("build ", 0) == 0) {
            if (haveBuild) {
                *why = "duplicate `build` line";
                return false;
            }
            const std::string b = line.substr(6);
            if (b != "none" && !builds::isHash(b)) {
                *why = "invalid build hash `" + b + "`";
                return false;
            }
            out->build = (b == "none") ? std::string() : b;
            haveBuild = true;
        } else if (line.rfind("crashes ", 0) == 0) {
            if (haveCrashes) {
                *why = "duplicate `crashes` line";
                return false;
            }
            if (!parseUint32(line.substr(8), &out->crashes)) {
                *why = "invalid `crashes` count `" + line.substr(8) + "`";
                return false;
            }
            haveCrashes = true;
        } else if (line.rfind("halted ", 0) == 0) {
            if (haveHalted) {
                *why = "duplicate `halted` line";
                return false;
            }
            if (!parseBool01(line.substr(7), &out->halted)) {
                *why = "invalid `halted` flag `" + line.substr(7) + "`";
                return false;
            }
            haveHalted = true;
        } else if (line.rfind("stopped ", 0) == 0) {
            if (haveStopped) {
                *why = "duplicate `stopped` line";
                return false;
            }
            if (!parseBool01(line.substr(8), &out->stopped)) {
                *why = "invalid `stopped` flag `" + line.substr(8) + "`";
                return false;
            }
            haveStopped = true;
        } else if (line.rfind("picked ", 0) == 0) {
            if (havePicked) {
                *why = "duplicate `picked` line";
                return false;
            }
            const std::string p = line.substr(7);
            if (p != "none" && !builds::isHash(p)) {
                *why = "invalid picked hash `" + p + "`";
                return false;
            }
            out->picked = (p == "none") ? std::string() : p;
            havePicked = true;
        } else if (line.rfind("shun ", 0) == 0) {
            const std::string h = line.substr(5);
            if (!builds::isHash(h)) {
                *why = "invalid shunned hash `" + h + "`";
                return false;
            }
            out->shunned.insert(h);
        } else {
            *why = "unknown line `" + line + "`";
            return false;
        }
    }
    if (!haveEnd) {
        *why = "missing `end` line (torn write)";
        return false;
    }
    if (!haveBuild || !haveCrashes || !haveHalted || !haveStopped) {
        *why = "missing required field (`build`, `crashes`, `halted` or `stopped`)";
        return false;
    }
    return true;
}

Status writeStateFile(store::Fs& fs, const std::string& superviseDir, const SuperviseState& st) {
    const std::string tmp = superviseDir + "/" + kStateTmpPrefix + uniqueSuffix();
    const std::string dst = superviseDir + "/" + kStateFile;
    Status s = fs.write_new(tmp, toBytes(formatState(st)));
    if (!s.is_ok()) {
        (void)fs.remove(tmp);
        return s;
    }
    s = fs.rename(tmp, dst);
    if (!s.is_ok()) {
        (void)fs.remove(tmp);
        return s;
    }
    return fs.sync_dir(superviseDir);
}

Status noteHandoverBegin(store::Fs& fs, const std::string& superviseDir) {
    if (!superviseDirExists(fs, superviseDir)) {
        return Status::ok();
    }
    SuperviseState st;
    Status s = loadExistingOrDefault(fs, superviseDir, &st);
    if (!s.is_ok()) {
        return s;
    }
    if (!st.stopped && !st.halted) {
        return Status::ok();
    }
    st.stopped = false;
    st.halted = false;
    return writeStateFile(fs, superviseDir, st);
}

Status noteHandoverTarget(store::Fs& fs, const std::string& superviseDir, const std::string& target) {
    if (!superviseDirExists(fs, superviseDir)) {
        return Status::ok();
    }
    SuperviseState st;
    Status s = loadExistingOrDefault(fs, superviseDir, &st);
    if (!s.is_ok()) {
        return s;
    }
    st.build = target;
    st.crashes = 0;
    st.halted = false;
    st.stopped = false;
    st.picked = target;
    st.shunned.erase(target);
    return writeStateFile(fs, superviseDir, st);
}

Status noteIntentionalStop(store::Fs& fs, const std::string& superviseDir) {
    if (!superviseDirExists(fs, superviseDir)) {
        return Status::ok();
    }
    SuperviseState st;
    Status s = loadExistingOrDefault(fs, superviseDir, &st);
    if (!s.is_ok()) {
        return s;
    }
    if (st.stopped) {
        return Status::ok();
    }
    st.stopped = true;
    return writeStateFile(fs, superviseDir, st);
}

void noteIntentionalStop(const std::string& serverDir) {
    if (serverDir.empty()) {
        return;
    }
    std::unique_ptr<store::Fs> fs = store::make_posix_fs();
    (void)noteIntentionalStop(*fs, serverDir + "/" + kSuperviseDir);
}

} // namespace nexus::server::supervision
