// Layer 5 migration and disk-format reader checks: see migration.h and models/migration/Migration.tla.

#include "server/migration.h"

#include "store/store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <span>

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace nexus::server::migration {

using store::Bytes;
using store::FileLock;
using store::Fs;
using store::LockMode;
using store::Status;

namespace {

bool contains(const std::vector<std::string>& names, const std::string& want) {
    return std::find(names.begin(), names.end(), want) != names.end();
}

std::span<const std::uint8_t> spanOf(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string strOf(const Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

bool isOldTopEntry(const std::string& name) {
    return name == "store" || name == "drivers" || name == "config" || name == "server.log" || name == "server.sock";
}

bool hasAnyOldTop(const std::vector<std::string>& topNames) {
    for (const std::string& n : topNames) {
        if (isOldTopEntry(n)) {
            return true;
        }
    }
    return false;
}

bool isJournalEntry(const std::string& name) {
    return name == "log" || name == "snapshot" || name == "snapshot.tmp" || name == "log.new" ||
           name == store::kGateName;
}

bool isPartName(const std::string& name) {
    return name == "snap" || name == "log" || name == "drivers" || name == "config" || name == "server.log";
}

std::string srcPathOf(const std::string& top, const std::string& part) {
    if (part == "snap") {
        return top + "/store/snapshot";
    }
    if (part == "log") {
        return top + "/store/log";
    }
    return top + "/" + part;
}

std::string stagePathOf(const std::string& dir, const std::string& part) {
    if (part == "snap") {
        return dir + "/store/" + kStagePrefix + "snapshot";
    }
    if (part == "log") {
        return dir + "/store/" + kStagePrefix + "log";
    }
    return dir + "/" + kStagePrefix + part;
}

std::string dstPathOf(const std::string& dir, const std::string& part) {
    if (part == "snap") {
        return dir + "/store/snapshot";
    }
    if (part == "log") {
        return dir + "/store/log";
    }
    return dir + "/" + part;
}

Status probeFile(Fs& fs, const std::string& path, bool* exists) {
    std::uint64_t sz = 0;
    const Status s = fs.size(path, sz);
    if (s.is_ok()) {
        *exists = true;
        return Status::ok();
    }
    if (s.code() == Status::Code::NotFound) {
        *exists = false;
        return Status::ok();
    }
    *exists = false;
    return s;
}

// Removes uncommitted `.migrating.tmp` and `.mig-*` staging files in `d` (if `d` exists).
Status cleanStagingIn(Fs& fs, const std::string& d) {
    std::vector<std::string> names;
    Status s = fs.list(d, names);
    if (s.code() == Status::Code::NotFound) {
        return Status::ok();
    }
    if (!s.is_ok()) {
        return s;
    }
    for (const std::string& n : names) {
        if (n == kPlanTmpFile || n.rfind(kStagePrefix, 0) == 0) {
            s = fs.remove(d + "/" + n);
            if (!s.is_ok() && s.code() != Status::Code::NotFound) {
                return s;
            }
        }
    }
    return Status::ok();
}

// Minimal RFC-8259 top-level object scanner to extract `"version": <uint>` from accounts.json / ais.json without
// pulling Prax into nexus_builds. Skips nested objects, arrays and escaped strings so inner `"version"` keys are
// ignored.
bool parseTopJsonVersion(const std::string& text, std::uint32_t* version) {
    std::size_t i = 0;
    const auto skipWs = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            ++i;
        }
    };
    const auto readString = [&](std::string* out) -> bool {
        if (i >= text.size() || text[i] != '"') {
            return false;
        }
        ++i;
        out->clear();
        while (i < text.size()) {
            const char c = text[i++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (i >= text.size()) {
                    return false;
                }
                out->push_back(text[i++]);
            } else {
                out->push_back(c);
            }
        }
        return false;
    };
    skipWs();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) {
        i = 3;
        skipWs();
    }
    if (i >= text.size() || text[i] != '{') {
        return false;
    }
    ++i;
    int depth = 1;
    bool found = false;
    while (i < text.size() && depth > 0) {
        skipWs();
        if (i >= text.size()) {
            break;
        }
        if (depth == 1 && text[i] == '"') {
            std::string key;
            if (!readString(&key)) {
                return false;
            }
            skipWs();
            if (i >= text.size() || text[i] != ':') {
                return false;
            }
            ++i;
            skipWs();
            if (key == "version") {
                if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) {
                    return false;
                }
                std::uint64_t v = 0;
                while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                    v = v * 10 + std::uint64_t(text[i] - '0');
                    if (v > 0xffffffffULL) {
                        return false;
                    }
                    ++i;
                }
                *version = static_cast<std::uint32_t>(v);
                found = true;
                continue;
            }
            if (i >= text.size()) {
                return false;
            }
        }
        const char c = text[i];
        if (c == '"') {
            std::string ignored;
            if (!readString(&ignored)) {
                return false;
            }
        } else if (c == '{' || c == '[') {
            ++depth;
            ++i;
        } else if (c == '}' || c == ']') {
            --depth;
            ++i;
        } else {
            ++i;
        }
    }
    return found && depth == 0;
}

} // namespace

