// The real filesystem behind the store's seam (fs.h).

#include "store/fs.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/statfs.h>
#endif

namespace nexus::store {

namespace {

Status os_error(const char* what, const std::string& path) {
    int e = errno;
    std::string msg = std::string(what) + " " + path + ": " + std::strerror(e);
    if (e == ENOENT) {
        return Status::not_found(msg);
    }
    if (e == ENOSPC || e == EDQUOT) {
        return Status::failed(msg + " (free space on that disk, then restart the server)");
    }
    if (e == EACCES || e == EPERM || e == EROFS) {
        return Status::failed(msg + " (the user running nexus-server must be able to read there, and the server to "
                              "write there too; `nexus-server store check` only reads)");
    }
    return Status::failed(msg + " (check the disk; the server reopens the store from what is durable when restarted)");
}

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

Status full_sync(int fd, const std::string& path) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return Status::ok();
    }
    // Some filesystems (network, FUSE) have no F_FULLFSYNC; fsync is the most they offer.
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) {
        return os_error("F_FULLFSYNC", path);
    }
#endif
    if (::fsync(fd) != 0) {
        return os_error("fsync", path);
    }
    return Status::ok();
}

Status write_all(int fd, std::span<const std::uint8_t> data, const std::string& path) {
    std::size_t done = 0;
    while (done < data.size()) {
        ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0) {
            return os_error("write", path);
        }
        done += std::size_t(w);
    }
    return Status::ok();
}

// The log, open for the journal's life: one open, then a write and a flush per batch on the same descriptor.
class PosixAppendFile : public AppendFile {
public:
    PosixAppendFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    Status append(std::span<const std::uint8_t> data) override { return write_all(fd_.get(), data, path_); }
    Status sync() override { return full_sync(fd_.get(), path_); }

private:
    Fd fd_;
    std::string path_;
};

// The directories a PosixFs holds, by the name each was held by: the descriptor every later call on a name in it goes
// through. Shared with each hold, which takes its entry out when it goes, so that a hold outliving its Fs is safe.
using Roots = std::map<std::string, int>;

// A directory held: its descriptor, opened once, with flock on it. Closing it releases the lock; so does the end of the
// process. It is opened close-on-exec, so no driver or other child the server starts ever holds it.
class PosixLock : public FileLock {
public:
    PosixLock(int fd, std::shared_ptr<Roots> roots, std::string dir)
        : fd_(fd), roots_(std::move(roots)), dir_(std::move(dir)) {
        roots_->emplace(dir_, fd_.get());   // a second hold of the same name in one Fs keeps the first's
    }
    ~PosixLock() override {
        auto it = roots_->find(dir_);
        if (it != roots_->end() && it->second == fd_.get()) {
            roots_->erase(it);
        }
    }
    PosixLock(const PosixLock&) = delete;
    PosixLock& operator=(const PosixLock&) = delete;

    // The held directory still has a name: a directory removed while held (rm -r of the server's directory) has no
    // link left, and what is written in it can never be found again.
    Status alive() const override {
        struct stat st;
        if (::fstat(fd_.get(), &st) != 0) {
            return os_error("fstat", dir_);
        }
        bool gone = st.st_nlink == 0;      // Linux: a removed directory has no link left
#if defined(__APPLE__)
        // macOS keeps a removed directory's link count, so the directory's present name (F_GETPATH, which follows
        // renames) is asked instead: removed, it names nothing, or another directory made there since.
        char path[MAXPATHLEN] = {};
        struct stat now;
        if (!gone && ::fcntl(fd_.get(), F_GETPATH, path) == 0) {
            gone = ::stat(path, &now) != 0 || now.st_dev != st.st_dev || now.st_ino != st.st_ino;
        }
#endif
        if (gone) {
            return Status::failed("store " + dir_ + ": its directory was removed while this server held it, so nothing "
                                  "written now could be found again; the store has failed and the server stops (put "
                                  "the directory back from a backup, or start the server to make a new store)");
        }
        return Status::ok();
    }

private:
    Fd fd_;
    std::shared_ptr<Roots> roots_;
    std::string dir_;
};

