#include "server/install.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "server/takeover.h"

extern char** environ;

#ifndef NEXUS_SOURCE_DIR
#define NEXUS_SOURCE_DIR ""
#endif
#ifndef NEXUS_BUILD_DIR
#define NEXUS_BUILD_DIR ""
#endif
#ifndef NEXUS_CMAKE
#define NEXUS_CMAKE "cmake"
#endif
#ifndef NEXUS_CTEST
#define NEXUS_CTEST "ctest"
#endif

namespace nexus::server {

namespace {

// ---- pure text ----------------------------------------------------------------------------------------------------

std::string xml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// A word of a systemd unit's command line or Environment=: quoted, with its quotes and backslashes escaped.
std::string unitWord(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c == '%' ? std::string("%%") : std::string(1, c);
    }
    return out + "\"";
}

bool hasKey(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::size_t at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line[at] == '#') {
            continue;
        }
        if (line.compare(at, key.size(), key) == 0 && (line.size() == at + key.size() ||
                line[at + key.size()] == ' ' || line[at + key.size()] == '\t')) {
            return true;
        }
    }
    return false;
}

// ---- files --------------------------------------------------------------------------------------------------------

bool exists(const std::string& path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool readFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream all;
    all << in.rdbuf();
    *out = all.str();
    return true;
}

// Writes `text` to `path` as a new file renamed over the old one, with `mode`.
bool writeFile(const std::string& path, const std::string& text, mode_t mode, std::string* why) {
    const std::string tmp = path + ".new";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        *why = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            *why = "cannot write " + tmp + ": " + std::strerror(errno);
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0) {
        *why = "cannot write " + tmp + ": " + std::strerror(errno);
        close(fd);
        unlink(tmp.c_str());
        return false;
    }
    if (close(fd) != 0) {
        *why = "cannot write " + tmp + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        *why = "cannot move " + tmp + " to " + path + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Makes `dir` and every directory above it that is missing, each new one with `mode`.
bool makeDirs(const std::string& dir, mode_t mode, std::string* why) {
    if (dir.empty() || isDir(dir)) {
        return true;
    }
    const std::size_t cut = dir.find_last_of('/');
    if (cut != std::string::npos && cut > 0 && !makeDirs(dir.substr(0, cut), mode, why)) {
        return false;
    }
    if (mkdir(dir.c_str(), mode) != 0 && errno != EEXIST) {
        *why = "cannot make " + dir + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

std::string userHome() {
    const passwd* pw = getpwuid(getuid());
    return pw != nullptr && pw->pw_dir != nullptr ? std::string(pw->pw_dir) : std::string();
}

// ---- processes ----------------------------------------------------------------------------------------------------

struct Ran {
    int status = -1;        // the exit status; -1 when it did not start or was ended by a signal
    std::string out;        // standard output and error, when captured
};

// Runs `argv` (found on PATH), standard input from /dev/null. With `capture`, its output is returned;
// without, it goes to this program's standard error, for the AI running setup to read.
Ran run(const std::vector<std::string>& argv, bool capture = true) {
    Ran r;
    int pipeFds[2] = {-1, -1};
    if (capture && pipe(pipeFds) != 0) {
        r.out = std::strerror(errno);
        return r;
    }
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    if (capture) {
        posix_spawn_file_actions_adddup2(&files, pipeFds[1], 1);
        posix_spawn_file_actions_adddup2(&files, pipeFds[1], 2);
        posix_spawn_file_actions_addclose(&files, pipeFds[0]);
        posix_spawn_file_actions_addclose(&files, pipeFds[1]);
    } else {
        posix_spawn_file_actions_adddup2(&files, 2, 1);
    }
    std::vector<std::string> args = argv;
    std::vector<char*> raw;
    for (std::string& a : args) {
        raw.push_back(a.data());
    }
    raw.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, raw[0], &files, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    if (capture) {
        close(pipeFds[1]);
    }
    if (rc != 0) {
        if (capture) {
            close(pipeFds[0]);
        }
        r.out = argv[0] + ": " + std::strerror(rc);
        return r;
    }
    if (capture) {
        char buf[4096];
        for (;;) {
            const ssize_t n = read(pipeFds[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            r.out.append(buf, static_cast<std::size_t>(n));
        }
        close(pipeFds[0]);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

// The full path of `program` on PATH; empty when it is not there.
std::string onPath(const std::string& program) {
    const char* path = std::getenv("PATH");
    std::istringstream dirs(path != nullptr ? path : "");
    std::string dir;
    while (std::getline(dirs, dir, ':')) {
        const std::string full = (dir.empty() ? std::string(".") : dir) + "/" + program;
        if (access(full.c_str(), X_OK) == 0 && !isDir(full)) {
            return full;
        }
    }
    return std::string();
}

// ---- the control socket -------------------------------------------------------------------------------------------

// One request to the running server and its first reply line. False when nothing answers.
bool ask(const std::string& socketPath, const std::string& request, std::string* reply) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof addr.sun_path) {
        return false;
    }
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return false;
    }
    const std::string line = request + "\n";
    if (write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
        close(fd);
        return false;
    }
    reply->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            break;
        }
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0) {
            continue;
        }
        char c = 0;
        if (read(fd, &c, 1) != 1 || c == '\n') {
            break;
        }
        reply->push_back(c);
    }
    close(fd);
    return true;
}

Seen look(const Paths& p, const std::string& placedBuild) {
    Seen s;
    s.placed = placedBuild;
    std::string reply;
    if (!ask(p.socket, "build", &reply)) {
        return s;
    }
    s.answers = true;
    parseBuildReply(reply, &s);     // a server from before takeover answers "ERROR unknown request ..."
    return s;
}

// ---- the service --------------------------------------------------------------------------------------------------

std::string uidText() {
    return std::to_string(static_cast<unsigned long>(getuid()));
}

#if defined(__APPLE__)
// The launchd domain that holds the agent: the person's login session (gui/<uid>) when there is one, else the user's
// background domain (user/<uid>), which an ssh session without a login has.
std::string launchdDomain() {
    const Ran r = run({"launchctl", "print", "gui/" + uidText()});
    return r.status == 0 ? "gui/" + uidText() : "user/" + uidText();
}
#endif

bool serviceLoaded(const Placement& at) {
#if defined(__APPLE__)
    return run({"launchctl", "print", launchdDomain() + "/" + at.serviceName}).status == 0;
#else
    return run({"systemctl", "--user", "is-enabled", at.serviceName + ".service"}).status == 0;
#endif
}

// Writes the service's file and registers it, once. False, with why, when this machine has no per-user service manager
// to register with (then `new` starts the server on demand, and keeps it only until logout).
bool installService(const Paths& p, const Placement& at, const std::string& pathText, std::string* done, bool* changed,
                    std::string* why) {
#if defined(__APPLE__)
    const std::string text = launchdPlist(at.serviceName, at.server, p.dir, p.log, at.home, pathText);
#else
    const std::string text = systemdUnit(at.server, p.dir, at.home, pathText);
#endif
    std::string old;
    const bool same = readFile(at.serviceFile, &old) && old == text;
    const std::size_t cut = at.serviceFile.find_last_of('/');
    if (!makeDirs(at.serviceFile.substr(0, cut), 0755, why)) {
        return false;
    }
    if (!same && !writeFile(at.serviceFile, text, 0644, why)) {
        return false;
    }
    const bool loaded = serviceLoaded(at);
    *changed = !(same && loaded);
#if defined(__APPLE__)
    if (loaded) {
        *done = same ? "the launchd agent " + at.serviceName + " is loaded" :
            "the launchd agent " + at.serviceName + " is rewritten; launchd takes its new settings at its next load";
        return true;
    }
    const Ran r = run({"launchctl", "bootstrap", launchdDomain(), at.serviceFile});
    if (r.status != 0) {
        *why = "launchctl bootstrap " + launchdDomain() + " " + at.serviceFile + " failed: " + trimmed(r.out);
        return false;
    }
    *done = "the launchd agent " + at.serviceName + " is loaded: the server starts at login and again after a "
        "takeover, from " + at.server;
    return true;
#else
    if (run({"systemctl", "--user", "show-environment"}).status != 0) {
        *why = "this machine has no systemd user manager for this user (systemctl --user does not answer), so no "
            "service keeps the server running; `nexus-server new` starts it on demand";
        return false;
    }
    // A unit file outside the user manager's own directory (a HOME other than the user's) is linked into it.
    const std::string own = userHome() + "/.config/systemd/user/" + at.serviceName + ".service";
    if (at.serviceFile != own && !exists(own)) {
        const Ran linked = run({"systemctl", "--user", "link", at.serviceFile});
        if (linked.status != 0) {
            *why = "systemctl --user link " + at.serviceFile + " failed: " + trimmed(linked.out);
            return false;
        }
    }
    const Ran reload = run({"systemctl", "--user", "daemon-reload"});
    if (reload.status != 0) {
        *why = "systemctl --user daemon-reload failed: " + trimmed(reload.out);
        return false;
    }
    if (!loaded) {
        const Ran r = run({"systemctl", "--user", "enable", at.serviceName + ".service"});
        if (r.status != 0) {
            *why = "systemctl --user enable " + at.serviceName + " failed: " + trimmed(r.out);
            return false;
        }
    }
    *done = "the systemd user unit " + at.serviceName + " is enabled: the server starts with the user's manager and "
        "again after a takeover, from " + at.server;
    // Without lingering, the user's manager, and the server with it, ends at the last logout.
    const Ran linger = run({"loginctl", "show-user", uidText(), "-p", "Linger"});
    if (trimmed(linger.out) != "Linger=yes") {
        const Ran on = run({"loginctl", "enable-linger"});
        *done += on.status == 0 ? "; lingering is on, so it runs across logout and from boot" :
            "; lingering could not be turned on (" + trimmed(on.out) + "), so it stops at the last logout";
    }
    return true;
#endif
}

// Starts the service's server now. On Linux the unit's failed state is cleared first: a build that crashed at start
// leaves it there once systemd's start limit is hit, and a start would then be refused.
bool startService(const Placement& at) {
#if defined(__APPLE__)
    return run({"launchctl", "kickstart", launchdDomain() + "/" + at.serviceName}).status == 0;
#else
    run({"systemctl", "--user", "reset-failed", at.serviceName + ".service"});
    return run({"systemctl", "--user", "start", at.serviceName + ".service"}).status == 0;
#endif
}

// ---- placing --------------------------------------------------------------------------------------------------------

std::string fileBuild(const std::string& path) {
    std::string bytes;
    return readFile(path, &bytes) ? buildOf(bytes) : std::string();
}

// Copies a directory tree to `to` (which must not exist), keeping modes, links and (on macOS) signatures.
bool copyTree(const std::string& from, const std::string& to, std::string* why) {
#if defined(__APPLE__)
    const Ran r = run({"/usr/bin/ditto", from, to});
#else
    const Ran r = run({"cp", "-a", "--reflink=auto", from, to});
#endif
    if (r.status != 0) {
        *why = "cannot copy " + from + " to " + to + ": " + trimmed(r.out);
        return false;
    }
    return true;
}

#if defined(__APPLE__)
// Whether the trees `a` and `b` hold the same files with the same bytes.
bool sameTree(const std::string& a, const std::string& b) {
    return isDir(a) && isDir(b) && run({"diff", "-rq", a, b}).status == 0;
}

// Puts a copy of the tree `from` at `to` in one rename: the new tree beside it, the old moved aside, then removed.
// `*copied` says whether it copied; a tree already the same is left as it is. The window's (macOS only).
bool placeTree(const std::string& from, const std::string& to, std::string* why, bool* copied = nullptr) {
    if (copied != nullptr) {
        *copied = false;
    }
    if (sameTree(from, to)) {
        return true;
    }
    if (copied != nullptr) {
        *copied = true;
    }
    const std::string fresh = to + ".new";
    const std::string old = to + ".old";
    run({"rm", "-rf", fresh, old});
    if (!copyTree(from, fresh, why)) {
        return false;
    }
    if (exists(to) && rename(to.c_str(), old.c_str()) != 0) {
        *why = "cannot move " + to + " aside: " + std::strerror(errno);
        return false;
    }
    if (rename(fresh.c_str(), to.c_str()) != 0) {
        *why = "cannot move " + fresh + " to " + to + ": " + std::strerror(errno);
        rename(old.c_str(), to.c_str());
        return false;
    }
    run({"rm", "-rf", old});
    return true;
}
#endif

// The server program to place: the build directory's, which `rebuild` has just made; this program when that is gone.
std::string builtServer(const std::string& self) {
    const std::string built = std::string(NEXUS_BUILD_DIR) + "/nexus-server";
    return access(built.c_str(), X_OK) == 0 ? built : self;
}

// node 20.19 or newer, by full path: the one on PATH, else one the AI fetched to ~/.nexus/node.
std::string findNode(const Placement& at, std::string* version) {
    for (const std::string& node : {onPath("node"), at.top + "/node/bin/node"}) {
        if (node.empty() || access(node.c_str(), X_OK) != 0) {
            continue;
        }
        const Ran r = run({node, "--version"});
        int major = 0;
        int minor = 0;
        if (r.status == 0 && std::sscanf(r.out.c_str(), "v%d.%d", &major, &minor) == 2 &&
                (major > 20 || (major == 20 && minor >= 19))) {
            *version = trimmed(r.out);
            return node;
        }
    }
    return std::string();
}

void say(const char* what, const std::string& text) {
    std::printf("%-5s %s\n", what, text.c_str());
    std::fflush(stdout);
}

// ---- the PATH the service runs the server with ----------------------------------------------------------------------

// Fixed and computed, never the PATH setup happened to run under, so that setup writes the same service file each time:
// node's directory (the drivers' node, and npm for a rebuild from inside a friend's turn), cmake's when the build was
// configured with one by full path, ~/.local/bin, then Homebrew's and the system's directories that exist.
std::string servicePath(const Placement& at, const std::string& node) {
    std::vector<std::string> dirs;
    auto add = [&](const std::string& dir) {
        if (!dir.empty() && isDir(dir) && std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
            dirs.push_back(dir);
        }
    };
    auto dirOf = [](const std::string& file) {
        const std::size_t cut = file.find_last_of('/');
        return cut == std::string::npos || cut == 0 ? std::string() : file.substr(0, cut);
    };
    add(dirOf(node));
    const std::string cmake = NEXUS_CMAKE;
    if (!cmake.empty() && cmake[0] == '/') {
        add(dirOf(cmake));
    }
    add(at.home + "/.local/bin");
    for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"}) {
        add(dir);
    }
    std::string out;
    for (const std::string& dir : dirs) {
        out += (out.empty() ? "" : ":") + dir;
    }
    return out;
}

