// The window's home: everything the window keeps is in ~/.nexus/client, beside nexus-server's ~/.nexus/server. On
// macOS and Linux ~ is HOME; on Windows it is USERPROFILE, so the native window's home is %USERPROFILE%\.nexus\client,
// while nexus-server inside WSL2 has its own $HOME/.nexus/server there: two directories on purpose, each side with its
// own config.
//
//   config            the window's settings (config.h): `nova-config <path>` and `sprint-url <url>`
//   tab-order.txt, selected-tab.txt, text-scale.txt, terminals.txt
//                     its saved preferences (tab_order.h)
//   nexus.log         on Windows only, its error lines (platform.h startErrorLog)
//
// ~/.nexus and ~/.nexus/client are made mode 0700 by whichever of the window and the server starts first.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nexus {

// `home` (HOME, or USERPROFILE on Windows) with .nexus/client added. False, with `why` saying what to set, when `home` is
// null or empty. Pure.
bool clientHomeFrom(const char* home, std::string* dir, std::string* why);

// The window's home from this process's environment: the one place the window resolves it.
bool clientHome(std::string* dir, std::string* why);

// Makes `dir` and the directory above it (~/.nexus), each mode 0700 on POSIX, if missing. False, with why.
bool makeClientHome(const std::string& dir, std::string* why);

// The window's settings, read from its config file.
struct ClientConfig {
    std::string path;               // the file read, for messages; empty when there is no home
    std::string novaConfig;         // `nova-config <path>`: the full path of nova-config; empty: found on the PATH
    std::string sprintUrl;          // `sprint-url <url>`: the sprint board's address; empty: from the sprint's record
    std::vector<std::string> problems;  // one line each: the errors, then the warnings
};

// The keys the window's config takes.
inline const std::vector<std::string> kClientConfigKeys{"nova-config", "sprint-url"};

// The settings in `text`, the file at `path`. Pure.
ClientConfig clientConfigFrom(std::string_view text, const std::string& path);

// The settings in `<dir>/config`: none set (the defaults) if there is no such file; a problem naming the file if it
// cannot be read.
ClientConfig readClientConfig(const std::string& dir);

// The window's settings, read once from its home on the first call (which writes each problem to stderr) and the same
// for the rest of the process. Call it first on the main thread; then any thread may read it.
const ClientConfig& clientConfig();

} // namespace nexus
