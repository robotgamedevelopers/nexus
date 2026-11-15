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
    if (e == ENOENT)
        return Status::not_found(msg);
    if (e == ENOSPC || e == EDQUOT)
        return Status::failed(msg + " (free space on that disk, then restart the server)");
    if (e == EACCES || e == EPERM || e == EROFS)
        return Status::failed(msg + " (the server's user must be able to write there)");
    return Status::failed(msg + " (check the disk; the server reopens the store from what is durable when restarted)");
}

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            ::close(fd_);
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
    if (::fcntl(fd, F_FULLFSYNC) == 0)
        return Status::ok();
    // Some filesystems (network, FUSE) have no F_FULLFSYNC; fsync is the most they offer.
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY)
        return os_error("F_FULLFSYNC", path);
#endif
    if (::fsync(fd) != 0)
        return os_error("fsync", path);
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

// A lock held: the descriptor flock was taken on. Closing it releases the lock; so does the end of the process. It is
// opened close-on-exec, so no driver or other child the server starts ever holds it.
class PosixLock : public FileLock {
public:
    explicit PosixLock(int fd) : fd_(fd) {}

private:
    Fd fd_;
};

// Where advisory locks cannot be trusted: a network filesystem (another machine's lock is not seen, or the lock is
// emulated), or one that passes locks to another program (FUSE, 9p: WSL2's view of Windows drives). Empty when the
// directory is on a local disk; otherwise the filesystem's name.
std::string unlockable_filesystem(const std::string& dir) {
    struct statfs fs;
    if (::statfs(dir.c_str(), &fs) != 0) {
        return std::string();   // the open below says why
    }
#if defined(__APPLE__)
    if ((fs.f_flags & MNT_LOCAL) == 0) {
        return fs.f_fstypename;
    }
    const std::string type = fs.f_fstypename;
    if (type.rfind("macfuse", 0) == 0 || type.rfind("osxfuse", 0) == 0 || type == "fuse") {
        return type;
    }
    return std::string();
#else
    switch (static_cast<unsigned long>(fs.f_type)) {
    case 0x6969UL:
        return "nfs";
    case 0x517BUL:
        return "smb";
    case 0xFF534D42UL:
        return "cifs";
    case 0xFE534D42UL:
        return "smb2";
    case 0x65735546UL:
        return "fuse";
    case 0x01021997UL:
        return "9p";
    case 0x564CUL:
        return "ncp";
    case 0x5346414FUL:
        return "afs";
    case 0x6B414653UL:
        return "kafs";
    case 0x00C36400UL:
        return "ceph";
    case 0x47504653UL:
        return "gpfs";
    case 0x0BD00BD0UL:
        return "lustre";
    default:
        return std::string();
    }
#endif
}