// ---- the Claude driver's trees --------------------------------------------------------------------------------------

// The trees under `dir`, files only, sorted, each as its path and bytes, hashed: a driver build's name.
void hashInto(const std::string& dir, const std::string& rel, std::string* all) {
    const Ran r = run({"/bin/ls", "-A", dir + rel});
    std::istringstream names(r.out);
    std::vector<std::string> sorted;
    for (std::string name; std::getline(names, name);) {
        if (!name.empty()) {
            sorted.push_back(name);
        }
    }
    std::sort(sorted.begin(), sorted.end());
    for (const std::string& name : sorted) {
        const std::string path = rel + "/" + name;
        if (isDir(dir + path)) {
            hashInto(dir, path, all);
        } else {
            std::string bytes;
            readFile(dir + path, &bytes);
            *all += path + '\0' + bytes + '\0';
        }
    }
}

// A driver build: its dist, package.json and package-lock.json (node_modules follows from the lock).
std::string driverBuild(const std::string& tree) {
    std::string all;
    hashInto(tree, "/dist", &all);
    std::string pkg;
    std::string lock;
    readFile(tree + "/package.json", &pkg);
    readFile(tree + "/package-lock.json", &lock);
    return buildOf(all + "package.json" + '\0' + pkg + '\0' + "package-lock.json" + '\0' + lock);
}

// Copies a tree cheaply where the filesystem can: clones on APFS, reflinks where Linux has them.
bool cloneTree(const std::string& from, const std::string& to, std::string* why) {
#if defined(__APPLE__)
    if (run({"/bin/cp", "-cR", from, to}).status == 0) {
        return true;
    }
    run({"rm", "-rf", to});
#endif
    return copyTree(from, to, why);
}

// ~/.nexus/drivers/claude is a link to one tree, claude@<driver build>; the build placed with a server is named in
// <server>.driver, so that the handover switches the link only when that server starts, and back on a rollback.
std::string driverTree(const Placement& at, const std::string& name) {
    return at.top + "/drivers/" + name;
}

std::string readName(const std::string& file) {
    std::string text;
    readFile(file, &text);
    return trimmed(text);
}

std::string linkTarget(const std::string& path) {
    char target[4096] = {};
    const ssize_t n = readlink(path.c_str(), target, sizeof target - 1);
    return n > 0 ? std::string(target, static_cast<std::size_t>(n)) : std::string();
}

