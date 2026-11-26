#include "client_home.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "pr/file.h"

#include "config.h"

namespace nexus {

namespace {

#if defined(_WIN32)
constexpr const char* kHomeVariable = "USERPROFILE";
constexpr char kSeparator = '\\';
#else
constexpr const char* kHomeVariable = "HOME";
constexpr char kSeparator = '/';
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
// On iOS and iPadOS HOME is the app's own container, whose top an app may not write: the window's home is under the
// container's Library/Application Support, the system's place for an app's own files, which the system makes on first
// use only when asked (makeClientHome).
constexpr const char* kAppData = "/Library/Application Support";
#else
constexpr const char* kAppData = "";
#endif

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

#if defined(_WIN32)
// The variable's value in UTF-8 (getenv gives the ANSI code page's), empty if unset.
std::string environmentUtf8(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
        return {};
    }
    std::wstring value(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, value.data(), n);
    if (got == 0 || got >= n) {
        return {};
    }
    value.resize(got);
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}
#else
// The directory: made 0700 if missing; it must be a directory this user owns, and it is set to 0700.
bool ensureDir(const std::string& dir, std::string* why) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        *why = "cannot make " + dir + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) {
        *why = "cannot read " + dir + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        *why = dir + " is not a directory of this user's; move it aside";
        return false;
    }
    if ((st.st_mode & 0777) != 0700 && chmod(dir.c_str(), 0700) != 0) {
        *why = "cannot set " + dir + " to mode 0700: " + std::strerror(errno);
        return false;
    }
    return true;
}
#endif

} // namespace

bool clientHomeFrom(const char* home, std::string* dir, std::string* why) {
    if (home == nullptr || *home == 0) {
        *why = std::string(kHomeVariable) + " is not set, so nexus has no home for its settings (" + kHomeVariable +
            "/.nexus/client); start nexus with " + kHomeVariable + " set to your home directory";
        return false;
    }
    std::string d = home;
    while (d.size() > 1 && (d.back() == '/' || d.back() == kSeparator)) {
        d.pop_back();
    }
    *dir = d + kAppData + kSeparator + ".nexus" + kSeparator + "client";
    return true;
}

bool clientHome(std::string* dir, std::string* why) {
#if defined(_WIN32)
    const std::string home = environmentUtf8(L"USERPROFILE");
    return clientHomeFrom(home.c_str(), dir, why);
#else
    return clientHomeFrom(std::getenv(kHomeVariable), dir, why);
#endif
}

bool makeClientHome(const std::string& dir, std::string* why) {
    const std::size_t cut = dir.find_last_of(kSeparator);
    const std::string top = cut == std::string::npos ? std::string() : dir.substr(0, cut);
#if defined(_WIN32)
    if (!prFileCreateDirectory(pr(dir))) {
        *why = "cannot make " + dir;
        return false;
    }
    (void)top;
    return true;
#else
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // Library is the container's own; Application Support within it, the system's, may not exist yet.
    const std::string data = top.substr(0, top.find_last_of(kSeparator));
    if (mkdir(data.c_str(), 0700) != 0 && errno != EEXIST) {
        *why = "cannot make " + data + ": " + std::strerror(errno);
        return false;
    }
#endif
    return (top.empty() || ensureDir(top, why)) && ensureDir(dir, why);
#endif
}

bool claimClientHome(const std::string& dir, std::string* why) {
    // Keep the handle open until the OS tears down the process, including static destructors.
    why->clear();
#if defined(_WIN32)
    const auto path = std::filesystem::u8path(dir) / L"client.lock";
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock != INVALID_HANDLE_VALUE) return true;
    DWORD error = GetLastError();
    if (error != ERROR_SHARING_VIOLATION)
        *why = "cannot lock the client home: Windows error " + std::to_string(error);
#else
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        int result;
        do {
            result = flock(fd, LOCK_EX | LOCK_NB);
        } while (result < 0 && errno == EINTR);
        if (result == 0) return true;
        int error = errno;
        close(fd);
        if (error == EWOULDBLOCK) return false;
        errno = error;
    }
    *why = std::string("cannot lock the client home: ") + std::strerror(errno);
#endif
    return false;
}

ClientConfig clientConfigFrom(std::string_view text, const std::string& path) {
    const Config c = parseConfig(text, path, kClientConfigKeys);
    ClientConfig out;
    out.path = path;
    auto at = c.values.find("nova-config");
    if (at != c.values.end()) {
        out.novaConfig = at->second;
    }
    at = c.values.find("sprint-url");
    if (at != c.values.end()) {
        out.sprintUrl = at->second;
    }
    if (const auto entry = c.values.find("server"); entry != c.values.end()) {
        out.serverHost = entry->second;
        out.serverSet = true;
    }
    if (const auto entry = c.values.find("server-command"); entry != c.values.end())
        out.serverCommand = entry->second;
    out.problems = c.errors;
    if (const auto entry = c.values.find("sprint"); entry != c.values.end()) {
        if (entry->second == "off") out.sprintEnabled = false;
        else if (entry->second != "on") out.problems.push_back(path + ": sprint must be on or off");
    }
    out.problems.insert(out.problems.end(), c.warnings.begin(), c.warnings.end());
    return out;
}

ClientConfig readClientConfig(const std::string& dir) {
    const std::string path = dir + kSeparator + "config";
    PrFileInfo info{};
    if (!prFileInfo(pr(path), &info)) {
        ClientConfig none;
        none.path = path;
        return none;
    }
    PrBlob* blob = prFileOpen(pr(path));
    if (!blob) {
        ClientConfig none;
        none.path = path;
        none.problems.push_back(path + " cannot be read, so its settings are not used; make it readable by you, or "
            "remove it");
        return none;
    }
    const auto bytes = prBlobBytes(blob);
    const std::string text(reinterpret_cast<const char*>(bytes.data), bytes.size);
    prBlobRelease(blob);
    return clientConfigFrom(text, path);
}

const ClientConfig& clientConfig() {
    static const ClientConfig config = [] {
        std::string dir;
        std::string why;
        if (!clientHome(&dir, &why)) {
            ClientConfig none;
            none.problems.push_back(why);
            return none;
        }
        return readClientConfig(dir);
    }();
    static std::once_flag told;
    std::call_once(told, [] {
        for (const std::string& p : config.problems) {
            std::fprintf(stderr, "nexus: %s\n", p.c_str());
        }
    });
    return config;
}

} // namespace nexus
