// nexus-server's config repository (config_repo.h).

#include "server/config_repo.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern char** environ;

namespace nexus::server {

namespace {

const char* const kIgnore =
    "# nexus-server's config repository holds its known files and nothing else: never a credential.\n"
    "*\n"
    "!.gitignore\n"
    "!accounts.json\n"
    "!ais.json\n";

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string errnoText(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// The file's text, and whether it exists. False on any failure but a missing file.
bool readFile(const std::string& path, std::string* out, bool* exists, std::string* why) {
    out->clear();
    *exists = false;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return true;
        }
        *why = errnoText("cannot read " + path);
        return false;
    }
    *exists = true;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            *why = errnoText("cannot read " + path);
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        out->append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

bool readFile(const std::string& path, std::string* out, std::string* why) {
    bool exists = false;
    return readFile(path, out, &exists, why);
}

// Writes `text` to `path` atomically, mode 0600: a temporary file beside it, synced, renamed over it.
bool writeAtomic(const std::string& path, const std::string& text, std::string* why) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        *why = errnoText("cannot write " + tmp);
        return false;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            *why = errnoText("cannot write " + tmp);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fchmod(fd, 0600) != 0 || ::fsync(fd) != 0 || ::close(fd) != 0) {
        *why = errnoText("cannot finish " + tmp);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        *why = errnoText("cannot rename " + tmp + " to " + path);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::string gitWhy(const std::string& what, const GitResult& r) {
    return what + " failed" + (r.status < 0 && r.err.empty() ? std::string(" (git could not be run; put git on PATH)") :
        " (git: " + trimmed(r.err.empty() ? r.out : r.err) + ")");
}

std::string parentOf(const std::string& dir) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') {
        d.pop_back();
    }
    const std::size_t slash = d.rfind('/');
    return slash == std::string::npos ? std::string(".") : slash == 0 ? std::string("/") : d.substr(0, slash);
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// git, started on the repository in `dir` with the clean environment (config_repo.h), its output on two pipes, in a
// process group of its own (so that a kill at a deadline ends what it started too, ssh included). With `push`, the
// two credential sources a push may use. False, with why, when it could not be started.
bool spawnGit(const std::string& dir, const std::vector<std::string>& args, bool push, pid_t* pid, int* out, int* err,
              std::string* why) {
    int o[2] = {-1, -1};
    int e[2] = {-1, -1};
    if (::pipe(o) != 0) {
        *why = errnoText("pipe");
        return false;
    }
    if (::pipe(e) != 0) {
        *why = errnoText("pipe");
        ::close(o[0]);
        ::close(o[1]);
        return false;
    }
    std::vector<std::string> full = {"git", "-C", dir, "--git-dir=" + dir + "/.git", "--work-tree=" + dir,
        "-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false", "-c", "commit.gpgsign=false",
        "-c", "tag.gpgsign=false", "-c", "user.name=nexus", "-c", "user.email=nexus@localhost",
        "-c", "init.defaultBranch=main", "-c", "protocol.allow=never", "-c", "protocol.file.allow=always",
        "-c", "protocol.ssh.allow=always", "-c", "protocol.https.allow=always", "-c", "credential.helper="};
    if (push) {
        full.insert(full.end(), {"-c", "credential.helper=!gh auth git-credential"});
    }
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : full) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> env = {"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0",
        "GIT_ASKPASS=true", "SSH_ASKPASS=true", "SSH_ASKPASS_REQUIRE=never",
        "GIT_CEILING_DIRECTORIES=" + parentOf(dir), "LC_ALL=C"};
    std::vector<const char*> pass = {"PATH"};
    if (push) {
        // The push's credentials only: the user's ssh keys or agent, and the GitHub CLI's login (its config).
        env.push_back("GIT_SSH_COMMAND=ssh -o BatchMode=yes -o ConnectTimeout=15");
        pass.insert(pass.end(), {"HOME", "SSH_AUTH_SOCK", "XDG_CONFIG_HOME", "GH_CONFIG_DIR"});
    }
    for (const char* name : pass) {
        if (const char* v = std::getenv(name)) {
            env.push_back(std::string(name) + "=" + v);
        }
    }
    std::vector<char*> envp;
    for (std::string& x : env) {
        envp.push_back(x.data());
    }
    envp.push_back(nullptr);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&files, o[1], 1);
    posix_spawn_file_actions_adddup2(&files, e[1], 2);
    posix_spawn_file_actions_addclose(&files, o[0]);
    posix_spawn_file_actions_addclose(&files, e[0]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    const int rc = posix_spawnp(pid, "git", &files, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    ::close(o[1]);
    ::close(e[1]);
    if (rc != 0) {
        ::close(o[0]);
        ::close(e[0]);
        *why = std::string("cannot run git: ") + std::strerror(rc) + "; put git on PATH";
        return false;
    }
    *out = o[0];
    *err = e[0];
    return true;
}

// Reads what is there on `fd` into `into`; closes it at its end. False once it is closed.
bool drain(int* fd, std::string* into) {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(*fd, buf, sizeof buf);
        if (n > 0) {
            into->append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        ::close(*fd);
        *fd = -1;
        return false;
    }
}

int exitStatus(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

GitResult runGit(const std::string& dir, const std::vector<std::string>& args, int deadlineMs) {
    GitResult r;
    pid_t pid = -1;
    int fds[2] = {-1, -1};
    if (!spawnGit(dir, args, false, &pid, &fds[0], &fds[1], &r.err)) {
        return r;
    }
    setNonBlocking(fds[0]);
    setNonBlocking(fds[1]);
    const std::int64_t deadline = steadyMs() + deadlineMs;
    bool killed = false;
    while (fds[0] >= 0 || fds[1] >= 0) {
        pollfd p[2] = {{fds[0], POLLIN, 0}, {fds[1], POLLIN, 0}};
        const std::int64_t left = deadline - steadyMs();
        if (left <= 0) {
            ::kill(-pid, SIGKILL);
            killed = true;
            break;
        }
        if (::poll(p, 2, static_cast<int>(left)) < 0 && errno != EINTR) {
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i] >= 0 && p[i].revents != 0) {
                drain(&fds[i], i == 0 ? &r.out : &r.err);
            }
        }
    }
    for (int& fd : fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    r.status = killed ? -1 : exitStatus(status);
    if (killed) {
        r.err = "git did not finish within " + std::to_string(deadlineMs / 1000) + " s and was ended";
    }
    return r;
}

ConfigRepo::ConfigRepo(std::string dir) : dir_(std::move(dir)) {}

ConfigRepo::ConfigRepo(std::function<std::string()> dirNow) : dirNow_(std::move(dirNow)) {}

ConfigRepo::~ConfigRepo() {
    if (push_.pid > 0) {
        ::kill(-push_.pid, SIGKILL);
    }
    endPush();
}

bool ConfigRepo::open(std::string* why) {
    struct stat st{};
    if (::stat(dir().c_str(), &st) != 0) {
        if (::mkdir(dir().c_str(), 0700) != 0) {
            *why = errnoText("cannot make " + dir());
            return false;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        *why = dir() + " is not a directory; move it aside, and nexus-server makes its config repository there";
        return false;
    }
    if (::stat((dir() + "/.git").c_str(), &st) == 0) {
        return true;
    }
    GitResult g = runGit(dir(), {"init", "-q"});
    if (g.status != 0) {
        *why = gitWhy("making the config repository in " + dir(), g);
        return false;
    }
    if (!writeAtomic(dir() + "/.gitignore", kIgnore, why)) {
        return false;
    }
    std::vector<std::string> files = {".gitignore"};
    accounts::Registry found;
    std::vector<std::string> errors;
    std::string a;
    std::string f;
    if (readFile(accountsPath(), &a, why) && readFile(aisPath(), &f, why) && (!a.empty() || !f.empty()) &&
            working(&found, &errors)) {
        files.push_back("accounts.json");
        files.push_back("ais.json");
        if (!writeAtomic(accountsPath(), accounts::accountsText(found), why) ||
                !writeAtomic(aisPath(), accounts::aisText(found), why)) {
            return false;
        }
    }
    std::vector<std::string> add = {"add", "--"};
    add.insert(add.end(), files.begin(), files.end());
    std::vector<std::string> commit = {"commit", "-q", "-m", "nexus-server made its config repository", "--"};
    commit.insert(commit.end(), files.begin(), files.end());
    if ((g = runGit(dir(), add)).status != 0 || (g = runGit(dir(), commit)).status != 0) {
        *why = gitWhy("the config repository's first commit", g);
        return false;
    }
    return true;
}

bool ConfigRepo::hasAccounts() {
    return runGit(dir(), {"cat-file", "-e", "HEAD:accounts.json"}).status == 0;
}

bool ConfigRepo::readAt(const std::string& rev, accounts::Registry* out, std::vector<std::string>* errors,
                        bool* upgraded) {
    const std::size_t had = errors->size();   // the caller's, not this read's
    std::string texts[2];
    const char* names[2] = {"accounts.json", "ais.json"};
    for (int i = 0; i < 2; ++i) {
        std::string why;
        if (rev.empty()) {
            if (!readFile(dir() + "/" + names[i], &texts[i], &why)) {
                errors->push_back(why);
                return false;
            }
            continue;
        }
        GitResult g = runGit(dir(), {"cat-file", "-e", rev + ":" + names[i]});
        if (g.status != 0) {
            continue;   // not in that commit: none
        }
        g = runGit(dir(), {"show", rev + ":" + names[i]});
        if (g.status != 0) {
            errors->push_back(gitWhy("reading " + std::string(names[i]) + " at " + rev, g));
            return false;
        }
        texts[i] = g.out;
    }
    const std::string where = rev.empty() ? dir() + "/" : rev + ":";
    // The working files are read against what HEAD holds: an AI added by hand there, not yet committed, reads
    // limited even in a version 1 file (accounts::readAIs). HEAD unreadable: every AI reads as new.
    accounts::Registry head;
    if (rev.empty()) {
        std::vector<std::string> ignored;
        if (!readAt("HEAD", &head, &ignored, nullptr)) {
            head.ais.clear();
        }
    }
    accounts::Read ra = accounts::readAccounts(texts[0], where + names[0], out);
    accounts::Read rf = accounts::readAIs(texts[1], where + names[1], out, rev.empty() ? &head : nullptr);
    errors->insert(errors->end(), ra.errors.begin(), ra.errors.end());
    errors->insert(errors->end(), rf.errors.begin(), rf.errors.end());
    if (ra.ok && rf.ok) {
        const std::vector<std::string> p = accounts::problems(*out);
        errors->insert(errors->end(), p.begin(), p.end());
        if (p.empty()) {
            if (auto problem = sync::accountRosterProblem(*out); !problem.empty()) {
                errors->push_back(std::move(problem));
            }
        }
    }
    if (upgraded != nullptr) {
        *upgraded = ra.upgraded || rf.upgraded;
    }
    return errors->size() == had;
}

bool ConfigRepo::committed(accounts::Registry* out, std::vector<std::string>* report, bool write) {
    bool upgraded = false;
    if (!readAt("HEAD", out, report, &upgraded)) {
        return false;
    }
    if (upgraded && write) {
        std::string why;
        if (pending(&why)) {
            // Never written over: the person's edit stays as it is, and the next reload writes the new version.
            report->push_back("the files are an older version, read as version " + std::to_string(accounts::kVersion) +
                "; not rewritten now, because " + why);
        } else {
            const bool ok = keep(*out, "upgrade: the files written in version " + std::to_string(accounts::kVersion) +
                                 ", the person's entries kept as they were", &why);
            report->push_back(ok ? "upgraded accounts.json and ais.json to version " +
                              std::to_string(accounts::kVersion) : "the upgrade was not committed: " + why);
        }
    }
    return true;
}

bool ConfigRepo::working(accounts::Registry* out, std::vector<std::string>* errors) {
    return readAt(std::string(), out, errors, nullptr);
}

bool ConfigRepo::at(const std::string& commit, accounts::Registry* out, std::vector<std::string>* errors) {
    if (commit.empty() || commit[0] == '-') {
        errors->push_back("\"" + commit + "\" is not a commit; `nexus-server config history` lists them");
        return false;
    }
    const GitResult g = runGit(dir(), {"rev-parse", "--verify", "-q", commit + "^{commit}"});
    if (g.status != 0) {
        errors->push_back("no commit " + commit + " in " + dir() + "; `nexus-server config history` lists them");
        return false;
    }
    return readAt(trimmed(g.out), out, errors, nullptr);
}

bool ConfigRepo::restoring(const std::string& commit, accounts::Registry* out, std::vector<std::string>* errors) {
    std::string why;
    if (pending(&why)) {
        errors->push_back("no restore while hand edits are pending: " + why + "; then restore");
        return false;
    }
    return at(commit, out, errors);
}

bool ConfigRepo::pending(std::string* why) {
    const GitResult g = runGit(dir(), {"status", "--porcelain", "--", "accounts.json", "ais.json"});
    if (g.status != 0) {
        *why = gitWhy("reading the config repository's status", g);
        return true;
    }
    if (!trimmed(g.out).empty()) {
        *why = "accounts.json or ais.json in " + dir() + " holds hand edits not yet applied, so nothing was "
            "written over them: run `nexus-server config check`, then `nexus-server config reload` to apply them (or "
            "`git -C " + dir() + " checkout -- .` to drop them)";
        return true;
    }
    return false;
}

bool ConfigRepo::keep(const accounts::Registry& registry, const std::string& message, std::string* why) {
    notes_.clear();
    if (auto problem = sync::accountRosterProblem(registry); !problem.empty()) {
        if (why != nullptr) {
            *why = std::move(problem);
        }
        return false;
    }
    if (!applying_ && pending(why)) {
        return false;
    }
    // What the files hold now (the person's edit, while a reload applies it): put back if anything below fails.
    const std::string paths[2] = {accountsPath(), aisPath()};
    std::string before[2];
    bool existed[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        if (!readFile(paths[i], &before[i], &existed[i], why)) {
            return false;
        }
    }
    auto undo = [&](const std::string& failed) {
        for (int i = 0; i < 2; ++i) {
            std::string ignored;
            if (existed[i]) {
                writeAtomic(paths[i], before[i], &ignored);
            } else {
                ::unlink(paths[i].c_str());
            }
        }
        runGit(dir(), {"reset", "-q", "--", "accounts.json", "ais.json"});
        *why = failed + "; the files and the index were put back as they were, and nothing was committed";
        return false;
    };
    std::string failed;
    if (!writeAtomic(paths[0], accounts::accountsText(registry), &failed) ||
            !writeAtomic(paths[1], accounts::aisText(registry), &failed)) {
        return undo(failed);
    }
    GitResult g = runGit(dir(), {"add", "--", "accounts.json", "ais.json"});
    if (g.status != 0) {
        return undo(gitWhy("staging accounts.json and ais.json", g));
    }
    g = runGit(dir(), {"diff", "--cached", "--quiet", "--", "accounts.json", "ais.json"});
    if (g.status == 0) {
        return true;    // the files already said this
    }
    if (g.status != 1) {
        return undo(gitWhy("comparing accounts.json and ais.json with HEAD", g));
    }
    g = runGit(dir(), {"commit", "-q", "-m", message, "--", "accounts.json", "ais.json"});
    if (g.status != 0) {
        return undo(gitWhy("committing accounts.json and ais.json", g));
    }
    std::string url;
    std::string ignored;
    if (remote(&url, &ignored) && !url.empty()) {
        pushWanted_ = true;
        notes_.push_back("committed; pushing to " + url + " in the background (the server's log says how it went)");
    }
    return true;
}

namespace {
std::string adoptMark(const std::string& dir) {
    return dir + "/.git/nexus-adopting";
}
} // namespace

bool ConfigRepo::keepSeed(const accounts::Registry& seed, const std::string& message, std::string* why) {
    const std::string mark = adoptMark(dir());
    const int fd = ::open(mark.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || ::fsync(fd) != 0) {
        *why = "cannot write " + mark + ": " + std::strerror(errno);
        if (fd >= 0) {
            ::close(fd);
        }
        return false;
    }
    ::close(fd);
    if (!keep(seed, message, why)) {
        ::unlink(mark.c_str());
        return false;
    }
    return true;
}

bool ConfigRepo::adopting() const {
    return ::access(adoptMark(dir()).c_str(), F_OK) == 0;
}

bool ConfigRepo::adopted(std::string* why) {
    if (::unlink(adoptMark(dir()).c_str()) != 0 && errno != ENOENT) {
        *why = "cannot remove " + adoptMark(dir()) + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool ConfigRepo::onlyKnownHistory(std::string* why) {
    // Every path any commit touched, on every ref, merges included (each against each of its parents: a file added
    // while a merge was made by hand is in no other commit), and every path the index holds: a push publishes the
    // history.
    const GitResult log = runGit(dir(), {"-c", "core.quotePath=true", "log", "--all", "--diff-merges=separate",
                                        "--format=", "--name-only"});
    const GitResult index = runGit(dir(), {"-c", "core.quotePath=true", "ls-files"});
    if (log.status != 0 || index.status != 0) {
        *why = gitWhy("listing every file in the config repository's history", log.status != 0 ? log : index);
        return false;
    }
    std::istringstream in(log.out + "\n" + index.out);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || std::find(kConfigFiles.begin(), kConfigFiles.end(), line) != kConfigFiles.end()) {
            continue;
        }
        *why = "the history of " + dir() + " holds " + line + ", which is not one of its files (.gitignore, "
            "accounts.json, ais.json), so nothing is pushed: a push would publish it, whatever the index says now. "
            "Make a new repository (move " + dir() + " aside, start nexus-server, which makes a new one, and copy "
            "accounts.json and ais.json into it, then `nexus-server config reload`), or rewrite this one's history "
            "so that no commit holds " + line + "; if it was a credential, replace that credential";
        return false;
    }
    return true;
}

bool ConfigRepo::pushStart(std::int64_t nowMs, std::string* why) {
    pushWanted_ = false;
    if (push_.pid > 0) {
        pushWanted_ = true;     // one at a time: this commit is pushed by the next
        *why = "a push is still running";
        return false;
    }
    std::string raw;
    if (!rawRemote(&raw, why)) {
        return false;
    }
    if (raw.empty()) {
        *why = "no remote is set";
        return false;
    }
    if (!onlyKnownHistory(why)) {
        return false;
    }
    Child c;
    if (!spawnGit(dir(), {"push", "-q", "origin", "HEAD:main"}, true, &c.pid, &c.out, &c.err, why)) {
        return false;
    }
    setNonBlocking(c.out);
    setNonBlocking(c.err);
    c.deadline = nowMs + kPushMs;
    c.url = remoteShown(raw);
    c.raw = raw;
    push_ = std::move(c);
    return true;
}

std::vector<int> ConfigRepo::pushFds() const {
    std::vector<int> fds;
    for (int fd : {push_.out, push_.err}) {
        if (fd >= 0) {
            fds.push_back(fd);
        }
    }
    return fds;
}

int ConfigRepo::pushWaitMs(std::int64_t nowMs) const {
    if (push_.pid <= 0) {
        return -1;
    }
    if (push_.out < 0 && push_.err < 0) {
        return 20;      // its pipes are closed: it is exiting
    }
    return static_cast<int>(std::max<std::int64_t>(0, push_.deadline - nowMs));
}

void ConfigRepo::endPush() {
    for (int* fd : {&push_.out, &push_.err}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    if (push_.pid > 0) {
        int status = 0;
        while (::waitpid(push_.pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    push_ = Child{};
}

bool ConfigRepo::pushServe(std::int64_t nowMs, std::string* result) {
    if (push_.pid <= 0) {
        return false;
    }
    if (push_.out >= 0) {
        drain(&push_.out, &push_.outText);
    }
    if (push_.err >= 0) {
        drain(&push_.err, &push_.errText);
    }
    const std::string url = push_.url;
    const std::string raw = push_.raw;
    const std::string again = "; the change stays committed here, and the next commit pushes again";
    if (nowMs >= push_.deadline) {
        ::kill(-push_.pid, SIGKILL);
        endPush();
        *result = "pushing to " + url + " did not finish within " + std::to_string(kPushMs / 1000) +
            " s and was ended" + again;
        return true;
    }
    if (push_.out >= 0 || push_.err >= 0) {
        return false;
    }
    int status = 0;
    pid_t done = 0;
    while ((done = ::waitpid(push_.pid, &status, WNOHANG)) < 0 && errno == EINTR) {
    }
    if (done == 0) {
        return false;
    }
    GitResult g;
    g.status = done < 0 ? -1 : exitStatus(status);
    g.out = push_.outText;
    g.err = push_.errText;
    push_.pid = -1;     // collected
    endPush();
    *result = g.status == 0 ? "pushed to " + url : gitWhy("pushing to " + url, g) + again;
    // What git said may name the url as it holds it: never with its credential.
    if (raw != url && !raw.empty()) {
        for (std::size_t at = result->find(raw); at != std::string::npos; at = result->find(raw, at + url.size())) {
            result->replace(at, raw.size(), url);
        }
    }
    return true;
}

bool ConfigRepo::history(std::vector<std::string>* lines, std::string* why) {
    const GitResult g = runGit(dir(), {"log", "--date=iso-strict", "--format=%h %ad %s"});
    if (g.status != 0) {
        *why = gitWhy("reading the config repository's history", g);
        return false;
    }
    std::istringstream in(g.out);
    std::string line;
    while (std::getline(in, line)) {
        lines->push_back(line);
    }
    return true;
}

namespace {

// A url's user part: [*begin, *end), its '@' included. False when it has none. With a scheme, the user part is before
// the host, in the part up to the first '/', '?' or '#' after "://"; without one (scp-like, `user@host:path`, or a
// path), in the part up to the first '/'.
bool userPart(const std::string& url, std::size_t* begin, std::size_t* end, bool* ssh) {
    const std::size_t scheme = url.find("://");
    std::size_t from = 0;
    std::size_t to = 0;
    if (scheme == std::string::npos) {
        to = url.find('/');
        *ssh = true;
    } else {
        from = scheme + 3;
        to = url.find_first_of("/?#", from);
        std::string kind = url.substr(0, scheme);
        std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        *ssh = kind == "ssh" || kind == "git+ssh" || kind == "ssh+git";
    }
    if (to == std::string::npos) {
        to = url.size();
    }
    const std::size_t at = url.rfind('@', to == 0 ? 0 : to - 1);
    if (at == std::string::npos || at < from || at >= to) {
        return false;
    }
    *begin = from;
    *end = at + 1;
    return true;
}

} // namespace

bool remoteCredential(const std::string& url) {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool ssh = false;
    if (!userPart(url, &begin, &end, &ssh)) {
        return false;
    }
    return !ssh || url.substr(begin, end - begin).find(':') != std::string::npos;
}

std::string remoteShown(const std::string& url) {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool ssh = false;
    if (!remoteCredential(url) || !userPart(url, &begin, &end, &ssh)) {
        return url;
    }
    return url.substr(0, begin) + url.substr(end);
}

bool ConfigRepo::remote(std::string* url, std::string* why) {
    if (!rawRemote(url, why)) {
        return false;
    }
    *url = remoteShown(*url);
    return true;
}

bool ConfigRepo::rawRemote(std::string* url, std::string* why) {
    const GitResult g = runGit(dir(), {"config", "--get", "remote.origin.url"});
    if (g.status == 1) {
        url->clear();
        return true;
    }
    if (g.status != 0) {
        *why = gitWhy("reading the config repository's remote", g);
        return false;
    }
    *url = trimmed(g.out);
    return true;
}

bool ConfigRepo::setRemote(const std::string& url, std::string* why) {
    if (url.empty() || url[0] == '-') {
        *why = "a remote is a git url, as git@github.com:<owner>/nexus-config.git";
        return false;
    }
    if (remoteCredential(url)) {
        *why = "the remote " + remoteShown(url) + " was given with a user or password part, so it was not set: "
            "credentials never go in the remote URL; nexus pushes with the machine's git credentials. Set it without "
            "them (as git@github.com:<owner>/nexus-config.git, or https://github.com/<owner>/nexus-config.git), and "
            "if that part was a token, replace the token";
        return false;
    }
    if (!onlyKnownHistory(why)) {
        return false;
    }
    std::string was;
    if (!remote(&was, why)) {
        return false;
    }
    const GitResult g = runGit(dir(), was.empty() ? std::vector<std::string>{"remote", "add", "origin", url}
                                                 : std::vector<std::string>{"remote", "set-url", "origin", url});
    if (g.status != 0) {
        *why = gitWhy("setting the remote", g);
        return false;
    }
    return true;
}

bool ConfigRepo::clearRemote(std::string* why) {
    std::string was;
    if (!remote(&was, why)) {
        return false;
    }
    if (was.empty()) {
        return true;
    }
    const GitResult g = runGit(dir(), {"remote", "remove", "origin"});
    if (g.status != 0) {
        *why = gitWhy("clearing the remote", g);
        return false;
    }
    pushWanted_ = false;
    return true;
}

std::string configBrief(const std::string& serverDir) {
    const std::string dir = serverDir + "/nexus-config";
    return "nexus-server's accounts and AIs are two JSON files you may edit for the person: " + dir +
        "/accounts.json and " + dir + "/ais.json (schema: docs/accounts.md in the nexus repository). They are a "
        "git repository; never put a credential in them. After an edit run `nexus-server config check` (changes "
        "nothing, prints every error with file, line and fix), then `nexus-server config reload` (applies them and "
        "prints what it applied or refused). `nexus-server config history` lists the versions, `nexus-server config "
        "restore <commit>` brings one back, and `nexus-server config path` prints where the files are. From another "
        "machine, run the same commands over ssh to the server's host.";
}

} // namespace nexus::server