bool isLink(const std::string& path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

// Points ~/.nexus/drivers/claude at the tree `name`, in one rename. Nothing to do for no name (a server placed before
// driver trees) or a link already there.
bool switchDriver(const Placement& at, const std::string& name, std::string* why) {
    if (name.empty() || linkTarget(at.driver) == name) {
        return true;
    }
    if (!isDir(driverTree(at, name))) {
        *why = "the Claude driver " + driverTree(at, name) + " is missing";
        return false;
    }
    const std::string tmp = at.driver + ".link";
    unlink(tmp.c_str());
    if (symlink(name.c_str(), tmp.c_str()) != 0) {
        *why = "cannot make " + tmp + ": " + std::strerror(errno);
        return false;
    }
    if (rename(tmp.c_str(), at.driver.c_str()) != 0) {
        *why = "cannot move " + tmp + " to " + at.driver + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Removes the driver trees no placed server names (the placed one's and the previous one's stay).
void pruneDrivers(const Placement& at) {
    const std::string keep1 = readName(at.server + ".driver");
    const std::string keep2 = readName(at.server + ".previous.driver");
    const Ran r = run({"/bin/ls", "-A", at.top + "/drivers"});
    std::istringstream names(r.out);
    for (std::string name; std::getline(names, name);) {
        if (name.rfind("claude@", 0) == 0 && name != keep1 && name != keep2 && name != linkTarget(at.driver)) {
            run({"rm", "-rf", driverTree(at, name)});
        }
    }
}

// ---- the handover process -------------------------------------------------------------------------------------------

// DIR/handover: the handover's state, one line: `running <pid> <build>`, then `done <build>`, `rolled-back <build>
// <previous>` or `failed <build>`. DIR/handover.log: what it did, a line each.
std::string handoverFile(const Paths& p) {
    return p.dir + "/handover";
}

struct HandoverState {
    std::string state;      // "", running, done, rolled-back, failed
    long pid = 0;
    std::string build;
    std::string previous;
};

HandoverState readHandover(const Paths& p) {
    HandoverState h;
    std::string text;
    if (!readFile(handoverFile(p), &text)) {
        return h;
    }
    std::istringstream words(text);
    words >> h.state;
    if (h.state == "running") {
        words >> h.pid >> h.build;
    } else {
        words >> h.build >> h.previous;
    }
    return h;
}

// A handover process still alive: one whose file says running and whose pid answers.
bool handoverRuns(const HandoverState& h) {
    return h.state == "running" && h.pid > 0 && kill(static_cast<pid_t>(h.pid), 0) == 0;
}

Millis steadyMs() {
    return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Starts `argv` detached (its own session), standard input from /dev/null, its output appended to `log`; its pid, or 0.
pid_t spawnDetached(const std::vector<std::string>& argv, const std::string& log, char** env) {
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&files, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&files, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(__APPLE__)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawn_file_actions_addclosefrom_np(&files, 3);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    std::vector<std::string> args = argv;
    std::vector<char*> raw;
    for (std::string& a : args) {
        raw.push_back(a.data());
    }
    raw.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, raw[0], &files, &attr, raw.data(), env);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : 0;
}

// ---- the probe ------------------------------------------------------------------------------------------------------

// Whether `program` starts as a server at all: `serve` on an empty home of its own under /tmp (HOME there too), asked
// `build` until it answers (at most 15 s), then stopped and its home removed. Nothing of the person's is touched.
bool probe(const std::string& program, std::string* why) {
    char tmpl[] = "/tmp/nexus-probe.XXXXXX";
    if (mkdtemp(tmpl) == nullptr) {
        *why = std::string("cannot make a directory under /tmp to try it in: ") + std::strerror(errno);
        return false;
    }
    const std::string home = tmpl;
    const std::string dir = home + "/.nexus/server";
    if (!makeDirs(dir, 0700, why)) {
        run({"rm", "-rf", home});
        return false;
    }
    std::vector<std::string> envText;
    for (char** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, "HOME=", 5) != 0) {
            envText.emplace_back(*e);
        }
    }
    envText.push_back("HOME=" + home);
    std::vector<char*> env;
    for (std::string& e : envText) {
        env.push_back(e.data());
    }
    env.push_back(nullptr);
    const pid_t pid = spawnDetached({program, "serve", "--dir", dir}, home + "/probe.log", env.data());
    if (pid == 0) {
        *why = "cannot start " + program;
        run({"rm", "-rf", home});
        return false;
    }
    bool answered = false;
    bool exited = false;
    int status = 0;
    std::string reply;
    for (int i = 0; i < 150 && !answered && !exited; ++i) {
        Seen s;
        answered = ask(dir + "/server.sock", "build", &reply) && parseBuildReply(reply, &s);
        exited = !answered && waitpid(pid, &status, WNOHANG) == pid;
        if (!answered && !exited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!exited) {
        ask(dir + "/server.sock", "stop", &reply);
        bool gone = false;
        for (int i = 0; i < 50 && !gone; ++i) {
            gone = waitpid(pid, &status, WNOHANG) == pid;
            if (!gone) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (!gone) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    if (!answered) {
        std::string log;
        readFile(home + "/probe.log", &log);
        log = trimmed(log);
        if (log.size() > 600) {
            log = "..." + log.substr(log.size() - 600);
        }
        *why = program + " did not start as a server on an empty home (" +
            (exited ? "it exited" : "it did not answer in 15 s") + ")" + (log.empty() ? "" : ": " + log);
    }
    run({"rm", "-rf", home});
    return answered;
}

} // namespace

// ---- pure ---------------------------------------------------------------------------------------------------------

Placement placement(const std::string& home, const std::string& realHome) {
    Placement at;
    at.home = home;
    at.top = home + "/.nexus";
    at.bin = at.top + "/bin";
    at.server = at.bin + "/nexus-server";
    at.driver = at.top + "/drivers/claude";
    at.client = at.top + "/client";
    at.app = home + "/Applications/nexus.app";
    at.localBin = home + "/.local/bin/nexus-server";
    const std::string suffix = home == realHome ? std::string() : "-" + buildOf(home).substr(0, 8);
#if defined(__APPLE__)
    at.serviceName = "com.robotgamedevelopers.nexus-server" + suffix;
    at.serviceFile = home + "/Library/LaunchAgents/" + at.serviceName + ".plist";
#else
    at.serviceName = "nexus-server" + suffix;
    at.serviceFile = home + "/.config/systemd/user/" + at.serviceName + ".service";
#endif
    return at;
}

std::string launchdPlist(const std::string& label, const std::string& program, const std::string& dir,
                         const std::string& log, const std::string& home, const std::string& path) {
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<!-- nexus-server, as `nexus-server setup` registers it (SETUP.md). setup rewrites this file. -->\n"
         "<plist version=\"1.0\">\n<dict>\n"
      << "    <key>Label</key>\n    <string>" << xml(label) << "</string>\n"
      << "    <key>ProgramArguments</key>\n    <array>\n        <string>" << xml(program)
      << "</string>\n        <string>serve</string>\n        <string>--dir</string>\n        <string>" << xml(dir)
      << "</string>\n    </array>\n"
      << "    <key>EnvironmentVariables</key>\n    <dict>\n        <key>HOME</key>\n        <string>" << xml(home)
      << "</string>\n        <key>PATH</key>\n        <string>" << xml(path) << "</string>\n    </dict>\n"
      << "    <key>RunAtLoad</key>\n    <true/>\n"
      << "    <!-- again after any exit but a clean one: a takeover exits 75, `nexus-server stop` 0 -->\n"
      << "    <key>KeepAlive</key>\n    <dict>\n        <key>SuccessfulExit</key>\n        <false/>\n    </dict>\n"
      << "    <key>ThrottleInterval</key>\n    <integer>1</integer>\n"
      << "    <key>StandardOutPath</key>\n    <string>" << xml(log) << "</string>\n"
      << "    <key>StandardErrorPath</key>\n    <string>" << xml(log) << "</string>\n"
      << "</dict>\n</plist>\n";
    return o.str();
}

std::string systemdUnit(const std::string& program, const std::string& dir, const std::string& home,
                        const std::string& path) {
    std::ostringstream o;
    o << "# nexus-server, as `nexus-server setup` registers it (SETUP.md). setup rewrites this file.\n"
         "[Unit]\nDescription=nexus-server for " << home << "\n\n"
         "[Service]\nExecStart=" << unitWord(program) << " serve --dir " << unitWord(dir) << "\n"
      << "Environment=" << unitWord("HOME=" + home) << " " << unitWord("PATH=" + path) << "\n"
      << "# again after any exit but a clean one: a takeover exits 75, `nexus-server stop` 0\n"
         "Restart=on-failure\nRestartSec=1\n\n[Install]\nWantedBy=default.target\n";
    return o.str();
}

std::string withLocalServer(const std::string& text) {
    if (hasKey(text, "server")) {
        return text;
    }
    std::string out = text;
    if (!out.empty() && out.back() != '\n') {
        out += '\n';
    }
    return out + "# the nexus-server the window opens on: this machine (`nexus-server setup` wrote this line)\n"
                 "server local\n";
}

std::string firstDriversFile(const std::string& node, const std::string& driverMain) {
    return "# Which program drives which friend (src/server/driver_table.h, drivers/claude/drivers.example). Written by\n"
           "# `nexus-server setup`; yours to change. The Claude driver runs placed, never from the fork being built.\n"
           "driver claude " + node + " " + driverMain + " --account main\n"
           "# The one friend of a first install: Claude, on the account main, in the home directory. The server takes\n"
           "# its first accounts.json and friends.json from these lines once; after that those files are the record.\n"
           "friend claude claude\n";
}

std::string movedDriverPaths(const std::string& text, const std::string& source, const std::string& placed) {
    const std::string from = source + "/drivers/claude/dist/";
    const std::string to = placed + "/dist/";
    std::string out = text;
    for (std::size_t at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size())) {
        out.replace(at, from.size(), to);
    }
    return out;
}

// ---- help ---------------------------------------------------------------------------------------------------------

std::string verbHelp(const std::string& topic) {
    if (topic == "setup") {
        return "usage: nexus-server setup [--no-service] [--wait <seconds>] [--start-wait <seconds>]\n"
               "                          [--program <file>] [--dir DIR]\n"
               "\n"
               "Places a tested build where it runs from and makes it the running server. `rebuild` runs it for you,\n"
               "after the tests pass; run it yourself only to set up again what is already tested. Each step looks\n"
               "first, so a second run changes nothing that is already right. Nothing asks the person anything or\n"
               "needs an administrator.\n"
               "\n"
               "  0. Refusals, before anything changes: a handover still running; a build that is not the placed one\n"
               "     and has no <program>.tested naming its build (rebuild writes it once the tests pass); a build that\n"
               "     does not start as a server on an empty home of its own under /tmp (tried, then removed).\n"
               "  1. ~/.nexus and DIR (default ~/.nexus/server) made 0700; the logins' directories, accounts/*, narrowed\n"
               "     to 0700. A server of the layout before ~/.nexus/server is stopped at once (an open turn on it is\n"
               "     cut, this once) and its sessions adopted.\n"
               "  2. The server copied to ~/.nexus/bin/nexus-server, the one placed before kept as nexus-server.previous\n"
               "     for a rollback; ~/.local/bin/nexus-server linked to it (an older file there is moved to\n"
               "     nexus-server.before-setup).\n"
               "  3. The Claude driver copied from <fork>/drivers/claude (built first: npm ci && npm run build there) to\n"
               "     ~/.nexus/drivers/claude@<its build>, beside the one running; ~/.nexus/drivers/claude links to the\n"
               "     tree of the build that runs, and each placed server runs the tree it was placed with.\n"
               "  4. With no DIR/drivers, a first one, once the driver is placed: the Claude driver under node 20.19 or\n"
               "     newer (PATH, else ~/.nexus/node/bin/node) on the account main, and the one friend claude. A drivers\n"
               "     file that is there is kept; only paths into the fork move to the placed driver (the old file is\n"
               "     kept beside it as drivers.before-setup).\n"
               "  5. macOS: the window copied to ~/Applications/nexus.app, and `server local` in ~/.nexus/client/config.\n"
               "  6. Unless --no-service, a per-user service that runs `serve` at login and again after a takeover: a\n"
               "     launchd agent (macOS) or a systemd user unit with lingering on (Linux), with a PATH of its own:\n"
               "     node's directory, ~/.local/bin, Homebrew's and the system's. Without a user manager it says so (a\n"
               "     note), and the handover starts the server itself.\n"
               "  7. The takeover (nexus-server help takeover), by a handover process of its own that outlives setup.\n"
               "  8. DIR/nexus-config, the accounts and friends (docs/accounts.md), made by the server when it starts.\n"
               "\n"
               "--wait: how long setup watches the handover (default 30 s). Still running then (a friend's turn open on\n"
               "the old server): a note, and exit 3; the handover finishes by itself, with or without a service.\n"
               "--start-wait: how long the placed build has to answer once started before the handover puts the\n"
               "previous build back (default 30 s).\n"
               "--no-service: register no service; the handover starts the server detached.\n"
               "--program: place this program in place of this build (its <file>.tested must name its build).\n"
               "\n"
               "Prints one line per step: `ok` (already right), `done`, `fixed`, `moved`, `start`, `drain`, `stop`,\n"
               "`note`, or `FAIL` with the reason; then `setup: done`, `setup: placed; the handover finishes in the\n"
               "background`, or `setup: done, with the problems above`.\n"
               "Exit: 0 the placed build runs; 1 a FAIL (a refusal, or the placed build did not come up and the\n"
               "previous one runs again); 2 a usage error; 3 placed, the handover still running.\n";
    }
    if (topic == "doctor") {
        return "usage: nexus-server doctor [--dir DIR]\n"
               "\n"
               "Looks, changes nothing, and prints one line per check: `ok`, `note` (needs only the person: the\n"
               "sign-in), or `fix <what is wrong>: <the command that fixes it>`. Then `doctor: nothing to fix` or\n"
               "`doctor: N to fix`. It checks: the fork this program was built from, its remote `upstream`, and that\n"
               "your changes are on a branch of your own rather than main; cmake, ctest and node 20.19 or newer; on\n"
               "macOS the Metal toolchain; ~/.nexus at 0700 and the accounts directory not open to others; the placed\n"
               "server, that it is the build in the build directory, and that it is the one running; no server of the\n"
               "older layout still running; the service; on Linux lingering; DIR/nexus-config; each driver line's\n"
               "program and script, placed rather than in the fork, and its account's login; on macOS the placed\n"
               "window, without a quarantine flag, and its config naming its server.\n"
               "Exit: 0 nothing to fix; 1 something to fix.\n";
    }
    if (topic == "rebuild") {
        return "usage: nexus-server rebuild [--jobs <n>] [--no-service] [--wait <seconds>] [--dir DIR]\n"
               "\n"
               "Builds the fork this program was built from and makes the new build the running server. The loop for\n"
               "changing nexus: edit the fork, commit, `nexus-server rebuild` (it works from inside a nexus\n"
               "conversation: the server hands over once that turn ends).\n"
               "\n"
               "  1. cmake --build <build> on every core, or on <n> with --jobs, which the unit tests use too (it\n"
               "     configures again by itself when CMakeLists.txt changed).\n"
               "  2. In <fork>/drivers/claude: npm ci when node_modules is missing or older than package-lock.json\n"
               "     (an upstream merge that changed dependencies), then npm run build. ~/.nexus/node/bin goes first\n"
               "     on PATH when it holds npm and PATH does not hold it yet.\n"
               "  3. ctest -L unit, and the Claude driver's npm test; once both pass, <build>/nexus-server.tested,\n"
               "     holding the build, which setup asks for (removed before step 1, so a failed rebuild leaves none).\n"
               "  4. The new build's own setup: <build>/nexus-server setup --dir DIR --wait 5, with --no-service and\n"
               "     --wait passed on. It waits only 5 s by default, since a rebuild asked from inside a friend's turn\n"
               "     cannot see the handover until that turn ends.\n"
               "\n"
               "A handover still running refuses it. A build or test that fails stops it there with `FAIL`, and\n"
               "nothing that runs is changed.\n"
               "Exit: setup's: 0 the new build runs; 3 placed, the handover finishing in the background (doctor says\n"
               "later how it ended); 1 a step failed, or the new build did not come up and the previous one runs\n"
               "again; 2 a usage error.\n";
    }
    if (topic == "takeover") {
        return "How a new build takes over from the running nexus-server (server/takeover.h, models/Takeover.tla).\n"
               "\n"
               "setup starts `nexus-server handover`, detached, and it outlives setup. It asks the server on its control\n"
               "socket, DIR/server.sock, `build`; the server answers `BUILD <hash>`, the hash of its program's bytes\n"
               "(and its driver tree's), with ` draining` while it drains. Then one step at a time:\n"
               "  nothing answers                    start the placed program (the service, else `serve` detached)\n"
               "  the placed build answers           done\n"
               "  another build answers              ask it `takeover`: it drains\n"
               "  it does not know `build`           it is from before takeover: ask it `stop`\n"
               "  it is draining                     wait\n"
               "  started, and no answer in 30 s     put nexus-server.previous back and start it: a rollback\n"
               "A draining server stops as soon as no friend's turn is open, so no turn is cut; after 10 minutes it\n"
               "stops anyway and each turn still open is cut with a notice. It exits 75; the service, or the handover\n"
               "when there is none, starts the placed program on the same store, which keeps every session. One server\n"
               "holds the socket and the store at a time, so a turn never runs on two. A server from before takeover,\n"
               "or of the layout before ~/.nexus/server, is stopped at once: an open turn on it is cut, on that first\n"
               "upgrade only. DIR/handover says how the last handover ended (`done`, `rolled-back`, `failed`), and\n"
               "DIR/handover.log what it did. `nexus-server stop` exits 0, and the service leaves the server stopped\n"
               "until the next login or setup.\n";
    }
    return std::string();
}

// ---- setup --------------------------------------------------------------------------------------------------------

DriverPin pinnedDriver(const std::string& self, const std::string& home) {
    const std::string name = readName(self + ".driver");
    if (name.empty() || name.find('/') != std::string::npos) {
        return DriverPin{};
    }
    const std::string drivers = home + "/.nexus/drivers";
    return DriverPin{drivers + "/claude", drivers + "/" + name};
}

std::string runningBuild(const std::string& programBuild, const DriverPin& pin) {
    if (pin.tree.empty()) {
        return programBuild;
    }
    return buildOf(programBuild + " " + pin.tree.substr(pin.tree.find_last_of('/') + 1));
}

int buildOfFile(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::fprintf(stderr, "usage: nexus-server build-of <file>\n");
        return 2;
    }
    std::string bytes;
    if (!readFile(args[0], &bytes)) {
        std::fprintf(stderr, "nexus-server: cannot read %s\n", args[0].c_str());
        return 1;
    }
    std::printf("%s\n", buildOf(bytes).c_str());
    return 0;
}

int setup(const Paths& p, const std::string& self, const std::vector<std::string>& args) {
    signal(SIGPIPE, SIG_IGN);
    bool service = true;
    int waitSeconds = 30;
    int startWait = static_cast<int>(kStartMs / 1000);
    std::string program;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-service") {
            service = false;
        } else if (args[i] == "--wait" && i + 1 < args.size()) {
            waitSeconds = std::atoi(args[++i].c_str());
        } else if (args[i] == "--start-wait" && i + 1 < args.size() && std::atoi(args[i + 1].c_str()) > 0) {
            startWait = std::atoi(args[++i].c_str());
        } else if (args[i] == "--program" && i + 1 < args.size()) {
            program = args[++i];
        } else {
            std::fprintf(stderr, "usage: nexus-server setup [--no-service] [--wait <seconds>] [--start-wait <seconds>] "
                "[--program <file>] [--dir DIR]\n");
            return 2;
        }
    }
    const Placement at = placement(p.home, userHome());
    const std::string source = NEXUS_SOURCE_DIR;
    std::string why;
    int problems = 0;
    auto fail = [&](const std::string& text) {
        say("FAIL", text);
        ++problems;
    };

    // 0. Before anything changes: no handover still running, and the build to place tested and able to start.
    const HandoverState before = readHandover(p);
    if (handoverRuns(before)) {
        fail("a handover to build " + before.build + " still runs (pid " + std::to_string(before.pid) + "): it ends "
            "when the running server's open turns end (at most " + std::to_string(kDrainMs / 60000) + " minutes) and "
            "the new build answers, or it rolls back. `nexus-server doctor` says when it has ended; then run setup "
            "again. Nothing was changed");
        return 1;
    }
    const std::string built = !program.empty() ? program : self == at.server ? builtServer(self) : self;
    std::string bytes;
    if (!readFile(built, &bytes)) {
        fail("cannot read " + built + ", the server to place; nothing was changed");
        return 1;
    }
    const std::string placedBuild = buildOf(bytes);
    const std::string current = fileBuild(at.server);
    const std::string fork = source + "/drivers/claude";
    const bool forkDriverBuilt = isDir(fork + "/dist") && isDir(fork + "/node_modules");
    const std::string forkDriver = forkDriverBuilt ? "claude@" + driverBuild(fork) : std::string();
    const bool newDriver = forkDriverBuilt && readName(at.server + ".driver") != forkDriver;
    if (current != placedBuild || newDriver) {
        // rebuild removes <program>.tested before it builds, and writes it, holding the program's build, only once
        // the tests pass, the Claude driver's with them (rebuild's step 3).
        if (readName(built + ".tested") != placedBuild) {
            fail("the build " + placedBuild + " (" + built + ") has not passed its tests: there is no " + built +
                ".tested naming it. Run `" + std::string(NEXUS_BUILD_DIR) + "/nexus-server rebuild`, which builds, "
                "tests, and then sets the build up. Nothing was changed");
            return 1;
        }
        if (current != placedBuild) {
            if (!probe(built, &why)) {
                fail(why + ". Nothing was changed: the running server and the placed build stay as they are");
                return 1;
            }
            say("ok", "the build " + placedBuild + " passed its tests and starts as a server on an empty home");
        } else {
            say("ok", "the build " + placedBuild + " passed its tests, the Claude driver's with them");
        }
    }

    // 1. The homes, 0700; the logins' directories no wider (the Claude driver refuses one others can reach).
    if (!makeDirs(at.top, 0700, &why) || !makeDirs(p.dir, 0700, &why) || !makeDirs(at.bin, 0700, &why) ||
            !makeDirs(p.accounts, 0700, &why)) {
        fail(why);
        return 1;
    }
    chmod(at.top.c_str(), 0700);
    {
        struct stat st{};
        if (stat(p.accounts.c_str(), &st) == 0 && st.st_uid == getuid() && (st.st_mode & 077) != 0) {
            chmod(p.accounts.c_str(), 0700);
            say("fixed", p.accounts + " was open to others; it is 0700 now");
        }
    }
    const Ran logins = run({"/bin/ls", p.accounts});
    std::istringstream names(logins.out);
    for (std::string name; std::getline(names, name);) {
        const std::string d = p.accounts + "/" + name;
        struct stat st{};
        if (!name.empty() && stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
                (st.st_mode & 077) != 0) {
            chmod(d.c_str(), 0700);
            say("fixed", d + " was open to others; it is 0700 now");
        }
    }

    // 1b. A server of the layout before ~/.nexus/server (its home ~/.nexus itself, its socket ~/.nexus/server.sock):
    // its sessions are adopted into the new home's store when that has none yet, and it is stopped. Its store stays.
    const std::string oldSocket = at.top + "/server.sock";
    if (p.dir != at.top) {
        const std::string oldLog = at.top + "/store/log";
        const std::string newStore = p.dir + "/store";
        std::string reply;
        const bool oldRuns = ask(oldSocket, "stop", &reply);
        if (oldRuns) {
            say("stop", "a server of the layout before " + p.dir + " ran on " + oldSocket + "; it is stopped");
            for (int i = 0; i < 50 && ask(oldSocket, "build", &reply); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        std::string sessions;
        if (!exists(newStore) && readFile(oldLog, &sessions)) {
            if (makeDirs(newStore, 0700, &why) && writeFile(newStore + "/log", sessions, 0600, &why)) {
                say("done", "the sessions in " + oldLog + " are adopted into " + newStore + " (the old store stays)");
            } else {
                fail("the sessions in " + oldLog + " were not adopted: " + why);
            }
        }
    }

    // 2a. A Claude driver placed before driver trees (a directory at ~/.nexus/drivers/claude): moved to a tree of its
    // own beside the new ones, with the link in its place, and named as the placed server's driver.
    if (isDir(at.driver) && !isLink(at.driver)) {
        const std::string name = "claude@" + driverBuild(at.driver);
        const std::string tree = driverTree(at, name);
        const bool moved = (isDir(tree) || rename(at.driver.c_str(), tree.c_str()) == 0) &&
            (!exists(at.driver) || run({"rm", "-rf", at.driver}).status == 0) &&
            symlink(name.c_str(), at.driver.c_str()) == 0;
        if (!moved) {
            fail("the Claude driver at " + at.driver + " was not moved to " + tree + ": " + std::strerror(errno));
            return 1;
        }
        if (!current.empty() && readName(at.server + ".driver").empty() &&
                !writeFile(at.server + ".driver", name + "\n", 0644, &why)) {
            fail(why);
            return 1;
        }
        say("moved", "the Claude driver " + at.driver + " to " + tree + "; " + at.driver + " links to it");
    }

    // 2. The server, placed, the one placed before kept beside it (with its driver's name) for a rollback. After a
    // handover that failed, the program kept is the one from before it, the last that ran.
    if (!current.empty() && (current != placedBuild || newDriver) && before.state != "failed") {
        std::string oldBytes;
        const std::string oldDriver = readName(at.server + ".driver");
        if (!readFile(at.server, &oldBytes) || !writeFile(at.server + ".previous", oldBytes, 0755, &why) ||
                !writeFile(at.server + ".previous.driver", oldDriver.empty() ? std::string() : oldDriver + "\n", 0644,
                           &why)) {
            fail("the placed server was not kept as " + at.server + ".previous, so nothing was placed: " + why);
            return 1;
        }
    }
    if (current == placedBuild) {
        say("ok", "the server is placed: " + at.server + " (build " + placedBuild + ")");
    } else if (!writeFile(at.server, bytes, 0755, &why)) {
        fail(why);
        return 1;
    } else {
        unlink((at.server + ".driver").c_str());
        say("done", "the server is placed: " + at.server + " (build " + placedBuild + ", from " + built + ")" +
            (current.empty() ? std::string() : "; the one before it is kept as " + at.server + ".previous"));
    }
    // A link where a login shell finds it, for a window on another machine (`--server <this machine>` runs
    // `nexus-server new` over ssh). An older program there is moved aside, never removed.
    if (linkTarget(at.localBin) == at.server) {
        say("ok", at.localBin + " links to the placed server");
    } else {
        const std::string aside = at.localBin + ".before-setup";
        const std::size_t cut = at.localBin.find_last_of('/');
        if (!makeDirs(at.localBin.substr(0, cut), 0755, &why)) {
            fail(why);
        } else if (exists(at.localBin) && rename(at.localBin.c_str(), aside.c_str()) != 0) {
            fail("cannot move the older " + at.localBin + " to " + aside + ": " + std::strerror(errno));
        } else {
            if (exists(aside) && !exists(at.localBin)) {
                say("moved", "the older " + at.localBin + " to " + aside);
            }
            if (symlink(at.server.c_str(), at.localBin.c_str()) == 0) {
                say("done", at.localBin + " links to the placed server");
            } else {
                fail("cannot link " + at.localBin + " to " + at.server + ": " + std::strerror(errno));
            }
        }
    }

    // 3. The Claude driver, placed as a tree of its own, claude@<driver build>, beside the running one, and named in
    // <server>.driver: the placed server runs it (pinnedDriver) and the handover points the link at it.
    std::string driverName;
    if (forkDriverBuilt) {
        driverName = forkDriver;
        const std::string tree = driverTree(at, driverName);
        const std::string fresh = tree + ".new";
        bool ok = makeDirs(at.top + "/drivers", 0700, &why);
        bool copied = false;
        if (ok && !isDir(tree)) {
            copied = true;
            std::string pkg;
            std::string lock;
            run({"rm", "-rf", fresh});
            ok = makeDirs(fresh, 0700, &why) && cloneTree(fork + "/dist", fresh + "/dist", &why) &&
                cloneTree(fork + "/node_modules", fresh + "/node_modules", &why);
            if (ok && (!readFile(fork + "/package.json", &pkg) || !readFile(fork + "/package-lock.json", &lock))) {
                why = "cannot read " + fork + "/package.json and package-lock.json";
                ok = false;
            }
            ok = ok && writeFile(fresh + "/package.json", pkg, 0644, &why) &&
                writeFile(fresh + "/package-lock.json", lock, 0644, &why);
            if (ok && rename(fresh.c_str(), tree.c_str()) != 0) {
                why = "cannot move " + fresh + " to " + tree + ": " + std::strerror(errno);
                ok = false;
            }
            if (!ok) {
                run({"rm", "-rf", fresh});
            }
        }
        if (ok && !exists(tree + "/dist/src/main.js")) {
            why = tree + "/dist/src/main.js is missing";
            ok = false;
        }
        ok = ok && (readName(at.server + ".driver") == driverName ||
            writeFile(at.server + ".driver", driverName + "\n", 0644, &why));
        if (ok) {
            say(copied ? "done" : "ok", "the Claude driver is placed: " + tree);
        } else {
            fail("the Claude driver was not placed: " + why);
            driverName.clear();
        }
    } else {
        fail("the Claude driver is not built in " + fork + ": run `npm ci && npm run build` there, then setup again");
    }

    // 4. The drivers file: the Claude driver on a first install, once its main.js is placed; paths into the fork
    // moved to the placed driver.
    std::string nodeVersion;
    const std::string node = findNode(at, &nodeVersion);
    std::string table;
    if (!readFile(p.drivers, &table)) {
        if (node.empty()) {
            fail("no node 20.19 or newer on PATH or in " + at.top + "/node, so no drivers file was written (SETUP.md, "
                "Node)");
        } else if (driverName.empty()) {
            fail("the Claude driver is not placed (above), so no drivers file was written");
        } else if (writeFile(p.drivers, firstDriversFile(node, at.driver + "/dist/src/main.js"), 0600, &why)) {
            say("done", "the drivers file " + p.drivers + " names the Claude driver, under " + node + " (" +
                nodeVersion + ")");
        } else {
            fail(why);
        }
    } else {
        const std::string moved = movedDriverPaths(table, source, at.driver);
        if (moved == table) {
            say("ok", "the drivers file " + p.drivers + " is kept as it is");
        } else if (!writeFile(p.drivers + ".before-setup", table, 0600, &why)) {
            fail(p.drivers + " runs the Claude driver from the fork, and was not rewritten: its copy was not kept: " +
                why);
        } else if (!writeFile(p.drivers, moved, 0600, &why)) {
            fail(p.drivers + " runs the Claude driver from the fork, and was not rewritten: " + why);
        } else {
            say("fixed", p.drivers + " ran the Claude driver from the fork; it runs the placed one now (the old "
                "file is " + p.drivers + ".before-setup)");
        }
    }

#if defined(__APPLE__)
    // 5. The window, placed as an app the person opens, and told to open on this machine's server.
    const std::string builtApp = std::string(NEXUS_BUILD_DIR) + "/nexus.app";
    if (access((builtApp + "/Contents/MacOS/nexus").c_str(), X_OK) == 0) {
        bool copied = false;
        if (makeDirs(at.home + "/Applications", 0755, &why) && placeTree(builtApp, at.app, &why, &copied)) {
            // A local build carries no quarantine flag; one copied in from elsewhere would make Gatekeeper ask.
            if (run({"/usr/bin/xattr", "-p", "com.apple.quarantine", at.app}).status == 0) {
                run({"/usr/bin/xattr", "-dr", "com.apple.quarantine", at.app});
                say("fixed", at.app + " carried a quarantine flag; it is removed");
            }
            say(copied ? "done" : "ok", "the window is placed: " + at.app + " (the person opens it: Spotlight, Finder "
                "or Launchpad)");
        } else {
            fail("the window was not placed: " + why);
        }
    } else {
        fail("the window is not built: " + builtApp + "/Contents/MacOS/nexus is missing (SETUP.md, Prerequisites: "
            "the Metal toolchain; then nexus-server rebuild)");
    }
    std::string clientConfig;
    readFile(at.client + "/config", &clientConfig);
    const std::string withServer = withLocalServer(clientConfig);
    if (withServer == clientConfig) {
        say("ok", "the window's config names its server");
    } else if (makeDirs(at.client, 0700, &why) && writeFile(at.client + "/config", withServer, 0600, &why)) {
        say("done", "the window opens on this machine's server (`server local` in " + at.client + "/config)");
    } else {
        fail(why);
    }
#endif

    // 6. The service.
    bool haveService = false;
    if (service) {
        std::string done;
        bool changed = false;
        if (installService(p, at, servicePath(at, node), &done, &changed, &why)) {
            say(changed ? "done" : "ok", done);
            haveService = true;
        } else {
            say("note", why);
        }
    }

    // 7. The takeover (server/takeover.h): the placed build running. The handover process does it, detached, so that
    // it finishes after setup has gone (setup run from inside a friend's turn, --wait over, a service or none); setup
    // watches it for --wait seconds and says what it saw.
    const std::string target = runningBuild(placedBuild, pinnedDriver(at.server, p.home));
    bool running = false;
    bool handedOver = false;
    if (nextStep(look(p, target)) == Step::Done) {
        say("ok", "the server runs the placed build " + target);
        handedOver = true;
        if (!switchDriver(at, readName(at.server + ".driver"), &why)) {
            fail(why);
        }
        pruneDrivers(at);
    } else {
        unlink(handoverFile(p).c_str());
        std::vector<std::string> argv = {self, "handover", "--dir", p.dir, "--start-wait", std::to_string(startWait)};
        if (haveService) {
            argv.emplace_back("--service");
        }
        const pid_t pid = spawnDetached(argv, p.dir + "/handover.log", environ);
        if (pid == 0) {
            fail("the handover process (" + self + " handover) did not start, so the placed build does not run yet; "
                "run setup again");
        } else {
            say("start", "the handover (pid " + std::to_string(pid) + ", its log " + p.dir + "/handover.log) brings "
                "the server to the placed build " + target + (haveService ? ", with the service" : ""));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
            Step last = Step::Done;
            bool first = true;
            HandoverState now;
            bool exited = false;
            for (;;) {
                now = readHandover(p);
                if (now.state != "" && now.state != "running") {
                    break;
                }
                exited = exited || waitpid(pid, nullptr, WNOHANG) == pid;
                if (exited && (now.state.empty() || now.state == "running")) {
                    now = readHandover(p);
                    break;
                }
                const Seen seen = look(p, target);
                // Asked to drain, then draining: one line.
                const Step step = nextStep(seen) == Step::Wait ? Step::AskDrain : nextStep(seen);
                if (first || step != last) {
                    first = false;
                    last = step;
                    if (step == Step::AskDrain || step == Step::Wait) {
                        say("drain", "the server runs build " + seen.build + "; it hands over to " + target +
                            " once no friend's turn is open");
                    } else if (step == Step::AskStop) {
                        say("stop", "the server is from before takeover, or of an older layout; it is stopped at once "
                            "(an open turn on it is cut, this once)");
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            exited = exited || waitpid(pid, nullptr, WNOHANG) == pid;
            if (now.state == "done") {
                say("ok", "the server runs the placed build " + now.build);
                handedOver = true;
            } else if (now.state == "rolled-back") {
                fail("the placed build " + now.build + " did not come up on the store within " +
                    std::to_string(startWait) + " s, so the handover put the previous build " + now.previous +
                    " back, and it runs again (see " + p.log + " and " + p.dir + "/handover.log). Fix the cause in "
                    "your fork, then `nexus-server rebuild`");
            } else if (now.state == "failed") {
                fail("the placed build " + now.build + " did not come up on the store, and there was no previous "
                    "build that ran to put back: no server runs (see " + p.log + " and " + p.dir + "/handover.log)");
            } else if (exited) {
                fail("the handover process ended without saying how (see " + p.dir + "/handover.log)");
            } else {
                running = true;
                say("note", "the handover still runs, in the background: the server drains until no friend's turn is "
                    "open (at most " + std::to_string(kDrainMs / 60000) + " minutes), then the placed build starts, or "
                    "the previous one again if it does not come up. `nexus-server doctor` says how it ended");
            }
        }
    }

    // 8. The accounts and friends' repository: the server makes it when it starts.
    if (isDir(p.dir + "/nexus-config/.git")) {
        say("ok", "the accounts and friends live in " + p.dir + "/nexus-config (docs/accounts.md)");
    } else if (handedOver) {
        fail(p.dir + "/nexus-config was not made; see " + p.log);
    }
    if (problems != 0) {
        std::printf("setup: done, with the problems above\n");
        return 1;
    }
    std::printf("%s\n", running ? "setup: placed; the handover finishes in the background" : "setup: done");
    return running ? 3 : 0;
}

// ---- handover -----------------------------------------------------------------------------------------------------

int handover(const Paths& p, const std::string& self, const std::vector<std::string>& args) {
    (void)self;
    signal(SIGPIPE, SIG_IGN);
    bool service = false;
    Millis startMs = kStartMs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--service") {
            service = true;
        } else if (args[i] == "--start-wait" && i + 1 < args.size() && std::atoi(args[i + 1].c_str()) > 0) {
            startMs = static_cast<Millis>(std::atoi(args[++i].c_str())) * 1000;
        } else {
            std::fprintf(stderr, "usage: nexus-server handover [--service] [--start-wait <seconds>] [--dir DIR]\n");
            return 2;
        }
    }
    const Placement at = placement(p.home, userHome());
    const std::string placed = runningBuild(fileBuild(at.server), pinnedDriver(at.server, p.home));
    const std::string previousProgram = at.server + ".previous";
    const std::string previous = exists(previousProgram) ?
        runningBuild(fileBuild(previousProgram), pinnedDriver(previousProgram, p.home)) : std::string();
    std::string why;
    auto state = [&](const std::string& line) {
        if (!writeFile(handoverFile(p), line + "\n", 0600, &why)) {
            std::fprintf(stderr, "nexus-server handover: %s\n", why.c_str());
        }
    };
    auto log = [](const std::string& line) {
        std::fprintf(stderr, "nexus-server handover: %s\n", line.c_str());
    };
    state("running " + std::to_string(static_cast<long>(getpid())) + " " + placed);
    log("to the placed build " + placed + (previous.empty() ? ", with no previous build" : ", previous " + previous) +
        (service ? ", with the service" : ", with no service"));
    Handover h(placed, previous, startMs);
    Act last = Act::Done;
    bool first = true;
    for (;;) {
        const Seen seen = look(p, h.placed());
        const Act act = h.step(seen, steadyMs());
        if (first || act != last) {
            first = false;
            last = act;
            log(std::string(actName(act)) + (seen.knowsBuild ? " (the server runs build " + seen.build +
                (seen.draining ? ", draining)" : ")") : seen.answers ? " (a server from before takeover runs)" :
                " (no server answers)"));
        }
        std::string reply;
        switch (act) {
            case Act::Done:
                switchDriver(at, readName(at.server + ".driver"), &why);
                pruneDrivers(at);
                state(h.rolledBack() ? "rolled-back " + placed + " " + h.placed() : "done " + h.placed());
                return h.rolledBack() ? 1 : 0;
            case Act::Fail:
                state("failed " + placed);
                return 1;
            case Act::AskStop:
                ask(p.socket, "stop", &reply);
                break;
            case Act::AskDrain:
                ask(p.socket, "takeover", &reply);
                break;
            case Act::Start:
                // The link first, so that a window or doctor sees the driver of the build that runs.
                if (!switchDriver(at, readName(at.server + ".driver"), &why)) {
                    log(why);
                }
                if (!(service && startService(at)) &&
                        spawnDetached({at.server, "serve", "--dir", p.dir}, p.log, environ) == 0) {
                    log("cannot start " + at.server);
                }
                break;
            case Act::RollBack: {
                std::string bytes;
                const std::string driverName = readName(previousProgram + ".driver");
                if (!readFile(previousProgram, &bytes) || !writeFile(at.server, bytes, 0755, &why) ||
                        !writeFile(at.server + ".driver", driverName.empty() ? std::string() : driverName + "\n", 0644,
                                   &why)) {
                    log("the previous build was not put back: " + why);
                    state("failed " + placed);
                    return 1;
                }
                log("the previous build " + h.placed() + " is back at " + at.server);
                break;
            }
            case Act::Wait:
                break;
        }
        // Quick while the old server drains, so that the moment it is gone is seen at once.
        std::this_thread::sleep_for(std::chrono::milliseconds(seen.draining ? 50 : 200));
    }
}

// ---- doctor -------------------------------------------------------------------------------------------------------

int doctor(const Paths& p, const std::string& self) {
    signal(SIGPIPE, SIG_IGN);
    const Placement at = placement(p.home, userHome());
    const std::string source = NEXUS_SOURCE_DIR;
    const std::string build = NEXUS_BUILD_DIR;
    int missing = 0;
    auto ok = [](const std::string& text) { say("ok", text); };
    auto fix = [&](const std::string& text, const std::string& command) {
        say("fix", text + ": " + command);
        ++missing;
    };

    // The fork.
    if (!isDir(source + "/.git")) {
        fix("the fork " + source + " this program was built from is gone", "clone your fork again (SETUP.md)");
    } else {
        ok("your fork of nexus: " + source + ", built in " + build);
        const Ran up = run({"git", "-C", source, "remote", "get-url", "upstream"});
        if (up.status != 0) {
            fix("your fork has no remote `upstream`", "git -C " + source + " remote add upstream " + kUpstreamUrl);
        } else {
            ok("upstream: " + trimmed(up.out));
        }
        const Ran branch = run({"git", "-C", source, "rev-parse", "--abbrev-ref", "HEAD"});
        if (trimmed(branch.out) == "main") {
            fix("your changes belong on your own branch, not main", "git -C " + source + " switch -c mine");
        } else if (branch.status == 0) {
            ok("your changes go on the branch " + trimmed(branch.out));
        }
    }
    // The tools a rebuild uses.
    for (const std::string& tool : {std::string(NEXUS_CMAKE), std::string(NEXUS_CTEST)}) {
        if (access(tool.c_str(), X_OK) == 0 || !onPath(tool).empty()) {
            ok("build tool: " + tool);
        } else {
            fix(tool + " is gone, so `nexus-server rebuild` cannot build", "SETUP.md, Prerequisites");
        }
    }
    std::string nodeVersion;
    const std::string node = findNode(at, &nodeVersion);
    if (node.empty()) {
        fix("no node 20.19 or newer", "SETUP.md, Prerequisites: Node");
    } else {
        ok("node: " + node + " " + nodeVersion);
    }
#if defined(__APPLE__)
    if (run({"xcrun", "-f", "metal"}).status != 0) {
        fix("no Metal toolchain, so the window cannot be built", "SETUP.md, Prerequisites: the Metal toolchain");
    } else {
        ok("the Metal toolchain");
    }
#endif
    // The homes.
    struct stat st{};
    if (stat(at.top.c_str(), &st) != 0) {
        fix(at.top + " is missing", "nexus-server setup");
    } else if ((st.st_mode & 077) != 0) {
        fix(at.top + " is open to others", "chmod 700 " + at.top);
    } else {
        ok(at.top + " (0700)");
    }
    if (stat(p.accounts.c_str(), &st) == 0 && (st.st_mode & 077) != 0) {
        fix(p.accounts + " is open to others, and the Claude driver refuses it", "chmod 700 " + p.accounts);
    }
    // The placed server and the running one.
    const std::string placedBuild = fileBuild(at.server);
    const std::string builtBuild = fileBuild(builtServer(self));
    const std::string rebuildCommand = build + "/nexus-server rebuild";
    if (placedBuild.empty()) {
        fix("the server is not placed at " + at.server, rebuildCommand);
    } else if (!builtBuild.empty() && builtBuild != placedBuild) {
        // Never setup straight from the build: rebuild tests it first, and setup refuses a build not tested.
        fix("the placed server (build " + placedBuild + ") is not the one built in " + build + " (build " + builtBuild +
            ")", rebuildCommand);
    } else {
        ok("the placed server: " + at.server + " (build " + placedBuild + ")");
    }
    // The handover: still running, or how the last one ended.
    const HandoverState last = readHandover(p);
    const bool handing = handoverRuns(last);
    if (handing) {
        ok("a handover to build " + last.build + " runs (pid " + std::to_string(last.pid) + ", its log " + p.dir +
            "/handover.log): the server drains until no friend's turn is open, then the placed build starts");
    } else if (last.state == "running") {
        fix("the handover to build " + last.build + " ended without saying how (its log: " + p.dir + "/handover.log)",
            rebuildCommand);
    } else if (last.state == "rolled-back") {
        fix("the build " + last.build + " did not come up on the store, so the previous build " + last.previous +
            " runs again (why: " + p.log + " and " + p.dir + "/handover.log)", "fix the cause in your fork, then " +
            rebuildCommand);
    } else if (last.state == "failed") {
        fix("the build " + last.build + " did not come up on the store, and no previous build that ran was there to "
            "put back (why: " + p.log + " and " + p.dir + "/handover.log)", "fix the cause in your fork, then " +
            rebuildCommand);
    }
    const std::string target = runningBuild(placedBuild, pinnedDriver(at.server, p.home));
    const Seen seen = look(p, target);
    const char* const again = handing ? "wait for the handover above" : "nexus-server setup";
    switch (nextStep(seen)) {
        case Step::Done: ok("the server runs the placed build, on " + p.socket); break;
        case Step::Start: fix("no server answers on " + p.socket, again); break;
        case Step::AskStop: fix("the server running is from before takeover", again); break;
        case Step::AskDrain: fix("the server runs build " + seen.build + ", not the placed one", again);
            break;
        case Step::Wait: ok("the server is draining, to hand over to the placed build"); break;
    }
    std::string oldReply;
    if (p.dir != at.top && ask(at.top + "/server.sock", "build", &oldReply)) {
        fix("a server of the layout before " + p.dir + " still runs on " + at.top + "/server.sock",
            "nexus-server setup");
    }
    if (serviceLoaded(at)) {
        ok("the service " + at.serviceName + " keeps the server running");
    } else {
        fix("no service keeps the server running across logout and reboot", "nexus-server setup");
    }
#if !defined(__APPLE__)
    const Ran linger = run({"loginctl", "show-user", uidText(), "-p", "Linger"});
    if (trimmed(linger.out) == "Linger=yes") {
        ok("lingering: the user's services run across logout and from boot");
    } else {
        fix("the user's services stop at the last logout", "loginctl enable-linger");
    }
#endif
    if (isDir(p.dir + "/nexus-config/.git")) {
        ok("the accounts and friends: " + p.dir + "/nexus-config");
    } else {
        fix(p.dir + "/nexus-config is not made", "nexus-server setup");
    }
    // The drivers.
    std::string table;
    if (!readFile(p.drivers, &table)) {
        fix("no drivers file, so no friend has a driver", "nexus-server setup");
    } else {
        std::istringstream lines(table);
        for (std::string line; std::getline(lines, line);) {
            std::istringstream words(line);
            std::string kind;
            std::string harness;
            std::string program;
            std::string script;
            words >> kind >> harness >> program >> script;
            if (kind != "driver") {
                continue;
            }
            if (access(program.c_str(), X_OK) != 0) {
                fix("the drivers file runs " + harness + " under " + program + ", which is not there",
                    "in " + p.drivers + ", put the full path of a node 20.19 or newer (`command -v node`) in its place");
            } else if (!script.empty() && script[0] == '/' && !exists(script)) {
                fix("the drivers file runs " + harness + " from " + script + ", which is not there",
                    "nexus-server rebuild");
            } else if (script.rfind(source + "/", 0) == 0) {
                fix("the drivers file runs " + harness + " from your fork, which a rebuild rewrites under it",
                    "nexus-server setup");
            } else {
                ok("driver " + harness + ": " + program + " " + script);
            }
            // Its account's login: only the person gives it, in nexus's own dialog (docs/secrets.md), never an AI.
            for (std::string word, account; words >> word;) {
                if (word == "--account" && words >> account) {
                    if (exists(p.accounts + "/" + account + "/oauth-token")) {
                        ok("the account " + account + " has its login");
                    } else {
                        say("note", "the account " + account + " has no login yet: the person signs it in in nexus's "
                            "own dialog, which asks for it when a session of its friend first starts the driver (SETUP.md, Not yet); "
                            "no AI handles it");
                    }
                }
            }
        }
    }
#if defined(__APPLE__)
    // The window.
    if (!isDir(at.app)) {
        fix("the window is not placed at " + at.app, "nexus-server setup");
    } else if (run({"/usr/bin/xattr", "-p", "com.apple.quarantine", at.app}).status == 0) {
        fix(at.app + " carries a quarantine flag, so Gatekeeper would ask", "xattr -dr com.apple.quarantine " + at.app);
    } else {
        ok("the window: " + at.app + ", no quarantine flag");
    }
    std::string clientConfig;
    readFile(at.client + "/config", &clientConfig);
    if (!hasKey(clientConfig, "server")) {
        fix("the window's config names no server, so it opens on none", "nexus-server setup");
    } else {
        ok("the window's config names its server");
    }
#endif
    std::printf("doctor: %s\n", missing == 0 ? "nothing to fix" :
        (std::to_string(missing) + " to fix, each with its command above").c_str());
    return missing == 0 ? 0 : 1;
}

// ---- rebuild ------------------------------------------------------------------------------------------------------

int rebuild(const Paths& p, const std::string& self, const std::vector<std::string>& args) {
    (void)self;
    signal(SIGPIPE, SIG_IGN);
    std::vector<std::string> setupArgs;
    unsigned jobCount = std::max(1u, std::thread::hardware_concurrency());
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-service") {
            setupArgs.push_back(args[i]);
        } else if (args[i] == "--wait" && i + 1 < args.size()) {
            setupArgs.push_back(args[i]);
            setupArgs.push_back(args[++i]);
        } else if (args[i] == "--jobs" && i + 1 < args.size() && std::atoi(args[i + 1].c_str()) > 0) {
            jobCount = static_cast<unsigned>(std::atoi(args[++i].c_str()));
        } else {
            std::fprintf(stderr, "usage: nexus-server rebuild [--jobs <n>] [--no-service] [--wait <seconds>] "
                "[--dir DIR]\n");
            return 2;
        }
    }
    const std::string source = NEXUS_SOURCE_DIR;
    const std::string build = NEXUS_BUILD_DIR;
    // A handover still running holds this build's program: it is not rebuilt under it.
    const HandoverState handing = readHandover(p);
    if (handoverRuns(handing)) {
        say("FAIL", "a handover to build " + handing.build + " still runs (pid " + std::to_string(handing.pid) +
            "): it ends when the running server's open turns end (at most " + std::to_string(kDrainMs / 60000) +
            " minutes). `nexus-server doctor` says when it has ended; then rebuild again. Nothing was changed");
        return 1;
    }
    // The mark that this build passed its tests goes first, and comes back only when they pass (step 3).
    const std::string marker = build + "/nexus-server.tested";
    if (exists(marker) && unlink(marker.c_str()) != 0) {
        say("FAIL", "cannot remove " + marker + ": " + std::strerror(errno) + "; nothing was changed");
        return 1;
    }
    const std::string jobs = std::to_string(jobCount);
    // A node the AI fetched to ~/.nexus/node (SETUP.md, Prerequisites) comes first on PATH, so npm is found from inside
    // a friend's turn too, where the driver's PATH may not hold it.
    const std::string nodeBin = placement(p.home, userHome()).top + "/node/bin";
    // Only once: a PATH that already holds it is left as it is, so that setup writes the same service file each time.
    const char* path = std::getenv("PATH");
    const std::string pathNow = path != nullptr ? path : "";
    if (access((nodeBin + "/npm").c_str(), X_OK) == 0 && (":" + pathNow + ":").find(":" + nodeBin + ":") ==
            std::string::npos) {
        const std::string joined = nodeBin + (pathNow.empty() ? std::string() : ":" + pathNow);
        setenv("PATH", joined.c_str(), 1);
    }
    say("build", "your fork " + source + ", in " + build);
    if (run({NEXUS_CMAKE, "--build", build, "-j", jobs}, false).status != 0) {
        say("FAIL", "the build failed (its errors are above); nothing that runs was changed");
        return 1;
    }
    const std::string driver = source + "/drivers/claude";
    if (isDir(driver)) {
        // npm ci when the modules are missing or older than the lock (an upstream merge that changed dependencies).
        struct stat lock{};
        struct stat kept{};
        const bool stale = stat((driver + "/node_modules/.package-lock.json").c_str(), &kept) != 0 ||
            (stat((driver + "/package-lock.json").c_str(), &lock) == 0 && lock.st_mtime > kept.st_mtime);
        if (stale && run({"npm", "ci", "--no-audit", "--no-fund", "--prefix", driver}, false).status != 0) {
            say("FAIL", "npm ci failed in " + driver + "; nothing that runs was changed");
            return 1;
        }
        if (run({"npm", "run", "build", "--prefix", driver}, false).status != 0) {
            say("FAIL", "the Claude driver's build failed in " + driver + "; nothing that runs was changed");
            return 1;
        }
    }
    say("test", "the fast tests: ctest -L unit, and the Claude driver's");
    if (run({NEXUS_CTEST, "--test-dir", build, "-L", "unit", "-j", jobs, "--output-on-failure"}, false).status != 0) {
        say("FAIL", "a unit test failed (above); nothing that runs was changed");
        return 1;
    }
    if (isDir(driver) && run({"npm", "test", "--prefix", driver}, false).status != 0) {
        say("FAIL", "a Claude driver unit test failed (above); nothing that runs was changed");
        return 1;
    }
    std::string why;
    const std::string program = build + "/nexus-server";
    const std::string tested = fileBuild(program);
    if (tested.empty() || !writeFile(marker, tested + "\n", 0644, &why)) {
        say("FAIL", "the tests passed, but " + marker + " was not written (" + (tested.empty() ? "cannot read " +
            program : why) + "); nothing that runs was changed");
        return 1;
    }
    say("ok", "the build " + tested + " passed its tests: " + marker);
    // The new build's own setup: what it places, and how, is the new code's.
    std::vector<std::string> argv = {build + "/nexus-server", "setup", "--dir", p.dir};
    if (std::find(setupArgs.begin(), setupArgs.end(), "--wait") == setupArgs.end()) {
        // From inside a friend's turn the server cannot drain until that turn ends: do not wait for it.
        argv.push_back("--wait");
        argv.push_back("5");
    }
    argv.insert(argv.end(), setupArgs.begin(), setupArgs.end());
    // setup's own exit: 0 handed over, 3 the handover finishing in the background, 1 a problem it named.
    const int status = run(argv, false).status;
    return status == 0 || status == 3 ? status : 1;
}

} // namespace nexus::server
