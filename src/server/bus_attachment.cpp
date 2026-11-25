#include "server/bus_attachment.h"

#include "server/bus_listener.h"
#include "server/sha256.h"
#include "store/codec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <string_view>
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
    why = std::string("Could not ") + action + " for the bus message: " + std::strerror(errno) +
          ". Check the storage directory and retry delivery.";
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

bool privateNode(int fd, bool directory, std::string& why) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return problem("inspect retained mail permissions", why);
    if (st.st_uid == ::geteuid() && (st.st_mode & 0777) == (directory ? 0700 : 0600) &&
        (directory ? S_ISDIR(st.st_mode) : (S_ISREG(st.st_mode) && st.st_nlink == 1))) return true;
    why = "Retained mail requires server-owned directories with mode 0700 and private regular files with mode 0600.";
    return false;
}

bool sameFile(int fd, std::string_view body, std::string& why) {
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
    why = "The stored bus file differs from this delivery. Check the retained file before retrying.";
    return false;
}

bool putBody(int dir, const char* name, std::string_view body, bool privateMail, std::string& why) {
    // Hold one writer per receipt, including when reclaiming a partial write from a previous process.
    if (::flock(dir, LOCK_EX | LOCK_NB) != 0) return problem("lock the incoming folder", why);
    if (::unlinkat(dir, kPartial, 0) != 0 && errno != ENOENT) return problem("clear the partial file", why);
    const Fd existing(::openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (existing.fd >= 0) return (!privateMail || privateNode(existing.fd, false, why)) && sameFile(existing.fd, body, why);
    if (errno != ENOENT) return problem("open the existing file", why);

    const Fd out(::openat(dir, kPartial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (out.fd < 0) return problem("create the partial file", why);
    if (privateMail && !privateNode(out.fd, false, why)) return false;
    std::size_t written = 0;
    while (written < body.size()) {
        const auto n = ::write(out.fd, body.data() + written, body.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return problem("write the partial file", why);
        written += static_cast<std::size_t>(n);
    }
    if (!syncToDisk(out.fd)) return problem("sync the file", why);
    // linkat publishes complete bytes and refuses to overwrite a file the AI may have added.
    if (::linkat(dir, kPartial, dir, name, 0) != 0) return problem("publish the file", why);
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
        why = "An attached bus message needs an AI instance with an absolute working directory on this server.";
        return false;
    }
    Fd root(::open("/", kDirectory));
    if (root.fd < 0) return problem("open the filesystem root", why);
    for (const auto& part : path.relative_path()) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            why = "The receiving AI's working directory contains '..'. Configure its absolute directory.";
            return false;
        }
        const int next = ::openat(root.fd, part.c_str(), kDirectory);
        if (next < 0) return problem("open the receiving AI's workspace", why);
        ::close(root.fd);
        root.fd = next;
    }
    // Hash the receipt identity, never the message or its subject, into a single safe path component.
    const auto folder = "bus-" + sha256Hex(recipient + '\0' + messageId);
    const Fd incoming(childDirectory(root.fd, "incoming"));
    if (incoming.fd < 0) return problem("open the incoming directory", why);
    const Fd dest(childDirectory(incoming.fd, folder.c_str()));
    if (dest.fd < 0) return problem("open the attachment folder", why);
    if (!putBody(dest.fd, kName, body, false, why)) return false;
    // Sync the complete path even on replay: an earlier attempt may have stopped during these syncs.
    if (!syncToDisk(dest.fd) || !syncToDisk(incoming.fd) || !syncToDisk(root.fd))
        return problem("sync the incoming directories", why);
    result = {"[message.txt (" + std::to_string(body.size()) + " bytes)]", "incoming/" + folder + '/' + kName};
    return true;
}

bool retainFilteredBus(int serverDirFd, const std::string& address, const std::string& recipient,
                       const BusMessage& message, std::string& why) {
    why.clear();
    const Fd mail(childDirectory(serverDirFd, "bus-filtered"));
    if (mail.fd < 0) return problem("open the private retained mail directory", why);
    if (!privateNode(mail.fd, true, why)) return false;
    const auto key = sha256Hex(address + '\0' + recipient + '\0' + message.id);
    const Fd dest(childDirectory(mail.fd, key.c_str()));
    if (dest.fd < 0) return problem("open the retained message directory", why);
    if (!privateNode(dest.fd, true, why)) return false;

    // NBUS v1 is the complete decoded delivery. Each field has a little-endian u32 byte length,
    // so embedded newlines and NULs round-trip without interpretation.
    store::Writer record;
    constexpr std::array<std::uint8_t, 5> tag{'N', 'B', 'U', 'S', 1};
    const std::array fields{&address, &recipient, &message.id, &message.from,
                            &message.replyTo, &message.subject, &message.body};
    record.out().reserve(message.body.size() + message.subject.size() + 512);
    record.raw(tag);
    for (const auto* field : fields) {
        if (field->size() > std::numeric_limits<std::uint32_t>::max()) {
            why = "A field exceeds the retained envelope format's size limit.";
            return false;
        }
        record.str(*field);
    }
    const auto& bytes = record.out();
    if (!putBody(dest.fd, "envelope.bin", {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, true, why)) return false;
    // This includes replay after publishing a file but before completing the original directory syncs.
    if (!syncToDisk(dest.fd) || !syncToDisk(mail.fd) || !syncToDisk(serverDirFd))
        return problem("sync the private retained mail directories", why);
    return true;
}

} // namespace nexus::server
