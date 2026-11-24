#include "server/bus_attachment.h"

#include "server/sha256.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nexus::server {
namespace {

constexpr int kDirectory = O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
constexpr char kName[] = "message.txt";
constexpr char kPartial[] = ".message.partial";

struct Fd {
    explicit Fd(int value) : fd(value) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int fd;
};

bool problem(const char* action, std::string& why) {
    why = std::string("Could not ") + action + " for the bus attachment: " + std::strerror(errno) +
          ". Check the receiving friend's workspace and retry delivery.";
    return false;
}

int childDirectory(int parent, const char* name) {
    if (::mkdirat(parent, name, 0700) != 0 && errno != EEXIST) return -1;
    return ::openat(parent, name, kDirectory);
}

bool syncToDisk(int fd) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) return false;
#endif
    return ::fsync(fd) == 0;
}

bool sameFile(int fd, const std::string& body, std::string& why) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return problem("inspect the existing file", why);
    if (S_ISREG(st.st_mode) && st.st_size >= 0 && static_cast<std::uintmax_t>(st.st_size) == body.size()) {
        std::array<char, 16384> buffer;
        std::size_t offset = 0;
        while (offset < body.size()) {
            const auto n = ::read(fd, buffer.data(), std::min(buffer.size(), body.size() - offset));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return problem("read the existing file", why);
            if (n == 0 || std::memcmp(buffer.data(), body.data() + offset, static_cast<std::size_t>(n)) != 0) break;
            offset += static_cast<std::size_t>(n);
        }
        if (offset == body.size()) return true;
    }
    why = "The existing bus attachment differs from the delivered message. Move it aside and retry delivery.";
    return false;
}

bool putBody(int dir, const std::string& body, std::string& why) {
    // Hold one writer per receipt, including when reclaiming a partial write from a previous process.
    if (::flock(dir, LOCK_EX | LOCK_NB) != 0) return problem("lock the incoming folder", why);
    if (::unlinkat(dir, kPartial, 0) != 0 && errno != ENOENT) return problem("clear the partial file", why);
    const Fd existing(::openat(dir, kName, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (existing.fd >= 0) return sameFile(existing.fd, body, why);
    if (errno != ENOENT) return problem("open the existing file", why);

    const Fd out(::openat(dir, kPartial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (out.fd < 0) return problem("create the partial file", why);
    std::size_t written = 0;
    while (written < body.size()) {
        const auto n = ::write(out.fd, body.data() + written, body.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return problem("write the partial file", why);
        written += static_cast<std::size_t>(n);
    }
    if (!syncToDisk(out.fd)) return problem("sync the file", why);
    // linkat publishes complete bytes and refuses to overwrite a file the friend may have added.
    if (::linkat(dir, kPartial, dir, kName, 0) != 0) return problem("publish the file", why);
    if (::unlinkat(dir, kPartial, 0) != 0) return problem("remove the partial file", why);
    return true;
}

} // namespace

bool prepareBusBody(const std::string& workspace, const std::string& recipient, const std::string& messageId,
                    const std::string& body, BusBody& result, std::string& why) {
    result = {};
    why.clear();
    if (body.size() <= kBusInlineBytes) {
        result = {body, body};
        return true;
    }
    if (body.size() > kBusAttachmentBytes) {
        why = "The message exceeds the 32 MiB attachment limit. Split it into smaller messages.";
        return false;
    }
    const std::filesystem::path path(workspace);
    if (!path.is_absolute() || workspace.find('\0') != std::string::npos) {
        why = "An attached bus message needs a friend instance with an absolute working directory on this server.";
        return false;
    }
    Fd root(::open("/", kDirectory));
    if (root.fd < 0) return problem("open the filesystem root", why);
    for (const auto& part : path.relative_path()) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            why = "The receiving friend's working directory contains '..'. Configure its absolute directory.";
            return false;
        }
        const int next = ::openat(root.fd, part.c_str(), kDirectory);
        if (next < 0) return problem("open the receiving friend's workspace", why);
        ::close(root.fd);
        root.fd = next;
    }
    // Hash the receipt identity, never the message or its subject, into a single safe path component.
    const auto folder = "bus-" + sha256Hex(recipient + '\0' + messageId);
    const Fd incoming(childDirectory(root.fd, "incoming"));
    if (incoming.fd < 0) return problem("open the incoming directory", why);
    const Fd dest(childDirectory(incoming.fd, folder.c_str()));
    if (dest.fd < 0) return problem("open the attachment folder", why);
    if (!putBody(dest.fd, body, why)) return false;
    // Sync the complete path even on replay: an earlier attempt may have stopped during these syncs.
    if (!syncToDisk(dest.fd) || !syncToDisk(incoming.fd) || !syncToDisk(root.fd))
        return problem("sync the incoming directories", why);
    result = {"[message.txt (" + std::to_string(body.size()) + " bytes)]", "incoming/" + folder + '/' + kName};
    return true;
}

} // namespace nexus::server
