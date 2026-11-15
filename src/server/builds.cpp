// Placement (builds.h): the verbs on the builds directory, each step named as the model names it
// (models/placement/Placement.tla).

#include "server/builds.h"

#include "accounts/registry.h"
#include "driver/protocol.h"
#include "server/sha256.h"
#include "store/fs.h"
#include "store/store.h"
#include "sync/formats.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace nexus::server::builds {

using store::Status;

namespace {

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset(o.fd_);
            o.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }
    // Closes it, and says whether the close succeeded (a close that fails may have lost a write).
    bool close() {
        const int fd = fd_;
        fd_ = -1;
        return fd < 0 || ::close(fd) == 0;
    }

private:
    int fd_ = -1;
};

Status osError(const std::string& what, const std::string& path, int e) {
    const std::string msg = what + " " + path + ": " + std::strerror(e);
    if (e == ENOSPC || e == EDQUOT) {
        return Status::failed(msg + " (the disk is full: free space and run it again; nothing half-placed is used, "
                              "and the next run removes it)");
    }
    if (e == EACCES || e == EPERM || e == EROFS) {
        return Status::failed(msg + " (the user running nexus-server must be able to write there)");
    }
    if (e == ENOENT) {
        return Status::not_found(msg);
    }
    return Status::failed(msg + " (check the disk; `nexus-server builds verify` reads every build back)");
}

Status osError(const std::string& what, const std::string& path) {
    return osError(what, path, errno);
}

Status fullSync(int fd, const std::string& path) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return Status::ok();
    }
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) {
        return osError("F_FULLFSYNC", path);
    }
#endif
    if (::fsync(fd) != 0) {
        return osError("fsync", path);
    }
    return Status::ok();
}

std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() ? name : dir + "/" + name;
}

// The names in a directory, without . and .., sorted (the descriptor is left as it was).
Status listDir(int dirfd, const std::string& path, std::vector<std::string>* names) {
    names->clear();
    const int copy = ::openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) {
        return osError("open", path);
    }
    DIR* d = ::fdopendir(copy);
    if (d == nullptr) {
        const int e = errno;
        ::close(copy);
        return osError("fdopendir", path, e);
    }
    errno = 0;
    while (dirent* ent = ::readdir(d)) {
        const std::string n = ent->d_name;
        if (n != "." && n != "..") {
            names->push_back(n);
        }
        errno = 0;
    }
    const int e = errno;
    ::closedir(d);
    if (e != 0) {
        return osError("readdir", path, e);
    }
    std::sort(names->begin(), names->end());
    return Status::ok();
}

// A whole file in a directory, not following a link; at most `limit` bytes.
Status readFileAt(int dirfd, const std::string& name, const std::string& path, std::string* out,
                  std::size_t limit = std::size_t(1) << 24) {
    out->clear();
    Fd fd(::openat(dirfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0) {
        return osError("open", path);
    }
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return osError("read", path);
        }
        if (n == 0) {
            break;
        }
        out->append(buf, std::size_t(n));
        if (out->size() > limit) {
            return Status::failed(path + " is larger than " + std::to_string(limit) + " bytes");
        }
    }
    return Status::ok();
}

Status readFile(const std::string& path, std::string* out) {
    return readFileAt(AT_FDCWD, path, path, out);
}

Status writeAll(int fd, const char* data, std::size_t size, const std::string& path) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t w = ::write(fd, data + done, size - done);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0) {
            return osError("write", path);
        }
        done += std::size_t(w);
    }
    return Status::ok();
}

// The link at dirfd/name: its target; NotFound when there is none.
Status readLinkAt(int dirfd, const std::string& name, const std::string& path, std::string* target) {
    char buf[4096];
    const ssize_t n = ::readlinkat(dirfd, name.c_str(), buf, sizeof buf);
    if (n < 0) {
        return osError("readlink", path);
    }
    if (std::size_t(n) >= sizeof buf) {
        return Status::failed("the link " + path + " is longer than " + std::to_string(sizeof buf) + " bytes");
    }
    target->assign(buf, std::size_t(n));
    return Status::ok();
}

// Removes dirfd/name and everything in it (a build's folders are read-only: each is made writable first). Gone already
// is fine.
Status removeTree(int dirfd, const std::string& name, const std::string& path) {
    struct stat st;
    if (::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return Status::ok();
        }
        return osError("stat", path);
    }
    if (S_ISDIR(st.st_mode)) {
        if ((st.st_mode & 0700) != 0700 && ::fchmodat(dirfd, name.c_str(), 0700, 0) != 0) {
            return osError("chmod", path);
        }
        Fd d(::openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (d.get() < 0) {
            return osError("open", path);
        }
        std::vector<std::string> names;
        Status s = listDir(d.get(), path, &names);
        if (!s.is_ok()) {
            return s;
        }
        for (const std::string& n : names) {
            s = removeTree(d.get(), n, path + "/" + n);
            if (!s.is_ok()) {
                return s;
            }
        }
        if (::unlinkat(dirfd, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
            return osError("rmdir", path);
        }
        return Status::ok();
    }
    if (::unlinkat(dirfd, name.c_str(), 0) != 0 && errno != ENOENT) {
        return osError("unlink", path);
    }
    return Status::ok();
}

bool exists(int dirfd, const std::string& name) {
    struct stat st;
    return ::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}

std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    return std::to_string(::getpid()) + "-" + std::to_string(++counter);
}