std::set<std::string> parsePlan(const std::string& text) {
    std::set<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::size_t end = nl == std::string::npos ? text.size() : nl;
        const std::string line = text.substr(start, end - start);
        if (isPartName(line)) {
            out.insert(line);
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

std::string formatPlan(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) {
        out += p;
        out += '\n';
    }
    return out;
}

Status migrateOldLayout(Fs& fs, const std::string& top, const std::string& dir, const Options& options,
                        Report* report) {
    if (report != nullptr) {
        *report = Report{};
    }
    if (top.empty() || top == dir) {
        return Status::ok();
    }
    const auto at = [&](const std::string& phase, const std::string& part = "none") {
        if (options.hook) {
            options.hook(phase, part);
        }
    };
    struct IdleGuard {
        const Options& opts;
        ~IdleGuard() {
            if (opts.hook) {
                opts.hook("idle", "none");
            }
        }
    };
    const bool syncs = options.fault != Fault::NoSync;

    // Fast path: nothing of the old server layout in `top`, and no `.migrating` in `dir`.
    std::vector<std::string> topNames;
    Status s = fs.list(top, topNames);
    if (s.code() == Status::Code::NotFound) {
        topNames.clear();
    } else if (!s.is_ok()) {
        return s;
    }
    const std::string planPath = dir + "/" + kPlanFile;
    bool planPresent = false;
    if (!(s = probeFile(fs, planPath, &planPresent)).is_ok()) {
        return s;
    }
    if (!hasAnyOldTop(topNames) && !planPresent) {
        return Status::ok();
    }

    IdleGuard guard{options};

    // 1. lock: ensure private directories and lock `dir` exclusively so concurrent starts serialize cleanly.
    at("lock");
    if (!(s = fs.private_dir(top)).is_ok() || !(s = fs.private_dir(dir)).is_ok()) {
        return s;
    }
    std::unique_ptr<FileLock> dirLock;
    if (options.fault != Fault::NoLock) {
        s = fs.lock(dir, LockMode::Exclusive, dirLock);
        if (s.code() == Status::Code::Busy) {
            return Status::busy("another nexus-server is migrating " + top + " to " + dir);
        }
        if (!s.is_ok()) {
            return Status::failed(dir + ": migration lock failed: " + s.message());
        }
    }

    // 2. probe: refuse while an old server still answers on `<top>/server.sock` or holds `<top>/store`, or while a
    // running server holds `<dir>/store`.
    at("probe");
    if (!(s = fs.list(top, topNames)).is_ok()) {
        return s;
    }
    std::unique_ptr<FileLock> oldStoreLock;
    std::vector<std::string> oldStoreNames;
    if (options.fault != Fault::IgnoreLiveOld) {
        if (options.old_server_live && options.old_server_live()) {
            return Status::failed("a nexus-server of the old layout still serves on " + top +
                                  "/server.sock; stop it first (`nexus-server stop --dir " + top +
                                  "`), then start nexus-server again");
        }
        if (contains(topNames, "store")) {
            if (!(s = fs.private_dir(top + "/store")).is_ok()) {
                return s;
            }
            s = fs.list(top + "/store", oldStoreNames);
            if (!s.is_ok()) {
                return Status::failed(top + "/store is not a readable directory (" + s.message() +
                                      "); inspect it and move it aside");
            }
            s = fs.lock(top + "/store", LockMode::Exclusive, oldStoreLock);
            if (s.code() == Status::Code::Busy) {
                return Status::busy("a process of the old layout still holds " + top +
                                    "/store open; stop it (`nexus-server stop --dir " + top +
                                    "`), then start nexus-server again");
            }
            if (!s.is_ok()) {
                return Status::failed(top + "/store: its lock cannot be taken: " + s.message());
            }
        }
    } else if (contains(topNames, "store")) {
        if (!(s = fs.private_dir(top + "/store")).is_ok()) {
            return s;
        }
        s = fs.list(top + "/store", oldStoreNames);
        if (!s.is_ok()) {
            return s;
        }
    }

    std::vector<std::string> dirNames;
    if (!(s = fs.list(dir, dirNames)).is_ok()) {
        return s;
    }
    std::unique_ptr<FileLock> newStoreLock;
    if (contains(dirNames, "store")) {
        if (!(s = fs.private_dir(dir + "/store")).is_ok()) {
            return s;
        }
        if (options.fault != Fault::NoLock) {
            s = fs.lock(dir + "/store", LockMode::Exclusive, newStoreLock);
            if (s.code() == Status::Code::Busy) {
                return Status::busy("a running nexus-server holds " + dir + "/store open");
            }
            if (!s.is_ok()) {
                return Status::failed(dir + "/store: its lock cannot be taken: " + s.message());
            }
        }
    }

    // 3. plan: remove uncommitted `.migrating.tmp` / `.mig-*` staging files, then either resume `.migrating` (settling
    // directories first!) or check conflicts and write `.migrating`.
    at("plan");
    if (!(s = cleanStagingIn(fs, dir)).is_ok() || !(s = cleanStagingIn(fs, dir + "/store")).is_ok()) {
        return s;
    }
    if (!(s = fs.list(top, topNames)).is_ok() || !(s = fs.list(dir, dirNames)).is_ok()) {
        return s;
    }
    const bool hasPlan = contains(dirNames, kPlanFile);
    if (!hasAnyOldTop(topNames) && !hasPlan) {
        at("unlock");
        newStoreLock.reset();
        oldStoreLock.reset();
        dirLock.reset();
        return Status::ok();
    }

    const std::vector<std::string> kOrder{"snap", "log", "drivers", "config", "server.log"};
    std::vector<std::string> planOrder;
    bool wrotePlan = false;

    if (hasPlan) {
        Bytes planBytes;
        if (!(s = fs.read_all(planPath, planBytes)).is_ok()) {
            return s;
        }
        const std::set<std::string> planned = parsePlan(strOf(planBytes));
        for (const std::string& p : kOrder) {
            if (planned.count(p) != 0) {
                planOrder.push_back(p);
            }
        }
        if (report != nullptr) {
            report->resumed = true;
        }
        // Settle `.migrating`, `dir/store`, `dir` and `top` before advancing (Migration.tla MPlanSync on resume):
        // if an earlier run was killed after writing `.migrating` or renaming a file into `dir` without syncing the
        // directory, settling makes `.migrating` durable before any destination file in `dir/store` is synced.
        if (syncs) {
            at("plan_sync");
            if (!(s = fs.sync(planPath)).is_ok()) {
                return s;
            }
            if (contains(dirNames, "store") && !(s = fs.sync_dir(dir + "/store")).is_ok()) {
                return s;
            }
            if (!(s = fs.sync_dir(dir)).is_ok() || !(s = fs.sync_dir(top)).is_ok()) {
                return s;
            }
        }
    } else {
        const auto unlockAndConflict = [&](const std::string& msg) {
            if (report != nullptr) {
                report->conflict = true;
            }
            at("unlock");
            newStoreLock.reset();
            oldStoreLock.reset();
            dirLock.reset();
            return Status::failed(msg);
        };
        if (contains(topNames, "store")) {
            if (!(s = fs.list(top + "/store", oldStoreNames)).is_ok()) {
                return s;
            }
            for (const std::string& n : oldStoreNames) {
                if (!isJournalEntry(n)) {
                    return unlockAndConflict(top + "/store holds `" + n +
                                             "`, which is not a journal file; inspect it and move it out before migrating");
                }
            }
        }
        std::vector<std::string> newStoreNames;
        if (contains(dirNames, "store") && !(s = fs.list(dir + "/store", newStoreNames)).is_ok()) {
            return s;
        }
        const bool topHasStore = contains(oldStoreNames, "snapshot") || contains(oldStoreNames, "log");
        const bool dirHasStore = contains(newStoreNames, "snapshot") || contains(newStoreNames, "log");

        if (options.fault != Fault::ClobberNew) {
            if (topHasStore && dirHasStore) {
                return unlockAndConflict("both the old layout (" + top + "/store) and " + dir +
                                         "/store hold store files, so neither was overwritten; check them "
                                         "(`nexus-server store check --dir " + top +
                                         "` and `nexus-server store check`) and move the unwanted one aside");
            }
            for (const char* item : {"drivers", "config"}) {
                if (!contains(topNames, item)) {
                    continue;
                }
                Bytes oldBytes;
                if (!(s = fs.read_all(top + "/" + item, oldBytes)).is_ok()) {
                    return Status::failed("cannot read " + top + "/" + item + " (" + s.message() + ")");
                }
                if (contains(dirNames, item)) {
                    Bytes newBytes;
                    if (!(s = fs.read_all(dir + "/" + item, newBytes)).is_ok()) {
                        return Status::failed("cannot read " + dir + "/" + item + " (" + s.message() + ")");
                    }
                    if (oldBytes != newBytes) {
                        return unlockAndConflict(std::string("both ") + top + "/" + item + " and " + dir + "/" + item +
                                                 " exist and differ, so neither was overwritten; keep the one you want in " +
                                                 dir + "/" + item + " and move " + top + "/" + item + " aside");
                    }
                }
            }
        }

        const bool clobber = options.fault == Fault::ClobberNew;
        if (contains(oldStoreNames, "snapshot") && (clobber || !contains(newStoreNames, "snapshot"))) {
            planOrder.push_back("snap");
        }
        if (contains(oldStoreNames, "log") && (clobber || !contains(newStoreNames, "log"))) {
            planOrder.push_back("log");
        }
        if (contains(topNames, "drivers") && (clobber || !contains(dirNames, "drivers"))) {
            planOrder.push_back("drivers");
        }
        if (contains(topNames, "config") && (clobber || !contains(dirNames, "config"))) {
            planOrder.push_back("config");
        }
        if (contains(topNames, "server.log") && !contains(dirNames, "server.log")) {
            planOrder.push_back("server.log");
        }

        if (!planOrder.empty() && options.fault != Fault::NoPlan) {
            const std::string tmpPath = dir + "/" + kPlanTmpFile;
            const std::string text = formatPlan(planOrder);
            if (!(s = fs.create(tmpPath)).is_ok() || !(s = fs.append(tmpPath, spanOf(text))).is_ok()) {
                return s;
            }
            if (syncs && !(s = fs.sync(tmpPath)).is_ok()) {
                return s;
            }
            if (!(s = fs.rename(tmpPath, planPath)).is_ok()) {
                return s;
            }
            wrotePlan = true;
            if (syncs) {
                at("plan_sync");
                if (!(s = fs.sync_dir(dir)).is_ok()) {
                    return s;
                }
            }
        }
    }

    // 4. stage, sync_stg, rename: copy each planned item not yet placed in `dir`, syncing before rename.
    const bool clobber = options.fault == Fault::ClobberNew;
    bool placedAny = false;
    bool storeMoved = false;
    for (const std::string& part : planOrder) {
        const std::string dst = dstPathOf(dir, part);
        bool dstExists = false;
        if (!(s = probeFile(fs, dst, &dstExists)).is_ok()) {
            return s;
        }
        if (!clobber && dstExists) {
            continue;
        }
        const std::string src = srcPathOf(top, part);
        Bytes bytes;
        s = fs.read_all(src, bytes);
        if (s.code() == Status::Code::NotFound) {
            bool nowExists = false;
            if (!(s = probeFile(fs, dst, &nowExists)).is_ok()) {
                return s;
            }
            if (nowExists) {
                continue;
            }
            s = Status::not_found("missing " + src);
        }
        if (!s.is_ok()) {
            return s;
        }
        if (part == "snap" || part == "log") {
            if (!(s = fs.private_dir(dir + "/store")).is_ok()) {
                return s;
            }
            if (!newStoreLock && options.fault != Fault::NoLock) {
                if (!(s = fs.lock(dir + "/store", LockMode::Exclusive, newStoreLock)).is_ok()) {
                    return s;
                }
            }
        }
        at("stage", part);
        const std::string stg = stagePathOf(dir, part);
        bool stgExists = false;
        if (!(s = probeFile(fs, stg, &stgExists)).is_ok()) {
            return s;
        }
        if (stgExists && !(s = fs.remove(stg)).is_ok()) {
            return s;
        }
        if (!(s = fs.create(stg)).is_ok() || !(s = fs.append(stg, bytes)).is_ok()) {
            return s;
        }
        if (syncs) {
            at("sync_stg", part);
            if (!(s = fs.sync(stg)).is_ok()) {
                return s;
            }
        }
        at("rename", part);
        if (!(s = fs.rename(stg, dst)).is_ok()) {
            return s;
        }
        placedAny = true;
        if (part == "snap" || part == "log") {
            storeMoved = true;
        } else if (report != nullptr) {
            report->moved.push_back(part);
        }
    }
    if (storeMoved && report != nullptr) {
        report->moved.insert(report->moved.begin(), "store");
    }

    if (placedAny && syncs) {
        at("sync_placed");
        bool hasLog = false;
        bool hasSnap = false;
        if (!(s = probeFile(fs, dir + "/store/log", &hasLog)).is_ok() ||
            !(s = probeFile(fs, dir + "/store/snapshot", &hasSnap)).is_ok()) {
            return s;
        }
        if ((storeMoved || hasLog || hasSnap) && !(s = fs.sync_dir(dir + "/store")).is_ok()) {
            return s;
        }
        if (!(s = fs.sync_dir(dir)).is_ok()) {
            return s;
        }
    }

    // 5. clean_top, sync_top: remove migrated old entries from `top` only after every destination file is durable.
    if (options.fault != Fault::LeaveOld) {
        if (!(s = fs.list(top, topNames)).is_ok()) {
            return s;
        }
        if (!hasAnyOldTop(topNames)) {
            at("clean_top");
        } else {
            if (contains(topNames, "store")) {
                if (!(s = fs.list(top + "/store", oldStoreNames)).is_ok()) {
                    return s;
                }
                for (const char* aux : {"snapshot.tmp", "log.new", store::kGateName}) {
                    if (contains(oldStoreNames, aux)) {
                        if (!(s = fs.remove(top + "/store/" + aux)).is_ok() && s.code() != Status::Code::NotFound) {
                            return s;
                        }
                    }
                }
                const bool hadSnap = contains(oldStoreNames, "snapshot");
                const bool hadLog = contains(oldStoreNames, "log");
                if (hadSnap) {
                    at("clean_top", "snap");
                    if (!(s = fs.remove(top + "/store/snapshot")).is_ok()) {
                        return s;
                    }
                    if (!hadLog) {
                        if (syncs && !(s = fs.sync_dir(top + "/store")).is_ok()) {
                            return s;
                        }
                        oldStoreLock.reset();
                        if (!(s = fs.remove_dir(top + "/store")).is_ok()) {
                            return s;
                        }
                    }
                }
                if (hadLog) {
                    at("clean_top", "log");
                    if (!(s = fs.remove(top + "/store/log")).is_ok()) {
                        return s;
                    }
                    if (syncs && !(s = fs.sync_dir(top + "/store")).is_ok()) {
                        return s;
                    }
                    oldStoreLock.reset();
                    if (!(s = fs.remove_dir(top + "/store")).is_ok()) {
                        return s;
                    }
                }
                if (!hadSnap && !hadLog) {
                    oldStoreLock.reset();
                    if (!(s = fs.remove_dir(top + "/store")).is_ok()) {
                        return s;
                    }
                }
            }
            for (const char* item : {"drivers", "config", "server.log", "server.sock"}) {
                if (contains(topNames, item)) {
                    at("clean_top", item);
                    if (!(s = fs.remove(top + "/" + item)).is_ok()) {
                        return s;
                    }
                }
            }
            if (syncs) {
                at("sync_top");
                if (!(s = fs.sync_dir(top)).is_ok()) {
                    return s;
                }
            }
        }
    }

    // 6. finish: remove `.migrating` and sync `dir`.
    bool planLeft = false;
    if (!(s = probeFile(fs, planPath, &planLeft)).is_ok()) {
        return s;
    }
    if (hasPlan || wrotePlan || planLeft) {
        at("finish");
        if (planLeft && !(s = fs.remove(planPath)).is_ok()) {
            return s;
        }
        if (syncs && !(s = fs.sync_dir(dir)).is_ok()) {
            return s;
        }
    }

    // 7. unlock.
    at("unlock");
    newStoreLock.reset();
    oldStoreLock.reset();
    dirLock.reset();
    if (report != nullptr) {
        report->migrated = true;
    }
    return Status::ok();
}

Status readDiskFormats(Fs& fs, const std::string& serverDir, builds::DiskFormats* out,
                       const std::function<std::uint64_t()>& now_ms) {
    *out = builds::DiskFormats{};
    store::StoreReaders sr;
    Status s = store::Store::inspect_readers(fs, serverDir + "/store", sr, now_ms);
    if (!s.is_ok()) {
        return s;
    }
    out->storeRecordReader = sr.record_reader;
    out->storeRecordSeq = sr.record_seq;
    out->storeSnapshotReader = sr.snapshot_reader;

    // friends.json: ais.json's name before the friend names became AI, in a server directory not yet started since.
    for (const char* name : {"accounts.json", "ais.json", "friends.json"}) {
        const std::string path = serverDir + "/nexus-config/" + name;
        Bytes bytes;
        s = fs.read_all(path, bytes);
        if (s.code() == Status::Code::NotFound) {
            continue;
        }
        if (!s.is_ok()) {
            return Status::failed("cannot read " + path + ": " + s.message());
        }
        std::uint32_t v = 0;
        if (!parseTopJsonVersion(strOf(bytes), &v) || v == 0) {
            return Status::failed(path + " has no readable top-level \"version\" number (`nexus-server config check`)");
        }
        if (v > out->configVersion) {
            out->configVersion = v;
            out->configFile = name;
        }
    }
    return Status::ok();
}

#ifndef _WIN32
bool probeOldServerSocket(const std::string& socketPath, int timeoutMs) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof addr.sun_path) {
        return false;
    }
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return false;
    }
    const char req[] = "status\n";
    std::size_t sent = 0;
    while (sent < sizeof(req) - 1) {
        const ssize_t n = ::write(fd, req + sent, (sizeof(req) - 1) - sent);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string reply;
    bool gotLine = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }
        pollfd p{fd, POLLIN, 0};
        const int pr = ::poll(&p, 1, static_cast<int>(left));
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        if (pr <= 0) {
            break;
        }
        char c = 0;
        const ssize_t n = ::read(fd, &c, 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        if (c == '\n') {
            gotLine = true;
            break;
        }
        if (reply.size() > 4096) {
            break;
        }
        reply.push_back(c);
    }
    ::close(fd);
    return gotLine && (reply == "SERVING" || reply.rfind("ERROR unknown request", 0) == 0);
}
#endif

} // namespace nexus::server::migration