// Where advisory locks cannot be trusted, by the Linux name of the filesystem (statfs's f_type, or the test seam's
// name): a network filesystem (another machine's lock is not seen, or the lock is emulated), or one that passes locks
// to another program. What each is, for the refusal; empty when it is none of these. virtiofs reports FUSE's type.
std::string unlockable_name(const std::string& type) {
    static const std::pair<const char*, const char*> kNames[] = {
        {"nfs", "NFS, a network filesystem"},
        {"smb", "SMB, a network filesystem"},
        {"cifs", "CIFS (SMB), a network filesystem"},
        {"smb2", "SMB2, a network filesystem"},
        {"fuse", "FUSE, a filesystem run by another program (virtiofs, sshfs and the like are FUSE)"},
        {"9p", "9p, a filesystem served by another program (WSL2's view of Windows drives is 9p)"},
        {"ncp", "NCP, a network filesystem"},
        {"afs", "AFS, a network filesystem"},
        {"kafs", "kAFS, a network filesystem"},
        {"ceph", "Ceph, a network filesystem"},
        {"gpfs", "GPFS, a cluster filesystem"},
        {"lustre", "Lustre, a cluster filesystem"},
        {"beegfs", "BeeGFS, a cluster filesystem"},
        {"coda", "Coda, a network filesystem"},
        {"vboxsf", "vboxsf, a VirtualBox shared folder"},
    };
    for (const auto& [name, what] : kNames) {
        if (type == name) {
            return what;
        }
    }
    return std::string();
}

// The filesystem a held directory is on, when it is one where locks cannot be trusted: what it is; else empty.
std::string unlockable_by_type(int dirfd) {
    struct statfs fs;
    if (::fstatfs(dirfd, &fs) != 0) {
        return std::string();   // the lock below says why, if anything is wrong
    }
#if defined(__APPLE__)
    const std::string type = fs.f_fstypename;
    if ((fs.f_flags & MNT_LOCAL) == 0) {
        return type + ", not a local disk";
    }
    if (type.rfind("macfuse", 0) == 0 || type.rfind("osxfuse", 0) == 0 || type == "fuse") {
        return unlockable_name("fuse");
    }
    return unlockable_name(type);
#else
    switch (static_cast<unsigned long>(fs.f_type)) {
    case 0x6969UL:
        return unlockable_name("nfs");
    case 0x517BUL:
        return unlockable_name("smb");
    case 0xFF534D42UL:
        return unlockable_name("cifs");
    case 0xFE534D42UL:
        return unlockable_name("smb2");
    case 0x65735546UL:
        return unlockable_name("fuse");
    case 0x01021997UL:
        return unlockable_name("9p");
    case 0x564CUL:
        return unlockable_name("ncp");
    case 0x5346414FUL:
        return unlockable_name("afs");
    case 0x6B414653UL:
        return unlockable_name("kafs");
    case 0x00C36400UL:
        return unlockable_name("ceph");
    case 0x47504653UL:
        return unlockable_name("gpfs");
    case 0x0BD00BD0UL:
        return unlockable_name("lustre");
    case 0x19830326UL:
        return unlockable_name("beegfs");
    case 0x73757245UL:
        return unlockable_name("coda");
    case 0x786F4256UL:
        return unlockable_name("vboxsf");
    default:
        return std::string();
    }
#endif
}

Status flock_failed(const std::string& what) {
    const int e = errno;
    return Status::failed("flock " + what + ": " + std::strerror(e) + " (this filesystem takes no locks; keep the "
                          "server's directory on a local disk)");
}

// An exclusive lock's wait on readers run out: the readers that held the store when the wait began (with the gate held
// no reader starts after), each of which gives up its copy within kReaderCopyLimitMs, did not let go in time: one
// stopped or stuck.
Status readers_held(const std::string& dir, std::uint64_t wait_ms) {
    return Status::failed(dir + " is held by readers only (`nexus-server store check`, or another tool reading the "
                          "store), and no new reader starts while a server waits, but those that held it did not let "
                          "go within " + std::to_string(wait_ms) + " ms (each gives up its copy within " +
                          std::to_string(kReaderCopyLimitMs) + " ms, so one is stopped or stuck); find it (lsof " +
                          dir + ") and end it, then start the server again");
}