std::uint64_t steadyMs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void realSleep(std::uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A tree read (and, with a destination, copied): every entry under it, each file's bytes hashed as they are read.
struct Walk {
    const Options* options = nullptr;
    bool sync = true;               // each copied file synced before it is closed
    std::uint64_t* written = nullptr;
    std::vector<Entry> entries;
};

Status walkFile(int srcDir, int dstDir, const std::string& name, const std::string& rel, const std::string& srcPath,
                const std::string& dstPath, const struct stat& st, Walk& w) {
    Fd in(::openat(srcDir, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (in.get() < 0) {
        return osError("open", srcPath);
    }
    Entry e;
    e.kind = EntryKind::File;
    e.path = rel;
    e.exec = (st.st_mode & 0111) != 0;
    Fd out;
    if (dstDir >= 0) {
        out.reset(::openat(dstDir, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (out.get() < 0) {
            return osError("create", dstPath);
        }
    }
    Sha256 sha;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(in.get(), buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return osError("read", srcPath);
        }
        if (n == 0) {
            break;
        }
        sha.update(buf, std::size_t(n));
        e.size += std::uint64_t(n);
        if (out.get() >= 0) {
            const std::uint64_t limit = w.options != nullptr ? w.options->test_disk_full_after : 0;
            if (limit != 0 && *w.written + std::uint64_t(n) > limit) {
                return osError("write", dstPath, ENOSPC);
            }
            Status s = writeAll(out.get(), buf, std::size_t(n), dstPath);
            if (!s.is_ok()) {
                return s;
            }
            *w.written += std::uint64_t(n);
        }
    }
    e.sha = sha.hex();
    if (out.get() >= 0) {
        if (::fchmod(out.get(), e.exec ? 0555 : 0444) != 0) {
            return osError("chmod", dstPath);
        }
        if (w.sync) {
            Status s = fullSync(out.get(), dstPath);
            if (!s.is_ok()) {
                return s;
            }
        }
        if (!out.close()) {
            return osError("close", dstPath);
        }
    }
    w.entries.push_back(e);
    return Status::ok();
}

// The entries of the directory srcDir (at `rel` in the build), copied into dstDir when it is not -1.
Status walkDir(int srcDir, int dstDir, const std::string& rel, const std::string& srcPath, const std::string& dstPath,
               Walk& w) {
    std::vector<std::string> names;
    Status s = listDir(srcDir, srcPath, &names);
    if (!s.is_ok()) {
        return s;
    }
    for (const std::string& name : names) {
        const std::string childRel = join(rel, name);
        const std::string childSrc = srcPath + "/" + name;
        const std::string childDst = dstPath + "/" + name;
        const std::string problem = pathProblem(childRel);
        if (!problem.empty()) {
            return Status::refused(childSrc + ": " + problem);
        }
        struct stat st;
        if (::fstatat(srcDir, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            return osError("stat", childSrc);
        }
        if (S_ISDIR(st.st_mode)) {
            Entry e;
            e.kind = EntryKind::Dir;
            e.path = childRel;
            w.entries.push_back(e);
            Fd sub(::openat(srcDir, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (sub.get() < 0) {
                return osError("open", childSrc);
            }
            Fd dsub;
            if (dstDir >= 0) {
                if (::mkdirat(dstDir, name.c_str(), 0700) != 0) {
                    return osError("mkdir", childDst);
                }
                dsub.reset(::openat(dstDir, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (dsub.get() < 0) {
                    return osError("open", childDst);
                }
            }
            s = walkDir(sub.get(), dsub.get(), childRel, childSrc, childDst, w);
        } else if (S_ISREG(st.st_mode)) {
            s = walkFile(srcDir, dstDir, name, childRel, childSrc, childDst, st, w);
        } else if (S_ISLNK(st.st_mode)) {
            Entry e;
            e.kind = EntryKind::Link;
            e.path = childRel;
            s = readLinkAt(srcDir, name, childSrc, &e.target);
            if (s.is_ok()) {
                const std::string bad = linkProblem(childRel, e.target);
                if (!bad.empty()) {
                    return Status::refused(childSrc + ": " + bad);
                }
                if (dstDir >= 0 && ::symlinkat(e.target.c_str(), dstDir, name.c_str()) != 0) {
                    return osError("symlink", childDst);
                }
                w.entries.push_back(e);
            }
        } else {
            return Status::refused(childSrc + " is not a file, a directory or a link, which is all a build holds");
        }
        if (!s.is_ok()) {
            return s;
        }
    }
    return Status::ok();
}

// One top-level part of a tree (bin, drivers, formats, nexus.app), read and, with dstRoot, copied.
Status walkPart(int srcRoot, int dstRoot, const std::string& part, const std::string& srcPath,
                const std::string& dstPath, Walk& w) {
    struct stat st;
    const std::string src = srcPath + "/" + part;
    const std::string dst = dstPath + "/" + part;
    if (::fstatat(srcRoot, part.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return osError("stat", src);
    }
    if (S_ISREG(st.st_mode)) {
        return walkFile(srcRoot, dstRoot, part, part, src, dst, st, w);
    }
    if (!S_ISDIR(st.st_mode)) {
        return Status::refused(src + " is not a file or a directory");
    }
    Entry e;
    e.kind = EntryKind::Dir;
    e.path = part;
    w.entries.push_back(e);
    Fd sub(::openat(srcRoot, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (sub.get() < 0) {
        return osError("open", src);
    }
    Fd dsub;
    if (dstRoot >= 0) {
        if (::mkdirat(dstRoot, part.c_str(), 0700) != 0) {
            return osError("mkdir", dst);
        }
        dsub.reset(::openat(dstRoot, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dsub.get() < 0) {
            return osError("open", dst);
        }
    }
    return walkDir(sub.get(), dsub.get(), part, src, dst, w);
}

// The parts a tree holds, in the order they are written; a name a build cannot hold is refused.
Status treeParts(int root, const std::string& path, std::vector<std::string>* parts, bool allowManifest) {
    std::vector<std::string> names;
    Status s = listDir(root, path, &names);
    if (!s.is_ok()) {
        return s;
    }
    parts->clear();
    for (const std::string& n : names) {
        if (allowManifest && n == kManifest) {
            continue;
        }
        if (std::find(kTopLevel.begin(), kTopLevel.end(), n) == kTopLevel.end()) {
            return Status::refused(path + " holds `" + n + "`, which a build cannot hold: a build holds only bin/, "
                                   "drivers/, formats and nexus.app/");
        }
    }
    for (const std::string& t : kTopLevel) {
        if (std::find(names.begin(), names.end(), t) != names.end()) {
            parts->push_back(t);
        }
    }
    return Status::ok();
}

std::vector<Entry> sortedEntries(std::vector<Entry> v) {
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return v;
}

// The modes a placed build's entries have: directories 0555, files 0555 or 0444. The path of the first that differs,
// or empty.
std::string modeProblem(int root, const std::vector<Entry>& entries) {
    for (const Entry& e : entries) {
        if (e.kind == EntryKind::Link) {
            continue;
        }
        struct stat st;
        if (::fstatat(root, e.path.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            return e.path + " cannot be read";
        }
        const mode_t want = e.kind == EntryKind::Dir || e.exec ? 0555 : 0444;
        if ((st.st_mode & 07777) != want) {
            char mode[8];
            std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
            return e.path + " has mode " + mode + ", not " + (want == 0555 ? "0555" : "0444") + " (read-only)";
        }
    }
    return std::string();
}

// The build folder dirfd/name read back whole against its manifest: every entry there, with its bytes, kind and mode,
// and nothing more; and the manifest's tree is the folder's name. Ok and the manifest, or why it is damaged (Failed).
Status checkFolder(int dirfd, const std::string& name, const std::string& path, Manifest* m, bool leaveOutDrivers,
                   bool checkModes) {
    Fd root(::openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root.get() < 0) {
        return osError("open", path);
    }
    std::string text;
    Status s = readFileAt(root.get(), kManifest, path + "/" + kManifest, &text);
    if (!s.is_ok()) {
        return Status::failed("build " + path + " is damaged: " + s.message());
    }
    std::string why;
    if (!readManifest(text, m, &why, leaveOutDrivers)) {
        return Status::failed("build " + path + " is damaged: " + why);
    }
    if (isHash(name) && m->tree != name) {
        return Status::failed("build " + path + " is damaged: its manifest names tree " + m->tree);
    }
    std::vector<std::string> parts;
    s = treeParts(root.get(), path, &parts, true);
    if (!s.is_ok()) {
        return Status::failed("build " + path + " is damaged: " + s.message());
    }
    Walk w;
    for (const std::string& p : parts) {
        s = walkPart(root.get(), -1, p, path, path, w);
        if (!s.is_ok()) {
            return Status::failed("build " + path + " is damaged: " + s.message());
        }
    }
    const std::vector<Entry> have = sortedEntries(w.entries);
    const std::vector<Entry> want = sortedEntries(m->entries);
    if (have != want) {
        std::size_t i = 0;
        while (i < have.size() && i < want.size() && have[i] == want[i]) {
            ++i;
        }
        const std::string at = i < want.size() ? want[i].path : (i < have.size() ? have[i].path : std::string("?"));
        return Status::failed("build " + path + " is damaged: " + at + " is not what its manifest lists (changed, "
                              "added or removed since it was placed); remove it (`nexus-server builds remove`) and "
                              "place it again");
    }
    if (checkModes) {
        const std::string bad = modeProblem(root.get(), want);
        if (!bad.empty()) {
            return Status::failed("build " + path + " is damaged: " + bad);
        }
    }
    return Status::ok();
}

// The pointer: the pair `active` names, and the builds that pair's name says. None when no build has been made
// current.
struct Pointer {
    bool exists = false;
    std::string pair;
    std::string current;
    std::string previous;
};

int exitOf(const Status& s) {
    switch (s.code()) {
    case Status::Code::Ok:
        return kExitOk;
    case Status::Code::Refused:
        return kExitRefused;
    case Status::Code::Busy:
        return kExitBusy;
    case Status::Code::NotFound:
    case Status::Code::Failed:
        return kExitFailed;
    }
    return kExitFailed;
}

std::string formatsLine(const Formats& f) {
    std::string out;
    for (const Format& x : f) {
        out += (out.empty() ? "" : ",") + x.name + "=" + std::to_string(x.writes) + "/" + std::to_string(x.reader);
    }
    return out.empty() ? std::string("formats:none") : "formats:" + out;
}

} // namespace

// One verb's run: the builds directory open, its lock, and each step named before it is taken.
struct Builds::Run {
    Builds& b;
    Result& r;
    std::string path;           // <server dir>/builds
    Fd dir;
    bool locked = false;

    Run(Builds& builds, Result& result) : b(builds), r(result), path(builds.serverDir_ + "/" + kBuildsDir) {}
    ~Run() {
        b.locked_ = false;
        b.phase_ = "idle";
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    bool fault(Fault f) const { return b.options_.fault == f; }
    bool syncs() const { return !fault(Fault::NoSync); }
    bool leaveOutDrivers() const { return fault(Fault::TestedProgramOnly); }

    void at(const char* step) {
        b.phase_ = step;
        if (b.options_.hook) {
            b.options_.hook(step);
        }
    }

    std::uint64_t now() const { return b.options_.now_ms ? b.options_.now_ms() : steadyMs(); }
    void pause(std::uint64_t ms) const {
        if (b.options_.sleep_ms) {
            b.options_.sleep_ms(ms);
        } else {
            realSleep(ms);
        }
    }

    int end(const Status& s) {
        if (!s.is_ok()) {
            r.err.push_back(s.message());
        }
        r.exit = exitOf(s);
        return r.exit;
    }

    // The builds directory opened (made, with the server's directory, when `make`).
    Status open(bool make) {
        if (make) {
            std::string at;
            const std::string& full = b.serverDir_;
            for (std::size_t i = 1; i <= full.size(); ++i) {
                if (i == full.size() || full[i] == '/') {
                    at = full.substr(0, i);
                    if (::mkdir(at.c_str(), 0700) != 0 && errno != EEXIST) {
                        return osError("mkdir", at);
                    }
                }
            }
            if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
                return osError("mkdir", path);
            }
        }
        dir.reset(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dir.get() < 0) {
            return osError("open", path);
        }
        return Status::ok();
    }

    // The builds lock: tried every kLockRetryMs for the wait, then Busy. Its filesystem is checked first, and once it
    // is held, that a second lock is refused (store/fs.h).
    Status lock(bool exclusive) {
        const std::string type = store::unlockable_filesystem(dir.get(), b.options_.test_fs_type);
        if (!type.empty()) {
            return Status::failed("the builds directory " + path + " is on " + type + ", where a lock taken by one "
                                  "run is not seen by another, so two placements could both go ahead; keep the "
                                  "server's directory on a local disk (--dir, or HOME)");
        }
        if (fault(Fault::NoLock)) {
            return Status::ok();
        }
        const std::uint64_t start = now();
        for (;;) {
            if (::flock(dir.get(), (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EWOULDBLOCK) {
                return osError("flock", path);
            }
            if (now() - start >= b.options_.lock_wait_ms) {
                return Status::busy("another run holds the builds lock on " + path + " (a place, current, back, "
                                    "prune or remove) and did not end within " +
                                    std::to_string(b.options_.lock_wait_ms / 1000) + " s; run this again when it "
                                    "has ended");
            }
            pause(kLockRetryMs);
        }
        locked = true;
        b.locked_ = exclusive;
        return store::locks_kept(dir.get(), path, "a second placement", b.options_.test_locks_ignored);
    }

    Status unlock() {
        at("unlock");
        if (locked && ::flock(dir.get(), LOCK_UN) != 0) {
            return osError("flock unlock", path);
        }
        locked = false;
        b.locked_ = false;
        return Status::ok();
    }

    Status pointer(Pointer* p) {
        *p = Pointer();
        std::string target;
        Status s = readLinkAt(dir.get(), kActive, path + "/" + kActive, &target);
        if (s.code() == Status::Code::NotFound) {
            return Status::ok();
        }
        if (!s.is_ok()) {
            return s;
        }
        if (!readPairName(target, &p->current, &p->previous)) {
            return Status::failed("the pointer " + path + "/" + kActive + " names `" + target + "`, which is not a "
                                  "pair; `nexus-server builds verify` says more");
        }
        p->exists = true;
        p->pair = target;
        return Status::ok();
    }

    // The leftovers of interrupted runs: every temporary name, and every pair the pointer does not name. Only the
    // lock's holder makes them, so whoever made them has ended.
    Status leftovers(std::vector<std::string>* out) {
        out->clear();
        Pointer p;
        Status s = pointer(&p);
        if (!s.is_ok()) {
            return s;
        }
        std::vector<std::string> names;
        s = listDir(dir.get(), path, &names);
        if (!s.is_ok()) {
            return s;
        }
        for (const std::string& n : names) {
            const NameKind k = classify(n);
            const bool left = k == NameKind::PairTmp || k == NameKind::ActiveTmp || k == NameKind::Gone ||
                              (k == NameKind::Tmp && !fault(Fault::ReuseTmp)) || (k == NameKind::Pair && n != p.pair);
            if (left) {
                out->push_back(n);
            }
        }
        return Status::ok();
    }

    // Lock, settle, clean: the start of every verb that changes anything.
    Status begin() {
        Status s = open(true);
        if (!s.is_ok()) {
            return s;
        }
        at("lock");
        s = lock(true);
        if (!s.is_ok()) {
            return s;
        }
        // The directory synced before its leftovers are judged: a rename by a run that died is durable from here.
        at("settle");
        s = fullSync(dir.get(), path);
        if (!s.is_ok()) {
            return s;
        }
        std::vector<std::string> left;
        s = leftovers(&left);
        if (!s.is_ok()) {
            return s;
        }
        for (const std::string& n : left) {
            at("clean");
            s = removeTree(dir.get(), n, path + "/" + n);
            if (!s.is_ok()) {
                return s;
            }
        }
        return Status::ok();
    }

    // Ends a run that holds the lock: the lock released, whatever the step's status (the first failure is kept).
    int finish(const Status& s) {
        Status u = Status::ok();
        if (locked) {
            u = unlock();
        }
        b.phase_ = "idle";
        if (b.options_.hook) {
            b.options_.hook("idle");
        }
        return end(s.is_ok() ? u : s);
    }

    // The folder of a placement that will not be named: removed, then the lock released.
    int drop(const std::string& tmp, const Status& why) {
        at("drop");
        Status s = removeTree(dir.get(), tmp, path + "/" + tmp);
        if (!s.is_ok() && why.is_ok()) {
            return finish(s);
        }
        if (!s.is_ok()) {
            r.err.push_back(s.message());
        }
        return finish(why);
    }

    int place(const std::string& tree, const std::string& evidencePath) {
        // Read before the lock: the tree is there and holds only what a build may; the evidence reads and passed.
        Fd src(::open(tree.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (src.get() < 0) {
            return end(osError("open", tree));
        }
        std::vector<std::string> parts;
        Status s = treeParts(src.get(), tree, &parts, false);
        if (!s.is_ok()) {
            return end(s);
        }
        Evidence ev;
        const bool hasEvidence = !evidencePath.empty();
        if (hasEvidence) {
            std::string text;
            s = readFile(evidencePath, &text);
            if (!s.is_ok()) {
                return end(s);
            }
            std::string why;
            if (!readEvidence(text, &ev, &why)) {
                return end(Status::refused(evidencePath + ": " + why));
            }
            if (ev.result != "pass") {
                return end(Status::refused(evidencePath + " says the tests' result was `" + ev.result + "`: only a "
                                           "tree whose tests passed is placed tested"));
            }
        }

        s = begin();
        if (!s.is_ok()) {
            return finish(s);
        }

        at("mktmp");
        std::string tmp;
        bool reused = false;
        if (fault(Fault::ReuseTmp)) {
            std::vector<std::string> names;
            s = listDir(dir.get(), path, &names);
            if (!s.is_ok()) {
                return finish(s);
            }
            for (const std::string& n : names) {
                if (classify(n) == NameKind::Tmp) {
                    tmp = n;
                    reused = true;
                    break;
                }
            }
        }
        if (tmp.empty()) {
            tmp = std::string(kTmpPrefix) + uniqueSuffix();
            if (::mkdirat(dir.get(), tmp.c_str(), 0700) != 0) {
                return finish(osError("mkdir", path + "/" + tmp));
            }
        }
        const std::string tmpPath = path + "/" + tmp;
        Fd out(::openat(dir.get(), tmp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (out.get() < 0) {
            return finish(osError("open", tmpPath));
        }

        Walk w;
        w.options = &b.options_;
        w.sync = syncs();
        std::uint64_t written = 0;
        w.written = &written;
        for (const std::string& part : parts) {
            if (reused && exists(out.get(), part)) {
                s = walkPart(out.get(), -1, part, tmpPath, tmpPath, w);    // carried on with as it is
            } else {
                at("write");
                s = walkPart(src.get(), out.get(), part, tree, tmpPath, w);
            }
            if (!s.is_ok()) {
                return drop(tmp, s);
            }
        }

        at("manifest");
        Manifest m;
        m.entries = sortedEntries(w.entries);
        const auto program = std::find_if(m.entries.begin(), m.entries.end(),
                                          [](const Entry& e) { return e.path == kProgram; });
        if (program == m.entries.end() || program->kind != EntryKind::File || !program->exec) {
            return drop(tmp, Status::refused(tree + " has no executable " + kProgram + ": a build is a server "
                                             "program and what goes with it"));
        }
        std::string formatsText;
        s = readFileAt(out.get(), kFormatsFile, tmpPath + "/" + kFormatsFile, &formatsText);
        if (!s.is_ok()) {
            return drop(tmp, s.code() == Status::Code::NotFound
                                 ? Status::refused(tree + " has no formats file (`nexus-server builds formats > " +
                                                   tree + "/formats` writes it)")
                                 : s);
        }
        std::string why;
        if (!readFormats(formatsText, &m.formats, &why)) {
            return drop(tmp, Status::refused(tree + "/formats: " + why));
        }
        m.tree = treeHash(m.entries, leaveOutDrivers());
        if (hasEvidence) {
            if (ev.tree != m.tree) {
                return drop(tmp, Status::refused(
                    "the evidence " + evidencePath + " names tree " + ev.tree + ", and this tree is " + m.tree +
                    ": the tests ran on other bytes (a file changed since they ran, the driver tree among them); run "
                    "the tests on this tree, write its evidence from `nexus-server builds hash`, and place it again"));
            }
            m.tested = true;
            m.run = ev.run;
        }
        const std::string manifest = manifestText(m);
        if (reused && ::unlinkat(out.get(), kManifest, 0) != 0 && errno != ENOENT) {
            return drop(tmp, osError("unlink", tmpPath + "/" + kManifest));
        }
        {
            Fd mf(::openat(out.get(), kManifest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0444));
            if (mf.get() < 0) {
                return drop(tmp, osError("create", tmpPath + "/" + kManifest));
            }
            const std::uint64_t limit = b.options_.test_disk_full_after;
            if (limit != 0 && written + manifest.size() > limit) {
                return drop(tmp, osError("write", tmpPath + "/" + kManifest, ENOSPC));
            }
            s = writeAll(mf.get(), manifest.data(), manifest.size(), tmpPath + "/" + kManifest);
            if (s.is_ok() && syncs()) {
                s = fullSync(mf.get(), tmpPath + "/" + kManifest);
            }
            if (s.is_ok() && !mf.close()) {
                s = osError("close", tmpPath + "/" + kManifest);
            }
            if (!s.is_ok()) {
                return drop(tmp, s);
            }
        }

        if (syncs()) {
            at("sync");
            // Read-only from the leaves up, then every directory synced, the folder itself last.
            std::vector<std::string> dirs;
            for (const Entry& e : m.entries) {
                if (e.kind == EntryKind::Dir) {
                    dirs.push_back(e.path);
                }
            }
            std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& c) { return a > c; });
            for (const std::string& d : dirs) {
                if (::fchmodat(out.get(), d.c_str(), 0555, 0) != 0) {
                    return drop(tmp, osError("chmod", tmpPath + "/" + d));
                }
                Fd df(::openat(out.get(), d.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (df.get() < 0) {
                    return drop(tmp, osError("open", tmpPath + "/" + d));
                }
                s = fullSync(df.get(), tmpPath + "/" + d);
                if (!s.is_ok()) {
                    return drop(tmp, s);
                }
            }
            if (::fchmod(out.get(), 0555) != 0) {
                return drop(tmp, osError("chmod", tmpPath));
            }
            s = fullSync(out.get(), tmpPath);
            if (!s.is_ok()) {
                return drop(tmp, s);
            }
        }

        at("verify");
        Manifest back;
        s = checkFolder(dir.get(), tmp, tmpPath, &back, leaveOutDrivers(), syncs());
        if (!s.is_ok()) {
            return drop(tmp, Status::failed("the copy did not read back as written, so nothing was placed: " +
                                            s.message()));
        }

        at("rename");
        const std::string state = m.tested ? "tested" : "untested";
        if (exists(dir.get(), m.tree)) {
            Manifest there;
            s = checkFolder(dir.get(), m.tree, path + "/" + m.tree, &there, leaveOutDrivers(), syncs());
            if (!s.is_ok()) {
                return drop(tmp, s);
            }
            if (m.tested && !there.tested) {
                return drop(tmp, Status::refused(
                    "build " + m.tree + " is already placed, untested; a build is never changed once placed, so the "
                    "evidence is not added to it: remove it (`nexus-server builds remove " + m.tree + "`) and place "
                    "it again with the evidence"));
            }
            r.out.push_back("already placed " + m.tree + " " + (there.tested ? "tested" : "untested"));
            return drop(tmp, Status::ok());
        }
        if (::renameat(dir.get(), tmp.c_str(), dir.get(), m.tree.c_str()) != 0) {
            return finish(osError("rename to " + m.tree, tmpPath));
        }
        at("syncdir");
        s = fullSync(dir.get(), path);
        if (!s.is_ok()) {
            return finish(s);
        }
        r.out.push_back("placed " + m.tree + " " + state);
        return finish(Status::ok());
    }

    // The stable link <server dir>/current -> builds/active/current, made once (by a new link renamed over it).
    Status stableLink() {
        const std::string want = std::string(kBuildsDir) + "/" + kActive + "/current";
        const std::string link = b.serverDir_ + "/" + kStable;
        struct stat st;
        if (::lstat(link.c_str(), &st) == 0) {
            if (!S_ISLNK(st.st_mode)) {
                return Status::refused(link + " is not a link; it is where the stable path to the current build "
                                       "goes: move it away and run this again");
            }
            std::string have;
            Status s = readLinkAt(AT_FDCWD, link, link, &have);
            if (!s.is_ok()) {
                return s;
            }
            if (have == want) {
                return Status::ok();
            }
        } else if (errno != ENOENT) {
            return osError("stat", link);
        }
        const std::string tmp = b.serverDir_ + "/.current-" + uniqueSuffix();
        if (::symlink(want.c_str(), tmp.c_str()) != 0) {
            return osError("symlink", tmp);
        }
        if (::rename(tmp.c_str(), link.c_str()) != 0) {
            const Status e = osError("rename to " + link, tmp);
            ::unlink(tmp.c_str());
            return e;
        }
        Fd top(::open(b.serverDir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (top.get() < 0) {
            return osError("open", b.serverDir_);
        }
        return fullSync(top.get(), b.serverDir_);
    }

    // current <hash> (back: an empty hash): the pointer moved to the pair (target, current) by one rename.
    int sw(const std::string& want, bool back) {
        Status s = begin();
        if (!s.is_ok()) {
            return finish(s);
        }
        at("check");
        Pointer p;
        s = pointer(&p);
        if (!s.is_ok()) {
            return finish(s);
        }
        std::string target = want;
        if (back) {
            if (!p.exists) {
                return finish(Status::refused("no build is current, so there is none to switch back to"));
            }
            if (p.previous.empty()) {
                return finish(Status::refused("build " + p.current + " is current and there is no previous build to "
                                              "switch back to"));
            }
            target = p.previous;
        }
        if (p.exists && target == p.current) {
            r.out.push_back("already current " + target);
            return finish(Status::ok());
        }
        if (!exists(dir.get(), target)) {
            return finish(Status::refused("build " + target + " is not placed (`nexus-server builds list` lists "
                                          "them)"));
        }
        Manifest m;
        s = checkFolder(dir.get(), target, path + "/" + target, &m, leaveOutDrivers(), syncs());
        if (!s.is_ok()) {
            return finish(s);
        }
        if (!m.tested) {
            return finish(Status::refused("build " + target + " is untested, and an untested build is never made "
                                          "current: place the tree again with the evidence that its exact content "
                                          "passed its tests (`place <tree> --evidence <file>`)"));
        }
        if (p.exists) {
            Manifest cur;
            std::string text;
            s = readFileAt(dir.get(), p.current + "/" + kManifest, path + "/" + p.current + "/" + kManifest, &text);
            std::string why;
            if (!s.is_ok() || !readManifest(text, &cur, &why, leaveOutDrivers())) {
                return finish(Status::failed("the current build " + p.current + " is damaged: " +
                                             (s.is_ok() ? why : s.message()) + "; `nexus-server builds verify`"));
            }
            const std::string refusal = formatsRefusal(m.formats, target, cur.formats, p.current);
            if (!refusal.empty()) {
                return finish(Status::refused(refusal));
            }
        }
        s = stableLink();
        if (!s.is_ok()) {
            return finish(s);
        }
        // two_renames (a reversed fault) makes it two pointer changes: (current, current), then (target, current).
        const bool twice = fault(Fault::TwoRenames) && p.exists;
        for (int phase = twice ? 1 : 2; phase <= 2; ++phase) {
            const std::string c = phase == 1 ? p.current : target;
            const std::string pv = p.exists ? p.current : std::string();
            s = movePointer(c, pv);
            if (!s.is_ok()) {
                return finish(s);
            }
        }
        r.out.push_back("current " + target);
        r.out.push_back("previous " + (p.exists ? p.current : std::string("none")));
        return finish(Status::ok());
    }

    // pmk, psync, prename, pdsync, anew, arename, async: the pair made and named, then the pointer renamed to it.
    Status movePointer(const std::string& c, const std::string& pv) {
        at("pmk");
        const std::string tmp = std::string(kPairTmpPrefix) + uniqueSuffix();
        const std::string tmpPath = path + "/" + tmp;
        if (::mkdirat(dir.get(), tmp.c_str(), 0700) != 0) {
            return osError("mkdir", tmpPath);
        }
        Fd pf(::openat(dir.get(), tmp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (pf.get() < 0) {
            return osError("open", tmpPath);
        }
        if (::symlinkat(("../" + c).c_str(), pf.get(), "current") != 0) {
            return osError("symlink", tmpPath + "/current");
        }
        if (!pv.empty() && ::symlinkat(("../" + pv).c_str(), pf.get(), "previous") != 0) {
            return osError("symlink", tmpPath + "/previous");
        }
        if (::fchmod(pf.get(), 0555) != 0) {
            return osError("chmod", tmpPath);
        }
        Status s;
        if (syncs()) {
            at("psync");
            s = fullSync(pf.get(), tmpPath);
            if (!s.is_ok()) {
                return s;
            }
        }
        at("prename");
        const std::string pair = pairName(c, pv);
        if (exists(dir.get(), pair)) {
            return Status::failed("a pair " + path + "/" + pair + " is there already, made while this run held the "
                                  "builds lock; another run is changing the builds without it");
        }
        if (::renameat(dir.get(), tmp.c_str(), dir.get(), pair.c_str()) != 0) {
            return osError("rename to " + pair, tmpPath);
        }
        if (syncs()) {
            at("pdsync");
            s = fullSync(dir.get(), path);
            if (!s.is_ok()) {
                return s;
            }
        }
        at("anew");
        const std::string link = std::string(kActiveTmpPrefix) + uniqueSuffix();
        if (::symlinkat(pair.c_str(), dir.get(), link.c_str()) != 0) {
            return osError("symlink", path + "/" + link);
        }
        at("arename");
        if (::renameat(dir.get(), link.c_str(), dir.get(), kActive) != 0) {
            return osError("rename to " + std::string(kActive), path + "/" + link);
        }
        if (syncs()) {
            at("async");
            s = fullSync(dir.get(), path);
            if (!s.is_ok()) {
                return s;
            }
        }
        return Status::ok();
    }

    // The builds and when each was placed (its manifest's modification time).
    Status placed(std::vector<Placed>* out) {
        out->clear();
        std::vector<std::string> names;
        Status s = listDir(dir.get(), path, &names);
        if (!s.is_ok()) {
            return s;
        }
        for (const std::string& n : names) {
            if (classify(n) != NameKind::Build) {
                continue;
            }
            struct stat st;
            Placed p;
            p.hash = n;
            if (::fstatat(dir.get(), (n + "/" + kManifest).c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
#if defined(__APPLE__)
                p.placedNs = std::int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
                p.placedNs = std::int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
            }
            out->push_back(p);
        }
        return Status::ok();
    }

    // ptry, pgone, pgsync, premove for one build: removed when no running server holds it. In use: false in *removed.
    Status removeOne(const std::string& hash, bool* removed) {
        *removed = false;
        at("ptry");
        Fd folder(::openat(dir.get(), hash.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (folder.get() < 0) {
            return osError("open", path + "/" + hash);
        }
        if (!fault(Fault::PruneInUse) && ::flock(folder.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                return Status::ok();
            }
            return osError("flock", path + "/" + hash);
        }
        at("pgone");
        const std::string gone = std::string(kGonePrefix) + uniqueSuffix();
        if (::renameat(dir.get(), hash.c_str(), dir.get(), gone.c_str()) != 0) {
            return osError("rename to " + gone, path + "/" + hash);
        }
        at("pgsync");
        Status s = fullSync(dir.get(), path);
        if (!s.is_ok()) {
            return s;
        }
        folder.reset();
        at("premove");
        s = removeTree(dir.get(), gone, path + "/" + gone);
        if (!s.is_ok()) {
            return s;
        }
        *removed = true;
        return Status::ok();
    }

    int prune(std::size_t keep, const std::string& only) {
        Status s = begin();
        if (!s.is_ok()) {
            return finish(s);
        }
        at("plist");
        Pointer p;
        s = pointer(&p);
        if (!s.is_ok()) {
            return finish(s);
        }
        std::vector<std::string> cands;
        if (!only.empty()) {
            if (only == p.current || only == p.previous) {
                return finish(Status::refused("build " + only + " is " + (only == p.current ? "current" : "previous") +
                                              ", and current and previous are never removed; make another build "
                                              "current first"));
            }
            if (!exists(dir.get(), only)) {
                return finish(Status::refused("build " + only + " is not placed"));
            }
            cands.push_back(only);
        } else {
            std::vector<Placed> all;
            s = placed(&all);
            if (!s.is_ok()) {
                return finish(s);
            }
            cands = pruneCandidates(all, p.current, p.previous, keep);
        }
        if (cands.empty()) {
            r.out.push_back("nothing to prune");
        }
        for (const std::string& h : cands) {
            bool removed = false;
            s = removeOne(h, &removed);
            if (!s.is_ok()) {
                return finish(s);
            }
            if (removed) {
                r.out.push_back("removed " + h);
            } else if (!only.empty()) {
                return finish(Status::refused("build " + h + " is in use: a running server was started from it; "
                                              "it is removed once that server has stopped"));
            } else {
                r.out.push_back("kept " + h + ": in use by a running server");
            }
        }
        return finish(Status::ok());
    }

    // The readers: the directory and its shared lock (nothing placed yet: says so).
    Status beginRead(bool* none) {
        *none = false;
        Status s = open(false);
        if (s.code() == Status::Code::NotFound) {
            *none = true;
            return Status::ok();
        }
        if (!s.is_ok()) {
            return s;
        }
        return lock(false);
    }

    int list() {
        bool none = false;
        Status s = beginRead(&none);
        if (!s.is_ok() || none) {
            if (none) {
                r.out.push_back("no builds placed in " + path);
            }
            return end(s);
        }
        Pointer p;
        s = pointer(&p);
        if (!s.is_ok()) {
            return end(s);
        }
        std::vector<Placed> all;
        s = placed(&all);
        if (!s.is_ok()) {
            return end(s);
        }
        std::sort(all.begin(), all.end(), [](const Placed& a, const Placed& c) {
            return a.placedNs != c.placedNs ? a.placedNs < c.placedNs : a.hash < c.hash;
        });
        for (const Placed& x : all) {
            std::string text;
            Manifest m;
            std::string why;
            const Status rs = readFileAt(dir.get(), x.hash + "/" + kManifest, path + "/" + x.hash, &text);
            const bool read = rs.is_ok() && readManifest(text, &m, &why, leaveOutDrivers());
            const std::string role = x.hash == p.current ? "current" : (x.hash == p.previous ? "previous" : "-");
            std::string use = "-";
            Fd folder(::openat(dir.get(), x.hash.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (folder.get() >= 0 && ::flock(folder.get(), LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
                use = "in-use";
            }
            if (!read) {
                r.out.push_back(x.hash + " " + role + " damaged " + use + " " + (rs.is_ok() ? why : rs.message()));
                continue;
            }
            r.out.push_back(x.hash + " " + role + " " + (m.tested ? "tested" : "untested") + " " + use + " " +
                            formatsLine(m.formats));
        }
        std::vector<std::string> left;
        s = leftovers(&left);
        for (const std::string& n : left) {
            r.out.push_back("leftover " + n + " (removed by the next place, current, back, prune or remove)");
        }
        return end(s);
    }

    int verify(const std::string& only) {
        bool none = false;
        Status s = beginRead(&none);
        if (!s.is_ok() || none) {
            if (none) {
                r.out.push_back("no builds placed in " + path);
            }
            return end(none && !only.empty() ? Status::refused("build " + only + " is not placed") : s);
        }
        std::vector<Placed> all;
        s = placed(&all);
        if (!s.is_ok()) {
            return end(s);
        }
        bool damaged = false;
        bool found = false;
        for (const Placed& x : all) {
            if (!only.empty() && x.hash != only) {
                continue;
            }
            found = true;
            Manifest m;
            const Status c = checkFolder(dir.get(), x.hash, path + "/" + x.hash, &m, leaveOutDrivers(), syncs());
            if (c.is_ok()) {
                r.out.push_back("ok " + x.hash + " " + (m.tested ? "tested" : "untested"));
            } else {
                damaged = true;
                r.out.push_back("damaged " + x.hash + ": " + c.message());
            }
        }
        if (!only.empty() && !found) {
            return end(Status::refused("build " + only + " is not placed"));
        }
        Pointer p;
        s = pointer(&p);
        if (!s.is_ok()) {
            damaged = true;
            r.out.push_back("damaged pointer: " + s.message());
        } else if (p.exists) {
            std::string why;
            std::string cur;
            std::string prev;
            const std::string pp = path + "/" + p.pair;
            if (!readLinkAt(dir.get(), p.pair + "/current", pp + "/current", &cur).is_ok() ||
                cur != "../" + p.current) {
                why = "its current link is not ../" + p.current;
            } else if (!p.previous.empty() &&
                       (!readLinkAt(dir.get(), p.pair + "/previous", pp + "/previous", &prev).is_ok() ||
                        prev != "../" + p.previous)) {
                why = "its previous link is not ../" + p.previous;
            } else if (!exists(dir.get(), p.current) || (!p.previous.empty() && !exists(dir.get(), p.previous))) {
                why = "a build it names is not placed";
            }
            if (why.empty()) {
                r.out.push_back("ok pointer " + p.pair);
            } else {
                damaged = true;
                r.out.push_back("damaged pointer " + p.pair + ": " + why);
            }
        } else {
            r.out.push_back("no build is current");
        }
        if (damaged) {
            return end(Status::failed("damaged: see the lines above; a damaged build is removed and placed again"));
        }
        return end(Status::ok());
    }
};

Builds::Builds(std::string serverDir, Options options) : serverDir_(std::move(serverDir)), options_(std::move(options)) {}
Builds::~Builds() = default;

int Builds::place(const std::string& tree, const std::string& evidence, Result& r) {
    Run run(*this, r);
    return run.place(tree, evidence);
}

int Builds::makeCurrent(const std::string& hash, Result& r) {
    Run run(*this, r);
    if (!isHash(hash)) {
        return run.end(Status::refused("`" + hash + "` is not a build's hash (64 hex digits; `builds list`)"));
    }
    return run.sw(hash, false);
}

int Builds::back(Result& r) {
    Run run(*this, r);
    return run.sw(std::string(), true);
}

int Builds::prune(std::size_t keep, Result& r) {
    Run run(*this, r);
    return run.prune(keep, std::string());
}

int Builds::remove(const std::string& hash, Result& r) {
    Run run(*this, r);
    if (!isHash(hash)) {
        return run.end(Status::refused("`" + hash + "` is not a build's hash (64 hex digits; `builds list`)"));
    }
    return run.prune(0, hash);
}

int Builds::list(Result& r) {
    Run run(*this, r);
    return run.list();
}

int Builds::verify(const std::string& hash, Result& r) {
    Run run(*this, r);
    if (!hash.empty() && !isHash(hash)) {
        return run.end(Status::refused("`" + hash + "` is not a build's hash (64 hex digits; `builds list`)"));
    }
    return run.verify(hash);
}

Status hashTree(const std::string& dir, std::string* hash, Fault fault) {
    Fd src(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (src.get() < 0) {
        return osError("open", dir);
    }
    std::vector<std::string> parts;
    Status s = treeParts(src.get(), dir, &parts, false);
    if (!s.is_ok()) {
        return s;
    }
    Walk w;
    for (const std::string& p : parts) {
        s = walkPart(src.get(), -1, p, dir, dir, w);
        if (!s.is_ok()) {
            return s;
        }
    }
    *hash = treeHash(w.entries, fault == Fault::TestedProgramOnly);
    return Status::ok();
}

Status checkBuild(const std::string& buildsDir, const std::string& hash, Manifest* manifest, Fault fault) {
    Fd d(::open(buildsDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (d.get() < 0) {
        return osError("open", buildsDir);
    }
    if (!exists(d.get(), hash)) {
        return Status::not_found("build " + hash + " is not placed in " + buildsDir);
    }
    return checkFolder(d.get(), hash, buildsDir + "/" + hash, manifest, fault == Fault::TestedProgramOnly,
                       fault != Fault::NoSync);
}

std::string thisBuildFormats() {
    Formats f{
        {"store-record", store::kRecordFormat, store::kRecordReader},
        {"store-snapshot", store::kSnapshotFormat, store::kSnapshotReader},
        {"config", std::uint32_t(accounts::kVersion), std::uint32_t(accounts::kVersion)},
        {"link", sync::kStateFormat, sync::kMinPeer},
        {"driver-protocol", std::uint32_t(driver::kProtocolVersion), std::uint32_t(driver::kProtocolVersion)},
    };
    return formatsText(f);
}

namespace {

const std::map<std::string, std::string>& helps() {
    static const std::map<std::string, std::string> h{
        {"place",
         "nexus-server builds place <tree> [--evidence <file>] [--dir DIR]\n"
         "  Places the tree as a build under DIR/builds, named by the hash of everything in it.\n"
         "  <tree> holds bin/nexus-server (executable), formats (`builds formats` writes it), and drivers/ and\n"
         "  nexus.app/ where they are placed; nothing else. <file> is the evidence its tests passed:\n"
         "    nexus-test-evidence 1 / tree <hash from `builds hash <tree>`> / result pass / run <one line>\n"
         "  Steps: the builds lock taken (waits up to 60 s); the directory synced and what interrupted runs left\n"
         "  removed; a temporary folder made; each part copied, every file hashed, synced and closed with each\n"
         "  call checked; the manifest written (the evidence must name this tree's hash, the driver tree within\n"
         "  it); everything made read-only and synced; every file read back against the manifest; one rename to\n"
         "  the hash; the directory synced; the lock released. Placing never changes a build already placed and\n"
         "  never changes which build is current.\n"
         "  Prints: placed <hash> tested|untested, or already placed <hash> tested|untested.\n"
         "  Exit: 0 placed or already placed; 1 a filesystem call failed (said, nothing is named), or locks cannot\n"
         "  be trusted where DIR is; 2 usage; 3 refused (no program, no formats, a name a build cannot hold,\n"
         "  evidence for other bytes or not a pass, already placed untested with evidence now); 4 lock busy.\n"},
        {"hash",
         "nexus-server builds hash <tree>\n"
         "  Prints the hash the tree would be placed under: what its tests' evidence names. Reads only.\n"
         "  Exit: 0; 1 the tree cannot be read; 2 usage; 3 it holds something a build cannot.\n"},
        {"formats",
         "nexus-server builds formats\n"
         "  Prints this program's formats file (format <name> <writes> <reader> lines), which a tree's assembly\n"
         "  writes to <tree>/formats. Exit: 0.\n"},
        {"list",
         "nexus-server builds list [--dir DIR]\n"
         "  One line per build, oldest placed first:\n"
         "    <hash> current|previous|- tested|untested|damaged in-use|- formats:<name>=<writes>/<reader>,...\n"
         "  and `leftover <name>` for each thing an interrupted run left. Takes the builds lock shared.\n"
         "  Exit: 0; 1 the directory cannot be read; 4 lock busy.\n"},
        {"verify",
         "nexus-server builds verify [<hash>] [--dir DIR]\n"
         "  Reads every build (or the one) back against its manifest: every file's bytes, kinds, modes, and\n"
         "  nothing more; and the pointer and its pair. Prints ok <hash> tested|untested, damaged <hash>: <why>,\n"
         "  ok pointer <pair> or damaged pointer ...\n"
         "  Exit: 0 all whole; 1 any damaged; 2 usage; 3 that build is not placed; 4 lock busy.\n"},
        {"current",
         "nexus-server builds current <hash> [--dir DIR]\n"
         "  Makes the build current and the current build previous, in one rename of the pointer.\n"
         "  Refused (nothing changes) unless the build is placed, reads back whole against its manifest, is\n"
         "  tested, and reads what the current build writes: for each of store-record, store-snapshot and config,\n"
         "  the current build's reader is at most this build's writes. Whether the store holds anything this build\n"
         "  cannot read is not checked here.\n"
         "  Steps: lock, settle, clean as place; the checks; DIR/current made a link to builds/active/current if it\n"
         "  is not; a pair folder made (links current and previous), synced, renamed to pair-<new>-<old>, the\n"
         "  directory synced; a new pointer link made and renamed over builds/active; the directory synced; the\n"
         "  lock released. A crash leaves the old pair or the new one in force, never a mix.\n"
         "  Prints: current <hash>, previous <hash|none>; or already current <hash>.\n"
         "  Exit: 0; 1 a filesystem call failed or the build is damaged; 2 usage; 3 refused; 4 lock busy.\n"},
        {"back",
         "nexus-server builds back [--dir DIR]\n"
         "  Makes previous current and current previous, by the steps and checks of `current`.\n"
         "  Exit: as current; 3 also when there is no previous build.\n"},
        {"prune",
         "nexus-server builds prune [--keep <n>] [--dir DIR]\n"
         "  Removes every build but current, previous and the n (default 2) most recently placed of the rest.\n"
         "  A build a running server was started from is kept (the server holds its folder's lock).\n"
         "  Steps: lock, settle, clean; for each build to remove, oldest first: its folder's lock tried\n"
         "  (refused: in use, kept), the folder renamed aside, the directory synced, the folder removed.\n"
         "  Prints: removed <hash> / kept <hash>: in use by a running server / nothing to prune.\n"
         "  Exit: 0; 1 a filesystem call failed; 2 usage; 4 lock busy.\n"},
        {"remove",
         "nexus-server builds remove <hash> [--dir DIR]\n"
         "  Removes the one build, by the steps of prune. Refused (exit 3) for current, previous, a build not\n"
         "  placed, and a build in use. Exit: as prune.\n"},
    };
    return h;
}

const char* const kVerbs[] = {"place", "hash", "formats", "list", "verify", "current", "back", "prune", "remove"};

} // namespace

std::string help(const std::string& verb) {
    if (verb.empty() || verb == "help") {
        std::string out = "nexus-server builds <verb>: the installed builds of nexus-server, under DIR/builds "
                          "(docs/builds.md)\n";
        for (const char* v : kVerbs) {
            out += helps().at(v);
        }
        out += "Every failure is said in one line on standard error, with a nonzero exit.\n";
        return out;
    }
    const auto it = helps().find(verb);
    return it == helps().end() ? std::string() : it->second;
}

int command(const std::string& serverDir, const std::vector<std::string>& args, const Options& options, Result& r) {
    const std::string verb = args.empty() ? std::string() : args[0];
    auto trimmed = [](std::string text) {
        if (!text.empty() && text.back() == '\n') {
            text.pop_back();
        }
        return text;
    };
    auto usage = [&]() {
        const std::string h = help(verb);
        r.err.push_back("usage: " + trimmed(h.empty() ? help(std::string()) : h));
        r.exit = kExitUsage;
        return r.exit;
    };
    auto helpOf = [&](const std::string& v) {
        const std::string h = help(v);
        if (h.empty()) {
            r.err.push_back("no verb `" + v + "`; " + trimmed(help(std::string())));
            r.exit = kExitUsage;
            return r.exit;
        }
        r.out.push_back(trimmed(h));
        r.exit = kExitOk;
        return r.exit;
    };
    if (verb.empty() || verb == "help" || verb == "--help" || verb == "-h") {
        return helpOf(args.size() > 1 ? args[1] : std::string());
    }
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h")) {
        return helpOf(verb);
    }
    Builds b(serverDir, options);
    if (verb == "place") {
        std::string tree;
        std::string evidence;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--evidence" && i + 1 < args.size()) {
                evidence = args[++i];
            } else if (tree.empty() && !args[i].empty() && args[i][0] != '-') {
                tree = args[i];
            } else {
                return usage();
            }
        }
        if (tree.empty()) {
            return usage();
        }
        return b.place(tree, evidence, r);
    }
    if (verb == "hash" && args.size() == 2) {
        std::string hash;
        const Status s = hashTree(args[1], &hash, options.fault);
        if (!s.is_ok()) {
            r.err.push_back(s.message());
            r.exit = exitOf(s);
            return r.exit;
        }
        r.out.push_back(hash);
        r.exit = kExitOk;
        return r.exit;
    }
    if (verb == "formats" && args.size() == 1) {
        std::string text = thisBuildFormats();
        if (!text.empty() && text.back() == '\n') {
            text.pop_back();
        }
        r.out.push_back(text);
        r.exit = kExitOk;
        return r.exit;
    }
    if (verb == "list" && args.size() == 1) {
        return b.list(r);
    }
    if (verb == "verify" && args.size() <= 2) {
        return b.verify(args.size() == 2 ? args[1] : std::string(), r);
    }
    if (verb == "current" && args.size() == 2) {
        return b.makeCurrent(args[1], r);
    }
    if (verb == "back" && args.size() == 1) {
        return b.back(r);
    }
    if (verb == "prune" && (args.size() == 1 || (args.size() == 3 && args[1] == "--keep"))) {
        std::size_t keep = kDefaultKeep;
        if (args.size() == 3) {
            const std::string& n = args[2];
            if (n.empty() || n.size() > 6 || n.find_first_not_of("0123456789") != std::string::npos) {
                return usage();
            }
            keep = std::size_t(std::stoul(n));
        }
        return b.prune(keep, r);
    }
    if (verb == "remove" && args.size() == 2) {
        return b.remove(args[1], r);
    }
    return usage();
}

BuildHold::~BuildHold() {
    release();
}

BuildHold::BuildHold(BuildHold&& other) noexcept : fd_(other.fd_), hash_(std::move(other.hash_)) {
    other.fd_ = -1;
    other.hash_.clear();
}

BuildHold& BuildHold::operator=(BuildHold&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        hash_ = std::move(other.hash_);
        other.fd_ = -1;
        other.hash_.clear();
    }
    return *this;
}

void BuildHold::release() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    hash_.clear();
}

Status holdBuild(const std::string& program, BuildHold& out, const std::function<std::uint64_t()>& now_ms,
                 const std::function<void(std::uint64_t)>& sleep_ms, std::uint64_t wait_ms) {
    out.release();
    const std::string hash = buildOfProgram(program);
    if (hash.empty()) {
        return Status::ok();
    }
    const std::string folder = program.substr(0, program.size() - std::strlen(kProgram) - 1);
    Fd fd(::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0) {
        return osError("open", folder);
    }
    const std::uint64_t start = now_ms ? now_ms() : steadyMs();
    for (;;) {
        if (::flock(fd.get(), LOCK_SH | LOCK_NB) == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EWOULDBLOCK) {
            return osError("flock", folder);
        }
        if ((now_ms ? now_ms() : steadyMs()) - start >= wait_ms) {
            return Status::failed("build " + hash + " is being removed (a prune holds it), so this server does not "
                                  "start from it; start it from the current build (`nexus-server builds list`)");
        }
        if (sleep_ms) {
            sleep_ms(kLockRetryMs);
        } else {
            realSleep(kLockRetryMs);
        }
    }
    // Still the folder under its hash name: one renamed aside to be removed is not held.
    struct stat held;
    struct stat named;
    if (::fstat(fd.get(), &held) != 0) {
        return osError("fstat", folder);
    }
    if (::stat(folder.c_str(), &named) != 0 || named.st_dev != held.st_dev || named.st_ino != held.st_ino) {
        return Status::failed("build " + hash + " was removed as this server started, so it does not start from it; "
                              "start it from the current build (`nexus-server builds list`)");
    }
    // A second descriptor of the same open file, so the lock goes with it (closed on exec: no driver holds it).
    out.fd_ = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 0);
    if (out.fd_ < 0) {
        return osError("dup", folder);
    }
    out.hash_ = hash;
    return Status::ok();
}

} // namespace nexus::server::builds
