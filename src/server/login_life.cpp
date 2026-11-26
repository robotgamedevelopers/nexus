#include "server/login_life.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iterator>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "accounts/registry.h"
#include "pr/json.h"
#include "store/fs.h"
#include "sync/secrets.h"
#include "sync/signin.h"

namespace nexus::server {

namespace {

// `long` when it is a safe line for the dialog (sync::isSecretWhy), else `short` (a long account name).
std::string fit(const std::string& longer, const std::string& shorter) {
    return sync::isSecretWhy(longer) ? longer : shorter;
}

std::string days(std::uint64_t n) {
    return std::to_string(n) + (n == 1 ? " day" : " days");
}

#ifndef _WIN32
// Use the directory entry's exact spelling even on case-insensitive filesystems. Match the opened inode to that
// entry so a replacement during the lookup cannot associate another file with this name.
int openExactAt(int parent, const char* name, int flags) {
    const int fd = ::openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | flags);
    if (fd < 0) {
        return -1;
    }
    const int listing = ::openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* entries = listing < 0 ? nullptr : ::fdopendir(listing);
    bool matches = false;
    if (entries != nullptr) {
        struct stat opened {};
        if (::fstat(fd, &opened) == 0) {
            while (dirent* entry = ::readdir(entries)) {
                if (std::strcmp(name, entry->d_name) == 0) {
                    matches = opened.st_ino == entry->d_ino;
                    break;
                }
            }
        }
        ::closedir(entries);
    } else if (listing >= 0) {
        ::close(listing);
    }
    if (matches) {
        return fd;
    }
    ::close(fd);
    errno = EINVAL;   // the opened entry does not bear the requested spelling
    return -1;
}
#endif

} // namespace

LoginTerm loginTerm(const std::string& provider) {
    if (provider == "claude") {
        return LoginTerm{365 * kDayMs, false, "Claude login", "run claude setup-token and paste the token it prints"};
    }
    return LoginTerm{0, false, provider + " login", "paste a new one"};
}

std::string dateOf(std::uint64_t ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char out[16];
    std::strftime(out, sizeof out, "%Y-%m-%d", &tm);
    return out;
}

#ifndef _WIN32
int openAccountDirectory(const std::string& dir, std::string& problem) {
    const std::string parent = store::dir_of(dir);
    const int base = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int fd = -1;
    if (base >= 0) {
        fd = openExactAt(base, store::name_of(dir).c_str(), O_DIRECTORY);
        const int error = errno;
        ::close(base);
        errno = error;
    }
    if (fd < 0) {
        const int error = errno;
        problem = error == EINVAL ? dir + " does not match the spelling of the account directory; check the account name's capitalization"
                                  : dir + " cannot be opened as an account directory (" + std::strerror(error) + ")";
        errno = error;
    }
    return fd;
}

