// A lightweight POSIX server binary placed inside test builds (`builds/<hash>/bin/nexus-server`) by
// handover_functional. Holds its placed build folder lock (`builds::holdBuild`), opens the real `store::Store` under
// `<dir>/store` (holding its OS lock), binds `<dir>/server.sock`, and speaks `status`, `drain`, `stop`, `turn open`,
// `turn end`, and `sessions` using the real `Drain` and `statusBuildLine` logic.

#include "server/builds.h"
#include "server/drain.h"
#include "server/handover.h"
#include "server/migration.h"
#include "server/supervision.h"
#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "store/store.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

using namespace nexus;

namespace {

volatile sig_atomic_t gStopSig = 0;
void onSig(int) { gStopSig = 1; }

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string selfPath() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::string();
    }
    char* real = ::realpath(buf.data(), nullptr);
    if (real == nullptr) {
        return std::string(buf.data());
    }
    std::string out(real);
    std::free(real);
    return out;
#else
    char* real = ::realpath("/proc/self/exe", nullptr);
    if (real != nullptr) {
        std::string out(real);
        std::free(real);
        return out;
    }
    return "/proc/self/exe";
#endif
}

std::string readMode(const std::string& exe) {
    // Look for `<build root>/drivers/mode.txt` beside `bin/nexus-server`.
    const std::size_t binPos = exe.rfind("/bin/nexus-server");
    if (binPos == std::string::npos) {
        return "good";
    }
    const std::string cfg = exe.substr(0, binPos) + "/drivers/mode.txt";
    std::ifstream in(cfg);
    std::string word;
    if (in >> word) {
        return word;
    }
    return "good";
}

store::StoreOptions readStoreOpts(const std::string& exe) {
    store::StoreOptions opts;
    const std::size_t binPos = exe.rfind("/bin/nexus-server");
    if (binPos == std::string::npos) {
        return opts;
    }
    std::ifstream in(exe.substr(0, binPos) + "/drivers/store_readers.txt");
    int maxRec = 0;
    int maxSnap = 0;
    if (in >> maxRec >> maxSnap) {
        opts.max_record_reader = static_cast<std::uint8_t>(maxRec);
        opts.max_snapshot_reader = static_cast<std::uint8_t>(maxSnap);
    }
    return opts;
}

bool appendFormat5Record(store::Fs& fs, const std::string& storeDir, std::uint8_t readerByte, std::uint64_t sid) {
    store::Writer rec;
    rec.u8(5);
    rec.u8(readerByte);
    rec.u64(sid);
    rec.u32(2);
    {
        store::Writer entryInner;
        entryInner.u8(1);
        entryInner.u8(1);
        entryInner.str(readerByte >= 5 ? "format5-breaking-entry" : "format5-compat-entry");
        entryInner.str("future-tail");
        store::Writer changeFrame;
        changeFrame.bytes(entryInner.out());
        rec.u8(static_cast<std::uint8_t>(store::Change::Kind::Append));
        rec.bytes(changeFrame.out());
    }
    {
        store::Writer unknownFrame;
        unknownFrame.str("future-unknown-change");
        rec.u8(99);
        rec.bytes(unknownFrame.out());
    }
    rec.str("");
    rec.u64(0);
    rec.str("future-record-tail");

    std::unique_ptr<store::FileLock> lock;
    if (!fs.lock(storeDir, store::LockMode::Exclusive, lock).is_ok()) {
        return false;
    }
    store::Journal j(fs, storeDir);
    store::Journal::Recovery recov;
    if (!j.open(recov).is_ok()) {
        return false;
    }
    return j.append(rec.out()).is_ok() && j.sync().is_ok();
}

bool sendAll(int fd, const std::string& s) {
    std::size_t off = 0;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, flags);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool readLine(int fd, std::string* out) {
    out->clear();
    char ch = 0;
    while (out->size() < 2048) {
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 2000);
        if (pr <= 0) {
            return false;
        }
        const ssize_t n = ::read(fd, &ch, 1);
        if (n <= 0) {
            return false;
        }
        if (ch == '\n') {
            if (!out->empty() && out->back() == '\r') {
                out->pop_back();
            }
            return true;
        }
        out->push_back(ch);
    }
    return false;
}