class PosixFs : public Fs {
public:
    Status lock(const std::string& path, LockMode mode, std::unique_ptr<FileLock>& out) override {
        out.reset();
        const std::string dir = dir_of(path);
        const std::string type = unlockable_filesystem(dir);
        if (!type.empty()) {
            return Status::failed(dir + " is on a " + type + " filesystem, where a lock is not trusted to keep a second "
                                  "nexus-server out; keep the server's directory on a local disk (--dir, or HOME)");
        }
        Fd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (fd.get() < 0) {
            return os_error("open", path);
        }
        const int want = mode == LockMode::Exclusive ? LOCK_EX : LOCK_SH;
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (::flock(fd.get(), want | LOCK_NB) == 0) {
                out = std::make_unique<PosixLock>(fd.release());
                return Status::ok();
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EWOULDBLOCK) {
                const int e = errno;
                return Status::failed("flock " + path + ": " + std::strerror(e) + " (this filesystem takes no locks; keep "
                                      "the server's directory on a local disk)");
            }
            // Refused: a shared lock to be had now means only readers hold it (next_lock_step).
            bool shared_free = false;
            if (mode == LockMode::Exclusive && ::flock(fd.get(), LOCK_SH | LOCK_NB) == 0) {
                shared_free = true;
                ::flock(fd.get(), LOCK_UN);
            }
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (next_lock_step(false, shared_free, static_cast<std::uint64_t>(waited)) != LockStep::Wait) {
                return Status::busy(path + " is locked by another process");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kLockRetryMs));
        }
    }

    Status open_append(const std::string& path, std::unique_ptr<AppendFile>& out) override {
        out.reset();
        int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) {
            return os_error("open", path);
        }
        out = std::make_unique<PosixAppendFile>(fd, path);
        return Status::ok();
    }

    Status make_dir(const std::string& dir) override {
        std::size_t start = 0;
        while (true) {
            std::size_t slash = dir.find('/', start);
            std::string at = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
            if (!at.empty()) {
                if (::mkdir(at.c_str(), 0700) == 0) {
                    Status s = sync_dir(dir_of(at));
                    if (!s.is_ok())
                        return s;
                } else if (errno != EEXIST) {
                    return os_error("mkdir", at);
                }
            }
            if (slash == std::string::npos)
                return Status::ok();
            start = slash + 1;
        }
    }

    Status list(const std::string& dir, std::vector<std::string>& names) override {
        names.clear();
        DIR* d = ::opendir(dir.c_str());
        if (d == nullptr)
            return os_error("opendir", dir);
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..")
                names.push_back(n);
        }
        ::closedir(d);
        return Status::ok();
    }

    Status size(const std::string& path, std::uint64_t& out) override {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
            return os_error("stat", path);
        out = std::uint64_t(st.st_size);
        return Status::ok();
    }

    Status read(const std::string& path, std::uint64_t off, std::uint64_t n, Bytes& out) override {
        out.clear();
        Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.get() < 0)
            return os_error("open", path);
        struct stat st;
        if (::fstat(fd.get(), &st) != 0)
            return os_error("fstat", path);
        std::uint64_t total = std::uint64_t(st.st_size);
        if (off >= total)
            return Status::ok();
        out.resize(std::size_t(std::min(n, total - off)));
        std::size_t got = 0;
        while (got < out.size()) {
            ssize_t r = ::pread(fd.get(), out.data() + got, out.size() - got, off_t(off + got));
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0)
                return os_error("read", path);
            if (r == 0)
                break;
            got += std::size_t(r);
        }
        out.resize(got);
        return Status::ok();
    }

    Status create(const std::string& path) override {
        Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (fd.get() < 0)
            return os_error("create", path);
        return Status::ok();
    }

    Status append(const std::string& path, std::span<const std::uint8_t> data) override {
        Fd fd(::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
        if (fd.get() < 0)
            return os_error("open", path);
        return write_all(fd.get(), data, path);
    }

    Status truncate(const std::string& path, std::uint64_t size) override {
        if (::truncate(path.c_str(), off_t(size)) != 0)
            return os_error("truncate", path);
        return Status::ok();
    }

    Status sync(const std::string& path) override {
        Fd fd(::open(path.c_str(), O_RDWR | O_CLOEXEC));
        if (fd.get() < 0)
            return os_error("open", path);
        return full_sync(fd.get(), path);
    }

    Status sync_dir(const std::string& dir) override {
        Fd fd(::open(dir.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.get() < 0)
            return os_error("open", dir);
        return full_sync(fd.get(), dir);
    }

    Status rename(const std::string& from, const std::string& to) override {
        if (::rename(from.c_str(), to.c_str()) != 0)
            return os_error("rename", from + " to " + to);
        return Status::ok();
    }

    Status remove(const std::string& path) override {
        if (::unlink(path.c_str()) != 0)
            return os_error("unlink", path);
        return Status::ok();
    }

    Status write_new(const std::string& path, std::span<const std::uint8_t> data) override {
        Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
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
};

} // namespace

std::unique_ptr<Fs> make_posix_fs() {
    return std::make_unique<PosixFs>();
}

} // namespace nexus::store
