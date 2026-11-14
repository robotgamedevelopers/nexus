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
    fsync(fd);
    fchmod(fd, mode);
    close(fd);
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
bool installService(const Paths& p, const Placement& at, std::string* done, bool* changed, std::string* why) {
    const char* path = std::getenv("PATH");
    const std::string pathText = path != nullptr ? path : "/usr/bin:/bin";
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
    if (at.serviceFile != own) {
        run({"systemctl", "--user", "link", at.serviceFile});
    }
    run({"systemctl", "--user", "daemon-reload"});
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

bool startService(const Placement& at) {
#if defined(__APPLE__)
    return run({"launchctl", "kickstart", launchdDomain() + "/" + at.serviceName}).status == 0;
#else
    return run({"systemctl", "--user", "start", at.serviceName + ".service"}).status == 0;
#endif
}

// Starts `program serve --dir <dir>` detached, as `new` does, when there is no service.
bool startDetached(const Paths& p, const std::string& program) {
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&files, 1, p.log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&files, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(__APPLE__)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawn_file_actions_addclosefrom_np(&files, 3);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    std::vector<std::string> args = {program, "serve", "--dir", p.dir};
    std::vector<char*> raw;
    for (std::string& a : args) {
        raw.push_back(a.data());
    }
    raw.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, program.c_str(), &files, &attr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    return rc == 0;
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

// Puts a copy of the tree `from` at `to` in one rename: the new tree beside it, the old moved aside, then removed.
bool placeTree(const std::string& from, const std::string& to, std::string* why) {
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
           "driver claude " + node + " " + driverMain + " --account main\n";
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

// ---- setup --------------------------------------------------------------------------------------------------------

int setup(const Paths& p, const std::string& self, const std::vector<std::string>& args) {
    signal(SIGPIPE, SIG_IGN);
    bool service = true;
    int waitSeconds = 30;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-service") {
            service = false;
        } else if (args[i] == "--wait" && i + 1 < args.size()) {
            waitSeconds = std::atoi(args[++i].c_str());
        } else {
            std::fprintf(stderr, "usage: nexus-server setup [--no-service] [--wait <seconds>] [--dir DIR]\n");
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

    // 2. The server, placed.
    const std::string built = self == at.server ? builtServer(self) : self;
    std::string bytes;
    if (!readFile(built, &bytes)) {
        fail("cannot read " + built + ", the server to place");
        return 1;
    }
    const std::string placedBuild = buildOf(bytes);
    if (fileBuild(at.server) == placedBuild) {
        say("ok", "the server is placed: " + at.server + " (build " + placedBuild + ")");
    } else if (writeFile(at.server, bytes, 0755, &why)) {
        say("done", "the server is placed: " + at.server + " (build " + placedBuild + ", from " + built + ")");
    } else {
        fail(why);
        return 1;
    }
    // A link where a login shell finds it, for a window on another machine (`--server <this machine>` runs
    // `nexus-server new` over ssh). An older program there is moved aside, never removed.
    {
        char target[4096] = {};
        const ssize_t n = readlink(at.localBin.c_str(), target, sizeof target - 1);
        if (n > 0 && std::string(target, static_cast<std::size_t>(n)) == at.server) {
            say("ok", at.localBin + " links to the placed server");
        } else {
            const std::size_t cut = at.localBin.find_last_of('/');
            if (makeDirs(at.localBin.substr(0, cut), 0755, &why)) {
                if (exists(at.localBin)) {
                    const std::string aside = at.localBin + ".before-setup";
                    rename(at.localBin.c_str(), aside.c_str());
                    say("moved", "the older " + at.localBin + " to " + aside);
                }
                if (symlink(at.server.c_str(), at.localBin.c_str()) == 0) {
                    say("done", at.localBin + " links to the placed server");
                }
            }
        }
    }

    // 3. The Claude driver, placed: dist always, node_modules when the lock changed.
    const std::string fork = source + "/drivers/claude";
    if (isDir(fork + "/dist") && isDir(fork + "/node_modules")) {
        std::string lockNew;
        std::string lockOld;
        readFile(fork + "/package-lock.json", &lockNew);
        readFile(at.driver + "/package-lock.json", &lockOld);
        bool ok = makeDirs(at.driver, 0700, &why);
        if (ok && (lockNew != lockOld || !isDir(at.driver + "/node_modules"))) {
            ok = placeTree(fork + "/node_modules", at.driver + "/node_modules", &why);
        }
        ok = ok && placeTree(fork + "/dist", at.driver + "/dist", &why);
        std::string pkg;
        ok = ok && readFile(fork + "/package.json", &pkg) && writeFile(at.driver + "/package.json", pkg, 0644, &why) &&
            writeFile(at.driver + "/package-lock.json", lockNew, 0644, &why);
        if (ok) {
            say("done", "the Claude driver is placed: " + at.driver);
        } else {
            fail("the Claude driver was not placed: " + why);
        }
    } else {
        fail("the Claude driver is not built in " + fork + ": run `npm ci && npm run build` there, then setup again");
    }

    // 4. The drivers file: the Claude driver on a first install; paths into the fork moved to the placed driver.
    std::string nodeVersion;
    const std::string node = findNode(at, &nodeVersion);
    std::string table;
    if (!readFile(p.drivers, &table)) {
        if (node.empty()) {
            fail("no node 20.19 or newer on PATH or in " + at.top + "/node, so no drivers file was written (SETUP.md, "
                "Node)");
        } else if (writeFile(p.drivers, firstDriversFile(node, at.driver + "/dist/src/main.js"), 0600, &why)) {
            say("done", "the drivers file " + p.drivers + " names the Claude driver, under " + node + " (" +
                nodeVersion + ")");
        } else {
            fail(why);
        }
    } else {
        const std::string moved = movedDriverPaths(table, source, at.driver);
        if (moved != table) {
            writeFile(p.drivers + ".before-setup", table, 0600, &why);
            if (writeFile(p.drivers, moved, 0600, &why)) {
                say("fixed", p.drivers + " ran the Claude driver from the fork; it runs the placed one now (the old "
                    "file is " + p.drivers + ".before-setup)");
            }
        } else {
            say("ok", "the drivers file " + p.drivers + " is kept as it is");
        }
    }

#if defined(__APPLE__)
    // 5. The window, placed as an app the person opens, and told to open on this machine's server.
    const std::string builtApp = std::string(NEXUS_BUILD_DIR) + "/nexus.app";
    if (access((builtApp + "/Contents/MacOS/nexus").c_str(), X_OK) == 0) {
        if (makeDirs(at.home + "/Applications", 0755, &why) && placeTree(builtApp, at.app, &why)) {
            // A local build carries no quarantine flag; one copied in from elsewhere would make Gatekeeper ask.
            if (run({"/usr/bin/xattr", "-p", "com.apple.quarantine", at.app}).status == 0) {
                run({"/usr/bin/xattr", "-dr", "com.apple.quarantine", at.app});
                say("fixed", at.app + " carried a quarantine flag; it is removed");
            }
            say("done", "the window is placed: " + at.app + " (the person opens it: Spotlight, Finder or Launchpad)");
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
        if (installService(p, at, &done, &changed, &why)) {
            say(changed ? "done" : "ok", done);
            haveService = true;
        } else {
            say("note", why);
        }
    }

    // 7. The takeover: the placed build running (server/takeover.h).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
    Step last = Step::Done;
    bool first = true;
    bool asked = false;
    for (;;) {
        const Seen seen = look(p, placedBuild);
        const Step step = nextStep(seen);
        if (first || step != last) {
            first = false;
            last = step;
            switch (step) {
                case Step::Done: say("ok", "the server runs the placed build " + placedBuild); break;
                case Step::Start: say("start", haveService ? "the service starts the server" : "the server starts"); break;
                case Step::AskDrain: say("drain", "the server runs build " + seen.build + "; it hands over to " +
                                         placedBuild + " once no friend's turn is open"); break;
                case Step::AskStop: say("stop", "the server is from before takeover; it is stopped"); break;
                case Step::Wait: break;
            }
        }
        if (step == Step::Done) {
            break;
        }
        if (step == Step::Start) {
            if (!(haveService && startService(at))) {
                startDetached(p, at.server);
            }
            asked = false;
        } else if (step == Step::AskDrain && !asked) {
            std::string reply;
            ask(p.socket, "takeover", &reply);
            asked = true;
        } else if (step == Step::AskStop) {
            std::string reply;
            ask(p.socket, "stop", &reply);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (step == Step::Wait || step == Step::AskDrain) {
                say("note", "the server is draining: it hands over to the placed build when its open turns end (at "
                    "most " + std::to_string(kDrainMs / 60000) + " minutes); nothing more to do");
            } else {
                fail("the server did not come up on the placed build within " + std::to_string(waitSeconds) +
                    " s; see " + p.log);
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 8. The accounts and friends' repository: the server makes it when it starts.
    if (isDir(p.dir + "/nexus-config/.git")) {
        say("ok", "the accounts and friends live in " + p.dir + "/nexus-config (docs/accounts.md)");
    } else if (last == Step::Done) {
        fail(p.dir + "/nexus-config was not made; see " + p.log);
    }
    std::printf("%s\n", problems == 0 ? "setup: done" : "setup: done, with the problems above");
    return problems == 0 ? 0 : 1;
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
    if (placedBuild.empty()) {
        fix("the server is not placed at " + at.server, "nexus-server setup");
    } else if (!builtBuild.empty() && builtBuild != placedBuild) {
        fix("the placed server (build " + placedBuild + ") is not the one built in " + build + " (build " + builtBuild +
            ")", build + "/nexus-server setup");
    } else {
        ok("the placed server: " + at.server + " (build " + placedBuild + ")");
    }
    const Seen seen = look(p, placedBuild);
    switch (nextStep(seen)) {
        case Step::Done: ok("the server runs the placed build, on " + p.socket); break;
        case Step::Start: fix("no server answers on " + p.socket, "nexus-server setup"); break;
        case Step::AskStop: fix("the server running is from before takeover", "nexus-server setup"); break;
        case Step::AskDrain: fix("the server runs build " + seen.build + ", not the placed one", "nexus-server setup");
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
                    "nexus-server setup (with node on PATH), or edit " + p.drivers);
            } else if (!script.empty() && script[0] == '/' && !exists(script)) {
                fix("the drivers file runs " + harness + " from " + script + ", which is not there",
                    "nexus-server rebuild");
            } else if (script.rfind(source + "/", 0) == 0) {
                fix("the drivers file runs " + harness + " from your fork, which a rebuild rewrites under it",
                    "nexus-server setup");
            } else {
                ok("driver " + harness + ": " + program + " " + script);
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
    bool tests = true;
    std::vector<std::string> setupArgs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-tests") {
            tests = false;
        } else if (args[i] == "--no-service") {
            setupArgs.push_back(args[i]);
        } else if (args[i] == "--wait" && i + 1 < args.size()) {
            setupArgs.push_back(args[i]);
            setupArgs.push_back(args[++i]);
        } else {
            std::fprintf(stderr, "usage: nexus-server rebuild [--no-tests] [--no-service] [--wait <seconds>] "
                "[--dir DIR]\n");
            return 2;
        }
    }
    const std::string source = NEXUS_SOURCE_DIR;
    const std::string build = NEXUS_BUILD_DIR;
    const std::string jobs = std::to_string(std::max(1u, std::thread::hardware_concurrency()));
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
    if (tests) {
        say("test", "the fast tests: ctest -L unit, and the Claude driver's");
        if (run({NEXUS_CTEST, "--test-dir", build, "-L", "unit", "-j", jobs, "--output-on-failure"}, false).status != 0) {
            say("FAIL", "a unit test failed (above); nothing that runs was changed");
            return 1;
        }
        if (isDir(driver) && run({"npm", "test", "--prefix", driver}, false).status != 0) {
            say("FAIL", "a Claude driver unit test failed (above); nothing that runs was changed");
            return 1;
        }
    }
    // The new build's own setup: what it places, and how, is the new code's.
    std::vector<std::string> argv = {build + "/nexus-server", "setup", "--dir", p.dir};
    if (std::find(setupArgs.begin(), setupArgs.end(), "--wait") == setupArgs.end()) {
        // From inside a friend's turn the server cannot drain until that turn ends: do not wait for it.
        argv.push_back("--wait");
        argv.push_back("5");
    }
    argv.insert(argv.end(), setupArgs.begin(), setupArgs.end());
    return run(argv, false).status == 0 ? 0 : 1;
}

} // namespace nexus::server