bool probeExisting(const std::string& sockPath) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof addr.sun_path) {
        return false;
    }
    std::memcpy(addr.sun_path, sockPath.data(), sockPath.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    bool serving = false;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) {
        std::string line;
        if (sendAll(fd, "status\n") && readLine(fd, &line) && line == "SERVING") {
            serving = true;
        }
    }
    ::close(fd);
    return serving;
}

} // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = onSig;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    std::string cmd = argc > 1 ? argv[1] : "";
    std::string dir;
    std::vector<std::string> extraArgs;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else {
            extraArgs.push_back(argv[i]);
        }
    }
    if (cmd == "supervise" && !dir.empty()) {
        const char* envHome = std::getenv("HOME");
        const std::string home = (envHome && *envHome) ? std::string(envHome) : dir;
        server::supervision::Options opts;
        if (const char* b = std::getenv("NEXUS_SUPERVISE_BACKOFF_MS")) {
            opts.backoff_ms = std::strtoull(b, nullptr, 10);
        }
        if (const char* s = std::getenv("NEXUS_SUPERVISE_STABLE_MS")) {
            opts.stable_ms = std::strtoull(s, nullptr, 10);
        }
        server::supervision::Result r;
        const int code = server::supervision::command(dir, home, extraArgs, opts, r);
        for (const std::string& l : r.out) {
            std::printf("%s\n", l.c_str());
        }
        std::fflush(stdout);
        for (const std::string& l : r.err) {
            std::fprintf(stderr, "nexus-server: supervise: %s\n", l.c_str());
        }
        return code;
    }
    if (cmd == "handover" && !dir.empty()) {
        server::handover::Result r;
        const int code = server::handover::command(dir, extraArgs, {}, r);
        for (const std::string& l : r.out) {
            std::printf("%s\n", l.c_str());
        }
        std::fflush(stdout);
        for (const std::string& l : r.err) {
            std::fprintf(stderr, "nexus-server: handover: %s\n", l.c_str());
        }
        return code;
    }
    if (cmd != "serve" || dir.empty()) {
        std::fprintf(stderr, "fake_build_server: usage: nexus-server serve|supervise|handover ... --dir <DIR>\n");
        return 2;
    }

    const std::string exe = selfPath();
    server::builds::BuildHold hold;
    const store::Status hs = server::builds::holdBuild(exe, hold);
    if (!hs.is_ok()) {
        std::fprintf(stderr, "fake_build_server: %s\n", hs.message().c_str());
        return 1;
    }

    const std::string mode = readMode(exe);
    if (mode == "crash") {
        std::fprintf(stderr, "fake_build_server: simulated startup crash\n");
        return 1;
    }

    const std::string sockPath = dir + "/server.sock";
    if (probeExisting(sockPath)) {
        // Another server already answers SERVING (matches StartOrder kRefusedExit = 0).
        return 0;
    }

    std::unique_ptr<store::Fs> fs = store::make_posix_fs();
    if (const char* envHome = std::getenv("HOME"); envHome != nullptr && *envHome != '\0') {
        std::string h = envHome;
        while (h.size() > 1 && h.back() == '/') {
            h.pop_back();
        }
        std::string sdir = dir;
        while (sdir.size() > 1 && sdir.back() == '/') {
            sdir.pop_back();
        }
        const std::string top = h + "/.nexus";
        if (sdir == top + "/server") {
            server::migration::Options mopts;
            mopts.old_server_live = [&] { return server::migration::probeOldServerSocket(top + "/server.sock"); };
            server::migration::Report mrep;
            const store::Status ms = server::migration::migrateOldLayout(*fs, top, sdir, mopts, &mrep);
            if (!ms.is_ok()) {
                std::fprintf(stderr, "fake_build_server: migration failed: %s\n", ms.message().c_str());
                return mrep.conflict ? 3 : 1;
            }
        }
    }
    if (!fs->private_dir(dir).is_ok() || !fs->private_dir(dir + "/store").is_ok()) {
        std::fprintf(stderr, "fake_build_server: cannot create store dir\n");
        return 1;
    }

    const store::StoreOptions storeOpts = readStoreOpts(exe);
    std::unique_ptr<store::Store> st;
    store::OpenReport rep;
    store::Status openSt = store::Status::busy("waiting");
    const std::uint64_t waitStart = nowMs();
    while (nowMs() - waitStart < 5000) {
        if (probeExisting(sockPath)) {
            return 0;
        }
        openSt = store::Store::open(*fs, dir + "/store", storeOpts, st, &rep);
        if (openSt.is_ok() || openSt.code() != store::Status::Code::Busy) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (!openSt.is_ok()) {
        std::fprintf(stderr, "fake_build_server: store open failed: %s\n", openSt.message().c_str());
        return openSt.code() == store::Status::Code::Busy ? 3 : 1;
    }

    if (mode == "hang") {
        // Holds the store lock and never binds server.sock, simulating a wedged new build.
        while (!gStopSig) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof addr.sun_path) {
        std::fprintf(stderr, "fake_build_server: socket path too long\n");
        return 1;
    }
    std::memcpy(addr.sun_path, sockPath.data(), sockPath.size());
    ::unlink(sockPath.c_str());
    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0) {
        return 1;
    }
    ::fcntl(listenFd, F_SETFD, FD_CLOEXEC);
    if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::chmod(sockPath.c_str(), 0600) != 0 || ::listen(listenFd, 16) != 0) {
        ::close(listenFd);
        return 1;
    }
    ::fcntl(listenFd, F_SETFL, O_NONBLOCK);

    std::uint64_t drainLimit = server::kDrainMs;
    if (const char* env = std::getenv(server::kDrainLimitVariable)) {
        drainLimit = std::strtoull(env, nullptr, 10);
    }
    server::Drain drain(drainLimit);
    bool stopping = false;
    bool drained = false;
    bool crashed = false;
    bool turnOpen = false;
    const bool legacy = (mode == "legacy");
    const bool crashAfterServe = (mode == "crash-after-serve");
    const std::uint64_t crashDeadline = nowMs() + 150;

    while (!stopping && !gStopSig) {
        if (crashAfterServe && nowMs() >= crashDeadline) {
            crashed = true;
            break;
        }
        if (drain.stopNow(turnOpen, nowMs())) {
            drained = true;
            break;
        }
        int waitMs = 50;
        if (const auto rem = drain.untilDeadline(nowMs())) {
            if (*rem < static_cast<std::uint64_t>(waitMs)) {
                waitMs = static_cast<int>(*rem);
            }
        }
        pollfd pfd{listenFd, POLLIN, 0};
        if (::poll(&pfd, 1, waitMs) < 0 && errno != EINTR) {
            break;
        }
        if (pfd.revents & POLLIN) {
            const int cfd = ::accept(listenFd, nullptr, nullptr);
            if (cfd >= 0) {
                std::string req;
                if (readLine(cfd, &req)) {
                    if (req == "status") {
                        if (legacy) {
                            sendAll(cfd, "SERVING\n");
                        } else {
                            sendAll(cfd, "SERVING\n" + server::statusBuildLine(hold.hash(), drain.draining()) + "\n");
                        }
                    } else if (req == "stop") {
                        server::supervision::noteIntentionalStop(dir);
                        sendAll(cfd, "OK\n");
                        stopping = true;
                    } else if (req == "crash") {
                        sendAll(cfd, "OK\n");
                        crashed = true;
                        stopping = true;
                    } else if (req == "drain") {
                        if (legacy) {
                            sendAll(cfd, "ERROR unknown request; legacy server\n");
                        } else {
                            drain.request(nowMs());
                            sendAll(cfd, "OK\n");
                        }
                    } else if (req == "turn open") {
                        turnOpen = true;
                        sendAll(cfd, "OK\n");
                    } else if (req == "turn end") {
                        // Commit a durable session entry before ending the turn to prove zero data loss across drain.
                        std::vector<store::Change> changes;
                        std::uint64_t sid = 0;
                        if (st->sessions().empty()) {
                            sid = st->next_session();
                            store::Change mk;
                            mk.kind = store::Change::Kind::SetInfo;
                            mk.info.friend_name = "friend";
                            mk.info.harness = "claude";
                            changes.push_back(mk);
                        } else {
                            sid = st->sessions().begin()->first;
                        }
                        store::Change app;
                        app.kind = store::Change::Kind::Append;
                        app.entry = store::Entry{1, "turn-completed-before-drain", true};
                        changes.push_back(app);
                        (void)st->commit(sid, changes);
                        turnOpen = false;
                        sendAll(cfd, "OK\n");
                    } else if (req == "entries") {
                        std::size_t count = 0;
                        for (const auto& [id, ses] : st->sessions()) {
                            (void)id;
                            count += ses.transcript.size();
                        }
                        sendAll(cfd, std::to_string(count) + "\n");
                    } else if (req == "sessions") {
                        sendAll(cfd, std::to_string(st->sessions().size()) + "\n");
                    } else if (req == "write-format5-compat" || req == "write-format5-break") {
                        std::uint64_t sid = 0;
                        if (st->sessions().empty()) {
                            sid = st->next_session();
                            store::Change mk;
                            mk.kind = store::Change::Kind::SetInfo;
                            mk.info.friend_name = "friend";
                            mk.info.harness = "claude";
                            const store::Change initChanges[] = {mk};
                            (void)st->commit(sid, initChanges);
                        } else {
                            sid = st->sessions().begin()->first;
                        }
                        st.reset();
                        const std::uint8_t rbyte = (req == "write-format5-break") ? 5 : 3;
                        const bool wrote = appendFormat5Record(*fs, dir + "/store", rbyte, sid);
                        const store::Status ro = store::Store::open(*fs, dir + "/store", storeOpts, st, &rep);
                        if (wrote && ro.is_ok()) {
                            sendAll(cfd, "OK\n");
                        } else {
                            sendAll(cfd, "ERROR format5 write failed\n");
                            crashed = true;
                            stopping = true;
                        }
                    } else {
                        sendAll(cfd, "ERROR unknown request;\n");
                    }
                }
                ::close(cfd);
            }
        }
        if (drain.stopNow(turnOpen, nowMs())) {
            drained = true;
            break;
        }
    }

    ::close(listenFd);
    ::unlink(sockPath.c_str());
    st.reset();
    if (crashed) {
        return 1;
    }
    return drained ? server::kDrainExit : 0;
}