std::string verifiedClaudeEmail(const std::string& accountDir) {
    std::string problem;
    const int dir = openAccountDirectory(accountDir, problem);
    if (dir < 0) return std::string();
    struct stat st{};
    const bool privateDir = ::fstat(dir, &st) == 0 && st.st_uid == ::getuid() && (st.st_mode & 0777) == 0700;
    const int fd = privateDir ? openExactAt(dir, "verified-claude.json", O_NONBLOCK) : -1;
    ::close(dir);
    if (fd < 0) return std::string();
    std::string bytes;
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::getuid() && st.st_nlink == 1 &&
        (st.st_mode & 0777) == 0600 && st.st_size > 0 && st.st_size <= 65536) {
        bytes.resize(static_cast<std::size_t>(st.st_size));
        std::size_t at = 0;
        while (at < bytes.size()) {
            const ssize_t n = ::read(fd, bytes.data() + at, bytes.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            at += static_cast<std::size_t>(n);
        }
        char extra;
        if (at != bytes.size() || ::read(fd, &extra, 1) != 0) sync::wipeBytes(bytes);
    }
    ::close(fd);
    if (bytes.empty()) return std::string();
    PrJson* doc = prJsonCreate();
    const bool parsed = doc && prJsonParse(doc, {bytes.data(), bytes.size()}, nullptr);
    std::string identity;
    if (parsed) {
        auto member = [&](PrJsonValueIndex object, std::string_view key) {
            return prJsonMember(doc, object, {key.data(), key.size()});
        };
        const PrStringView email = prJsonString(doc, member(0, "email"));
        const auto grant = member(0, "credential");
        const auto version = member(0, "version");
        const auto expiry = member(grant, "expiresAt");
        const bool valid = prJsonKind(doc, version) == PrJsonKind_Number && prJsonNumber(doc, version, 0) == 1 &&
            email.data && email.size > 0 && email.size <= 254 &&
            prJsonString(doc, member(grant, "accessToken")).size > 0 &&
            prJsonString(doc, member(grant, "refreshToken")).size > 0 &&
            prJsonKind(doc, expiry) == PrJsonKind_Number && prJsonNumber(doc, expiry, 0) > 0;
        if (valid) identity.assign(email.data, email.size);
    }
    prJsonDestroy(doc);
    sync::wipeBytes(bytes);
    return identity;
}

bool hasVerifiedClaude(const std::string& accountDir, const std::string& expectedEmail) {
    if (expectedEmail.empty()) return false;
    const std::string email = verifiedClaudeEmail(accountDir);
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    return email.size() == expectedEmail.size() &&
        std::equal(email.begin(), email.end(), expectedEmail.begin(),
                   [&](char a, char b) { return lower(a) == lower(b); });
}

static int enrollmentDirectory(const std::string& home, const std::string& provider, const std::string& account) {
    if (!sync::enrollsByHelper(provider) || !accounts::isName(account) || home.empty() ||
        home.find('\0') != std::string::npos) {
        errno = EINVAL;
        return -1;
    }
    int dir = ::open(home.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) {
        return -1;
    }
    const char* parts[] = {".nexus", "server", "harness", provider.c_str(), account.c_str()};
    for (size_t i = 0; i < std::size(parts); ++i) {
        const int child = openExactAt(dir, parts[i], O_DIRECTORY);
        ::close(dir);
        if (child < 0) {
            return -1;
        }
        dir = child;
        struct stat info {};
        const mode_t mask = i + 1 == std::size(parts) ? 0077 : 0022;
        if (::fstat(dir, &info) != 0 || info.st_uid != ::getuid() || (info.st_mode & mask) != 0) {
            ::close(dir);
            errno = EACCES;
            return -1;
        }
    }
    return dir;
}

std::string forgetEnrollment(const std::string& home, const std::string& provider, const std::string& account) {
    int dir = enrollmentDirectory(home, provider, account);
    if (dir < 0) return errno == ENOENT ? "" : std::strerror(errno);
    if (::flock(dir, LOCK_EX | LOCK_NB) != 0) {
        ::close(dir);
        return "The account login is in use.";
    }
    const auto removeFile = [](int parent, const char* name) -> std::string {
        if (::unlinkat(parent, name, 0) != 0 && errno != ENOENT) return std::strerror(errno);
        return ::fsync(parent) == 0 ? "" : std::strerror(errno);
    };
    auto error = removeFile(dir, ".nexus-enrollment-pending");
    if (error.empty()) error = removeFile(dir, ".nexus-enrolled");
    const char* file = provider == "antigravity" ? "antigravity-oauth-token" : "auth.json";
    std::vector<const char*> parts;
    if (provider == "codex") parts = {".codex"};
    if (provider == "grok") parts = {".grok"};
    if (provider == "muse") parts = {".config", "muse"};
    if (provider == "antigravity") parts = {".gemini", "antigravity-cli"};
    int parent = ::dup(dir);
    if (parent < 0) error = std::strerror(errno);
    for (const auto part : parts) {
        if (!error.empty()) break;
        const int child = openExactAt(parent, part, O_DIRECTORY);
        if (child < 0 && errno != ENOENT) error = std::strerror(errno);
        ::close(parent);
        parent = child;
        if (parent < 0) break;
    }
    if (parent >= 0) {
        if (error.empty()) error = removeFile(parent, file);
        ::close(parent);
    }
    ::close(dir);
    return error;
}