class PosixFs : public Fs {
public:
    explicit PosixFs(PosixOptions options) : options_(std::move(options)), roots_(std::make_shared<Roots>()) {
        if (!options_.now_ms) {
            options_.now_ms = [] {
                return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            };
        }
        if (!options_.sleep_ms) {
            options_.sleep_ms = [](std::uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        }
    }

    Status lock(const std::string& dir, LockMode mode, std::unique_ptr<FileLock>& out) override {
        out.reset();
        // The directory itself, by its name this once (a symbolic link to it followed, so that every name it is reached
        // by takes the one lock); from now on only by this descriptor.
        Fd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (fd.get() < 0) {
            return os_error("open", dir);
        }
        const std::string type = unlockable_filesystem(fd.get(), options_.test_fs_type);
        if (!type.empty()) {
            return Status::failed(dir + " is on " + type + ", where a lock is not trusted to keep a second "
                                  "nexus-server out; keep the server's directory on a local disk (--dir, or HOME)");
        }
        const bool exclusive = mode == LockMode::Exclusive;
        const std::uint64_t start = options_.now_ms();
        // The gate first (kGateName, fs.h), through the directory's descriptor: a writer holds it exclusive while it
        // waits for the store's lock, made if it is not there; a reader takes it shared, without waiting, only for the
        // moment of taking the store's lock, and makes nothing (no gate: no writer of this build has waited here)
        // (models/store/StoreLock.tla LockTry, RLock: NoNewReaderWhileWriterWaits).
        // A writer makes it only by an exclusive create, and opens it when that finds it made: several writers making
        // it at once on macOS, each with a plain O_CREAT, had one refused with ENOENT (server_lock's `new` race).
        int opened = -1;
        if (!options_.test_no_gate) {
            opened = ::openat(fd.get(), kGateName, (exclusive ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
            for (int tries = 0; exclusive && opened < 0 && errno == ENOENT && tries < 100; ++tries) {
                opened = ::openat(fd.get(), kGateName, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (opened < 0 && errno == EEXIST) {
                    opened = ::openat(fd.get(), kGateName, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
                }
            }
            if (opened < 0 && (exclusive || errno != ENOENT)) {
                return os_error("open", dir + "/" + kGateName);
            }
        }
        Fd gate(opened);
        if (gate.get() >= 0 && !exclusive) {
            while (::flock(gate.get(), LOCK_SH | LOCK_NB) != 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EWOULDBLOCK) {
                    return Status::refused(dir + " has a nexus-server waiting to open it, and a reader gives way to it");
                }
                return flock_failed(dir + "/" + kGateName);
            }
        }
        if (exclusive && gate.get() >= 0) {
            for (;;) {
                if (::flock(gate.get(), LOCK_EX | LOCK_NB) == 0) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno != EWOULDBLOCK) {
                    return flock_failed(dir + "/" + kGateName);
                }
                // Refused: a shared gate to be had means readers hold it, each for the moment of taking the store's
                // lock (tried again below); none means another writer holds it, waiting for the store's lock: Busy.
                if (::flock(gate.get(), LOCK_SH | LOCK_NB) != 0) {
                    return Status::busy(dir + " is being opened by another writer, which holds its gate");
                }
                ::flock(gate.get(), LOCK_UN);
                if (options_.now_ms() - start >= options_.reader_wait_ms) {
                    return readers_held(dir, options_.reader_wait_ms);
                }
                options_.sleep_ms(kLockRetryMs);
            }
        }
        const int want = exclusive ? LOCK_EX : LOCK_SH;
        for (;;) {
            if (::flock(fd.get(), want | LOCK_NB) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EWOULDBLOCK) {
                return flock_failed(dir);
            }
            // Refused: a shared lock to be had now means only readers hold it (next_lock_step); with the gate held, no
            // reader starts now, so these are the readers that had it when this wait began.
            bool shared_free = false;
            if (exclusive && ::flock(fd.get(), LOCK_SH | LOCK_NB) == 0) {
                shared_free = true;
                ::flock(fd.get(), LOCK_UN);
            }
            const LockStep step = next_lock_step(false, shared_free, options_.now_ms() - start,
                                                 options_.reader_wait_ms);
            if (step == LockStep::Busy) {
                return Status::busy(dir + " is locked by a writer");
            }
            if (step == LockStep::Readers) {
                return readers_held(dir, options_.reader_wait_ms);
            }
            options_.sleep_ms(kLockRetryMs);
        }
        // The lock checked where it is: a second exclusive lock on another descriptor of the same directory must be
        // refused now. Granted, this filesystem does not keep locks, whatever its type said.
        Status kept = locks_kept(fd.get(), dir, "a second nexus-server", options_.test_locks_ignored);
        if (!kept.is_ok()) {
            return kept;
        }
        // The gate goes as `gate` closes: readers may start again, and are refused by the store's lock while it is held.
        out = std::make_unique<PosixLock>(fd.release(), roots_, dir);
        return Status::ok();
    }

    Status open_append(const std::string& path, std::unique_ptr<AppendFile>& out) override {
        out.reset();
        const At at = where(path);
        int fd = ::openat(at.fd, at.name.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) {
            return os_error("open", path);
        }
        out = std::make_unique<PosixAppendFile>(fd, path);
        return Status::ok();
    }

    Status make_dir(const std::string& dir) override {
        if (held(dir) >= 0) {
            return Status::ok();    // held by its descriptor: there, whatever its name now names
        }
        std::size_t start = 0;
        while (true) {
            std::size_t slash = dir.find('/', start);
            std::string at = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
            if (!at.empty()) {
                if (::mkdir(at.c_str(), 0700) == 0) {
                    Status s = sync_dir(dir_of(at));
                    if (!s.is_ok()) {
                        return s;
                    }
                } else if (errno != EEXIST) {
                    return os_error("mkdir", at);
                }
            }
            if (slash == std::string::npos) {
                return Status::ok();
            }
            start = slash + 1;
        }
    }

    Status list(const std::string& dir, std::vector<std::string>& names) override {
        names.clear();
        const int root = held(dir);
        DIR* d = nullptr;
        if (root >= 0) {
            const int fd = ::openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            d = fd < 0 ? nullptr : ::fdopendir(fd);
            if (fd >= 0 && d == nullptr) {
                ::close(fd);
            }
        } else {
            d = ::opendir(dir.c_str());
        }
        if (d == nullptr) {
            return os_error("opendir", dir);
        }
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") {
                names.push_back(n);
            }
        }
        ::closedir(d);
        return Status::ok();
    }

    Status size(const std::string& path, std::uint64_t& out) override {
        const At at = where(path);
        struct stat st;
        if (::fstatat(at.fd, at.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            return os_error("stat", path);
        }
        if (S_ISLNK(st.st_mode)) {
            return Status::failed(path + " is a symbolic link; it must be a regular file (remove the link)");
        }
        if (!S_ISREG(st.st_mode)) {
            return Status::failed(path + " is not a regular file");
        }
        out = std::uint64_t(st.st_size);
        return Status::ok();
    }

    Status read(const std::string& path, std::uint64_t off, std::uint64_t n, Bytes& out) override {
        out.clear();
        const At at = where(path);
        Fd fd(::openat(at.fd, at.name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (fd.get() < 0) {
            if (errno == ELOOP
#ifdef EMLINK
                || errno == EMLINK
#endif
#ifdef EFTYPE
                || errno == EFTYPE
#endif
            ) {
                return Status::failed(path + " is a symbolic link; it must be a regular file (remove the link)");
            }
            return os_error("open", path);
        }
        struct stat st;
        if (::fstat(fd.get(), &st) != 0) {
            return os_error("fstat", path);
        }
        if (!S_ISREG(st.st_mode)) {
            return Status::failed(path + " is not a regular file");
        }
        std::uint64_t total = std::uint64_t(st.st_size);
        if (off >= total) {
            return Status::ok();
        }
        out.resize(std::size_t(std::min(n, total - off)));
        std::size_t got = 0;
        while (got < out.size()) {
            ssize_t r = ::pread(fd.get(), out.data() + got, out.size() - got, off_t(off + got));
            if (r < 0 && errno == EINTR) {
                continue;
            }
            if (r < 0) {
                return os_error("read", path);
            }
            if (r == 0) {
                break;
            }
            got += std::size_t(r);
        }
        out.resize(got);
        return Status::ok();
    }

    Status create(const std::string& path) override {
        const At at = where(path);
        Fd fd(::openat(at.fd, at.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (fd.get() < 0) {
            return os_error("create", path);
        }
        return Status::ok();
    }

    Status append(const std::string& path, std::span<const std::uint8_t> data) override {
        const At at = where(path);
        Fd fd(::openat(at.fd, at.name.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
        if (fd.get() < 0) {
            return os_error("open", path);
        }
        return write_all(fd.get(), data, path);
    }

    Status truncate(const std::string& path, std::uint64_t size) override {
        const At at = where(path);
        Fd fd(::openat(at.fd, at.name.c_str(), O_WRONLY | O_CLOEXEC));
        if (fd.get() < 0) {
            return os_error("open", path);
        }
        if (::ftruncate(fd.get(), off_t(size)) != 0) {
            return os_error("truncate", path);
        }
        return Status::ok();
    }

    Status sync(const std::string& path) override {
        const At at = where(path);
        Fd fd(::openat(at.fd, at.name.c_str(), O_RDWR | O_CLOEXEC));
        if (fd.get() < 0) {
            return os_error("open", path);
        }
        return full_sync(fd.get(), path);
    }

    Status sync_dir(const std::string& dir) override {
        const int root = held(dir);
        if (root >= 0) {
            return full_sync(root, dir);
        }
        Fd fd(::open(dir.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.get() < 0) {
            return os_error("open", dir);
        }
        return full_sync(fd.get(), dir);
    }

    Status rename(const std::string& from, const std::string& to) override {
        const At a = where(from);
        const At b = where(to);
        if (::renameat(a.fd, a.name.c_str(), b.fd, b.name.c_str()) != 0) {
            return os_error("rename", from + " to " + to);
        }
        return Status::ok();
    }

    Status remove(const std::string& path) override {
        const At at = where(path);
        if (::unlinkat(at.fd, at.name.c_str(), 0) != 0) {
            return os_error("unlink", path);
        }
        return Status::ok();
    }

    Status remove_dir(const std::string& dir) override {
        const At at = where(dir);
        if (::unlinkat(at.fd, at.name.c_str(), AT_REMOVEDIR) != 0) {
            return os_error("rmdir", dir);
        }
        return Status::ok();
    }

    Status write_new(const std::string& path, std::span<const std::uint8_t> data) override {
        const At at = where(path);
        Fd fd(::openat(at.fd, at.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (fd.get() < 0) {
            return os_error("create", path);
        }
        Status s = write_all(fd.get(), data, path);
        if (s.is_ok()) {
            s = full_sync(fd.get(), path);
        }
        return s;
    }

    Status private_dir(const std::string& dir) override {
        Status s = make_dir(dir);
        if (!s.is_ok()) {
            return s;
        }
        struct stat st;
        if (::lstat(dir.c_str(), &st) != 0) {
            return os_error("lstat", dir);
        }
        if (S_ISLNK(st.st_mode)) {
            return Status::failed(dir + " is a symbolic link; it must be a directory of its own, mode 0700 (remove the "
                                  "link)");
        }
        if (!S_ISDIR(st.st_mode)) {
            return Status::failed(dir + " is not a directory");
        }
        if (st.st_uid != ::geteuid()) {
            return Status::failed(dir + " belongs to another user; it must be the server's user's, mode 0700");
        }
        if ((st.st_mode & 077) != 0) {
            char mode[8];
            std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
            return Status::failed(dir + " is open to other users (mode " + mode + "); make it 0700 (chmod 700 " + dir +
                                  ")");
        }
        return Status::ok();
    }

private:
    // A name to open: in a held directory, its descriptor and the name in it; elsewhere, the path as it is.
    struct At {
        int fd;
        std::string name;
    };
    int held(const std::string& dir) const {
        auto it = roots_->find(dir);
        return it == roots_->end() ? -1 : it->second;
    }
    At where(const std::string& path) const {
        const int root = held(dir_of(path));
        if (root >= 0) {
            return At{root, name_of(path)};
        }
        return At{AT_FDCWD, path};
    }

    PosixOptions options_;
    std::shared_ptr<Roots> roots_;
};

} // namespace

std::string unlockable_filesystem(int dirfd, const std::string& test_fs_type) {
    return test_fs_type.empty() ? unlockable_by_type(dirfd) : unlockable_name(test_fs_type);
}

Status locks_kept(int dirfd, const std::string& dir, const std::string& who, bool test_locks_ignored) {
    Fd again(::openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (again.get() < 0) {
        return os_error("open", dir);
    }
    const bool granted = test_locks_ignored || ::flock(again.get(), LOCK_EX | LOCK_NB) == 0;
    if (granted || errno != EWOULDBLOCK) {
        return Status::failed("locks on " + dir + " are not kept: a second exclusive lock was " +
                              (granted ? std::string("granted") : "neither granted nor refused (" +
                               std::string(std::strerror(errno)) + ")") + " while this process held one, so " + who +
                              " would not be kept out; keep the server's directory on a local disk (--dir, or HOME)");
    }
    return Status::ok();
}

std::unique_ptr<Fs> make_posix_fs(PosixOptions options) {
    return std::make_unique<PosixFs>(std::move(options));
}

} // namespace nexus::store