bool hasEnrollment(const std::string& home, const std::string& provider, const std::string& account) {
    const int dir = enrollmentDirectory(home, provider, account);
    if (dir < 0) return false;
    const std::string expected = provider + "\n";
    std::string bytes(expected.size() + 1, '\0');
    const int marker = openExactAt(dir, ".nexus-enrolled", O_NONBLOCK);
    ::close(dir);
    if (marker < 0) {
        return false;
    }
    struct stat info {};
    const bool valid = ::fstat(marker, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 07777) == 0600 &&
                       info.st_uid == ::getuid() && info.st_nlink == 1;
    size_t used = 0;
    bool eof = false;
    while (valid && used < bytes.size()) {
        const ssize_t count = ::read(marker, bytes.data() + used, bytes.size() - used);
        if (count > 0) {
            used += static_cast<size_t>(count);
        } else if (count == 0 || errno != EINTR) {
            eof = count == 0;
            break;
        }
    }
    ::close(marker);
    bytes.resize(used);
    return eof && bytes == expected;
}

LoginSeen inspectLogin(const std::string& path) {
    LoginSeen seen;
    auto unusable = [&](const std::string& problem) {
        seen.file = LoginSeen::File::Unusable;
        seen.problem = problem;
        return seen;
    };
    std::string problem;
    const int account = openAccountDirectory(store::dir_of(path), problem);
    if (account < 0) {
        return errno == ENOENT ? seen : unusable(problem);
    }
    const int fd = ::openat(account, store::name_of(path).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    const int error = errno;
    ::close(account);
    if (fd < 0) {
        errno = error;
        if (errno == ENOENT) {
            return seen;
        }
        if (errno == ELOOP) {
            return unusable(path + " is a symbolic link; the login must be the file itself: remove the link");
        }
        return unusable(path + " cannot be read (" + std::strerror(errno) + "): make it readable by the user "
                        "nexus-server runs as, mode 0600");
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return unusable(path + " is not a regular file: move it aside");
    }
    if (st.st_uid != ::getuid()) {
        ::close(fd);
        return unusable(path + " belongs to another user: remove it");
    }
    if ((st.st_mode & 077) != 0) {
        ::close(fd);
        return unusable(path + " is open to other users (mode 0" + std::to_string((st.st_mode >> 6) & 7) +
                        std::to_string((st.st_mode >> 3) & 7) + std::to_string(st.st_mode & 7) + "): run chmod 600 " +
                        path + ", and make a new login if anyone else could have read it");
    }
    std::string bytes;
    if (st.st_size > 0 && static_cast<std::uint64_t>(st.st_size) <= sync::kSecretValueBytes + 2) {
        bytes.resize(static_cast<std::size_t>(st.st_size));
        const ssize_t n = ::read(fd, bytes.data(), bytes.size());
        bytes.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    }
    ::close(fd);
    // One token, with at most one line break after it (a file saved by hand).
    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
        bytes.pop_back();
    }
    sync::SecretValue value(bytes);
    sync::wipeBytes(bytes);
    if (const std::string problem = sync::secretShape(store::name_of(path), value); !problem.empty()) {
        seen.file = LoginSeen::File::Malformed;
        seen.problem = problem;
        return seen;
    }
#if defined(__APPLE__)
    seen.storedAtMs = static_cast<std::uint64_t>(st.st_mtimespec.tv_sec) * 1000 +
        static_cast<std::uint64_t>(st.st_mtimespec.tv_nsec) / 1000000;
#else
    seen.storedAtMs = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000 +
        static_cast<std::uint64_t>(st.st_mtim.tv_nsec) / 1000000;
#endif
    seen.file = LoginSeen::File::Usable;
    return seen;
}
#endif

bool loginLive(const std::string& provider, const LoginSeen& seen, std::uint64_t now) {
    if (seen.file != LoginSeen::File::Usable) {
        return false;
    }
    const LoginTerm term = loginTerm(provider);
    return term.lifetimeMs == 0 || term.renewable || now < seen.storedAtMs + term.lifetimeMs;
}

sync::Hub::LoginAsk loginAsk(const std::string& account, const std::string& provider, const LoginSeen& seen,
                             std::uint64_t now) {
    const LoginTerm term = loginTerm(provider);
    switch (seen.file) {
        case LoginSeen::File::Missing:
            return {true, fit(account + " has no " + term.name + " yet: " + term.makeNew,
                              "this account has no " + term.name + " yet: " + term.makeNew)};
        case LoginSeen::File::Malformed:
            return {true, fit(account + "'s " + term.name + " file is not one token: " + term.makeNew,
                              "this account's " + term.name + " file is not one token: " + term.makeNew)};
        case LoginSeen::File::Unusable:
            return {false, account + "'s " + term.name + " cannot be used as its file lies, and a paste would not fix "
                           "it, so nexus asks nothing: " + seen.problem};
        case LoginSeen::File::Usable:
            break;
    }
    if (term.lifetimeMs != 0 && !term.renewable && now >= seen.storedAtMs + term.lifetimeMs) {
        const std::string ended = dateOf(seen.storedAtMs + term.lifetimeMs);
        return {true, fit(account + "'s " + term.name + " ended on " + ended + ", a year after it was pasted, and "
                          "cannot be renewed: " + term.makeNew,
                          "this account's " + term.name + " ended on " + ended + ": " + term.makeNew)};
    }
    // Usable as it lies and not past its end, yet refused by the provider: revoked, or no longer valid.
    return {true, fit(account + "'s " + term.name + " was refused by the provider: revoked, or no longer valid "
                      "(server.log has its words): " + term.makeNew,
                      "this account's " + term.name + " was refused (revoked, or no longer valid): " + term.makeNew)};
}

std::vector<LoginWatch::Notice> LoginWatch::check(const std::vector<Login>& logins, std::uint64_t now) {
    std::vector<Notice> out;
    for (const Login& l : logins) {
        const LoginTerm term = loginTerm(l.provider);
        if (term.lifetimeMs == 0 || term.renewable) {
            continue;
        }
        Told& told = told_[l.account];
        if (told.storedAtMs != l.storedAtMs) {
            told = Told{l.storedAtMs, 0};   // a login stored again: its notices start over
        }
        const std::uint64_t end = l.storedAtMs + term.lifetimeMs;
        const std::uint64_t left = end > now ? (end - now + kDayMs - 1) / kDayMs : 0;
        const int stage = left == 0 ? 4 : left <= 1 ? 3 : left <= 7 ? 2 : left <= 30 ? 1 : 0;
        if (stage <= told.stage) {
            continue;
        }
        told.stage = stage;
        const std::string date = dateOf(end);
        if (stage == 4) {
            out.push_back(Notice{l.account, true,
                fit(l.account + "'s " + term.name + " ended on " + date + ", a year after it was pasted, and cannot "
                    "be renewed: " + term.makeNew,
                    "this account's " + term.name + " ended on " + date + ": " + term.makeNew)});
        } else {
            out.push_back(Notice{l.account, false,
                l.account + "'s " + term.name + " ends on " + date + ", in " + days(left) + ". " + term.name +
                "s last a year and nexus cannot renew them: on that day nexus asks for a new one in its window ("
                + term.makeNew + "). Until then nothing changes."});
        }
    }
    return out;
}

} // namespace nexus::server
