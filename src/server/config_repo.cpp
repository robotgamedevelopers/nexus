// nexus-server's config repository (config_repo.h).

#include "server/config_repo.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
    "!friends.json\n";

std::string errnoText(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// The file's text, or empty when it is missing. False on any other failure.
bool readFile(const std::string& path, std::string* out, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out->clear();
        if (errno == ENOENT) {
            return true;
        }
        *why = errnoText("cannot read " + path);
        return false;
    }
    std::ostringstream all;
    all << in.rdbuf();
    *out = all.str();
    return true;
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
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fchmod(fd, 0600) != 0 || ::fsync(fd) != 0 || ::close(fd) != 0) {
        *why = errnoText("cannot finish " + tmp);
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        *why = errnoText("cannot rename " + tmp + " to " + path);
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
    return what + " failed" + (r.status < 0 ? std::string(" (git could not be run; put git on PATH)") :
        " (git: " + trimmed(r.err.empty() ? r.out : r.err) + ")");
}

} // namespace

GitResult runGit(const std::string& dir, const std::vector<std::string>& args) {
    GitResult r;
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};
    if (::pipe(out) != 0 || ::pipe(err) != 0) {
        r.err = errnoText("pipe");
        return r;
    }
    std::vector<std::string> full = {"git", "-C", dir, "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
        "-c", "user.name=nexus", "-c", "user.email=nexus@localhost", "-c", "init.defaultBranch=main"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : full) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> env = {"GIT_CONFIG_NOSYSTEM=1", "GIT_TERMINAL_PROMPT=0", "LC_ALL=C"};
    for (const char* name : {"PATH", "HOME"}) {
        if (const char* v = std::getenv(name)) {
            env.push_back(std::string(name) + "=" + v);
        }
    }
    std::vector<char*> envp;
    for (std::string& e : env) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&files, out[1], 1);
    posix_spawn_file_actions_adddup2(&files, err[1], 2);
    posix_spawn_file_actions_addclose(&files, out[0]);
    posix_spawn_file_actions_addclose(&files, err[0]);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, "git", &files, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&files);
    ::close(out[1]);
    ::close(err[1]);
    if (rc != 0) {
        ::close(out[0]);
        ::close(err[0]);
        r.err = std::string("cannot run git: ") + std::strerror(rc);
        return r;
    }
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    int open = 2;
    char buf[4096];
    while (open > 0) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || fds[i].revents == 0) {
                continue;
            }
            const ssize_t n = ::read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                (i == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }
    for (pollfd& p : fds) {
        if (p.fd >= 0) {
            ::close(p.fd);
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

ConfigRepo::ConfigRepo(std::string dir) : dir_(std::move(dir)) {}

bool ConfigRepo::open(std::string* why) {
    struct stat st{};
    if (::stat(dir_.c_str(), &st) != 0) {
        if (::mkdir(dir_.c_str(), 0700) != 0) {
            *why = errnoText("cannot make " + dir_);
            return false;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        *why = dir_ + " is not a directory; move it aside, and nexus-server makes its config repository there";
        return false;
    }
    if (::stat((dir_ + "/.git").c_str(), &st) == 0) {
        return true;
    }
    GitResult g = runGit(dir_, {"init", "-q"});
    if (g.status != 0) {
        *why = gitWhy("making the config repository in " + dir_, g);
        return false;
    }
    if (!writeAtomic(dir_ + "/.gitignore", kIgnore, why)) {
        return false;
    }
    std::vector<std::string> add = {"add", "--", ".gitignore"};
    accounts::Registry found;
    std::vector<std::string> errors;
    std::string a;
    std::string f;
    if (readFile(accountsPath(), &a, why) && readFile(friendsPath(), &f, why) && (!a.empty() || !f.empty()) &&
            working(&found, &errors)) {
        add.push_back("accounts.json");
        add.push_back("friends.json");
        if (!writeAtomic(accountsPath(), accounts::accountsText(found), why) ||
                !writeAtomic(friendsPath(), accounts::friendsText(found), why)) {
            return false;
        }
    }
    if ((g = runGit(dir_, add)).status != 0 ||
            (g = runGit(dir_, {"commit", "-q", "-m", "nexus-server made its config repository"})).status != 0) {
        *why = gitWhy("the config repository's first commit", g);
        return false;
    }
    return true;
}

bool ConfigRepo::readAt(const std::string& rev, accounts::Registry* out, std::vector<std::string>* errors,
                        bool* upgraded) {
    const std::size_t had = errors->size();   // the caller's, not this read's
    std::string texts[2];
    const char* names[2] = {"accounts.json", "friends.json"};
    for (int i = 0; i < 2; ++i) {
        std::string why;
        if (rev.empty()) {
            if (!readFile(dir_ + "/" + names[i], &texts[i], &why)) {
                errors->push_back(why);
                return false;
            }
            continue;
        }
        GitResult g = runGit(dir_, {"cat-file", "-e", rev + ":" + names[i]});
        if (g.status != 0) {
            continue;   // not in that commit: none
        }
        g = runGit(dir_, {"show", rev + ":" + names[i]});
        if (g.status != 0) {
            errors->push_back(gitWhy("reading " + std::string(names[i]) + " at " + rev, g));
            return false;
        }
        texts[i] = g.out;
    }
    const std::string where = rev.empty() ? dir_ + "/" : rev + ":";
    accounts::Read ra = accounts::readAccounts(texts[0], where + names[0], out);
    accounts::Read rf = accounts::readFriends(texts[1], where + names[1], out);
    errors->insert(errors->end(), ra.errors.begin(), ra.errors.end());
    errors->insert(errors->end(), rf.errors.begin(), rf.errors.end());
    if (ra.ok && rf.ok) {
        const std::vector<std::string> p = accounts::problems(*out);
        errors->insert(errors->end(), p.begin(), p.end());
    }
    if (upgraded != nullptr) {
        *upgraded = ra.upgraded || rf.upgraded;
    }
    return errors->size() == had;
}

bool ConfigRepo::committed(accounts::Registry* out, std::vector<std::string>* report) {
    bool upgraded = false;
    if (!readAt("HEAD", out, report, &upgraded)) {
        return false;
    }
    if (upgraded) {
        // An older version, read as this one: written in this version, committed, so that it is never read old again.
        std::string why;
        const bool was = applying_;
        applying_ = true;
        const bool ok = keep(*out, "upgrade: the files written in version " + std::to_string(accounts::kVersion) +
                             ", the person's entries kept as they were", &why);
        applying_ = was;
        report->push_back(ok ? "upgraded accounts.json and friends.json to version " +
                          std::to_string(accounts::kVersion) : "the upgrade was not committed: " + why);
    }
    return true;
}

bool ConfigRepo::working(accounts::Registry* out, std::vector<std::string>* errors) {
    return readAt(std::string(), out, errors, nullptr);
}

bool ConfigRepo::at(const std::string& commit, accounts::Registry* out, std::vector<std::string>* errors) {
    const GitResult g = runGit(dir_, {"rev-parse", "--verify", "-q", commit + "^{commit}"});
    if (g.status != 0) {
        errors->push_back("no commit " + commit + " in " + dir_ + "; `nexus-server config history` lists them");
        return false;
    }
    return readAt(trimmed(g.out), out, errors, nullptr);
}

bool ConfigRepo::pending(std::string* why) {
    const GitResult g = runGit(dir_, {"status", "--porcelain", "--", "accounts.json", "friends.json"});
    if (g.status != 0) {
        *why = gitWhy("reading the config repository's status", g);
        return true;
    }
    if (!trimmed(g.out).empty()) {
        *why = "accounts.json or friends.json in " + dir_ + " holds hand edits not yet applied, so nothing was "
            "written over them: run `nexus-server config check`, then `nexus-server config reload` to apply them (or "
            "`git -C " + dir_ + " checkout -- .` to drop them)";
        return true;
    }
    return false;
}

bool ConfigRepo::keep(const accounts::Registry& registry, const std::string& message, std::string* why) {
    notes_.clear();
    if (!applying_ && pending(why)) {
        return false;
    }
    if (!writeAtomic(accountsPath(), accounts::accountsText(registry), why) ||
            !writeAtomic(friendsPath(), accounts::friendsText(registry), why)) {
        return false;
    }
    GitResult g = runGit(dir_, {"add", "--", "accounts.json", "friends.json"});
    if (g.status != 0) {
        *why = gitWhy("staging accounts.json and friends.json", g);
        return false;
    }
    if (runGit(dir_, {"diff", "--cached", "--quiet"}).status == 0) {
        return true;    // the files already said this
    }
    g = runGit(dir_, {"commit", "-q", "-m", message});
    if (g.status != 0) {
        *why = gitWhy("committing accounts.json and friends.json", g);
        return false;
    }
    push();
    return true;
}

bool ConfigRepo::onlyKnownTracked(std::string* why) {
    const GitResult g = runGit(dir_, {"ls-files"});
    if (g.status != 0) {
        *why = gitWhy("listing the config repository's files", g);
        return false;
    }
    std::istringstream in(g.out);
    std::string line;
    while (std::getline(in, line)) {
        bool known = false;
        for (const std::string& k : kConfigFiles) {
            known = known || line == k;
        }
        if (!known) {
            *why = dir_ + " tracks " + line + ", which is not one of its files (.gitignore, accounts.json, "
                "friends.json), so nothing is pushed: `git -C " + dir_ + " rm --cached " + line + "` and commit, then "
                "set the remote again";
            return false;
        }
    }
    return true;
}

void ConfigRepo::push() {
    std::string url;
    std::string why;
    if (!remote(&url, &why) || url.empty()) {
        return;
    }
    if (!onlyKnownTracked(&why)) {
        notes_.push_back("not pushed: " + why);
        return;
    }
    const GitResult g = runGit(dir_, {"push", "-q", "origin", "HEAD:main"});
    if (g.status != 0) {
        notes_.push_back(gitWhy("pushing to " + url + " (applied all the same; the next commit pushes again)", g));
    }
}

bool ConfigRepo::history(std::vector<std::string>* lines, std::string* why) {
    const GitResult g = runGit(dir_, {"log", "--date=iso-strict", "--format=%h %ad %s"});
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

bool ConfigRepo::remote(std::string* url, std::string* why) {
    const GitResult g = runGit(dir_, {"config", "--get", "remote.origin.url"});
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
    if (!onlyKnownTracked(why)) {
        return false;
    }
    std::string was;
    if (!remote(&was, why)) {
        return false;
    }
    const GitResult g = runGit(dir_, was.empty() ? std::vector<std::string>{"remote", "add", "origin", url}
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
    const GitResult g = runGit(dir_, {"remote", "remove", "origin"});
    if (g.status != 0) {
        *why = gitWhy("clearing the remote", g);
        return false;
    }
    return true;
}

std::string configBrief(const std::string& serverDir) {
    const std::string dir = serverDir + "/nexus-config";
    return "nexus-server's accounts and friends are two JSON files you may edit for the person: " + dir +
        "/accounts.json and " + dir + "/friends.json (schema: docs/accounts.md in the nexus repository). They are a "
        "git repository; never put a credential in them. After an edit run `nexus-server config check` (changes "
        "nothing, prints every error with file, line and fix), then `nexus-server config reload` (applies them and "
        "prints what it applied or refused). `nexus-server config history` lists the versions, `nexus-server config "
        "restore <commit>` brings one back, and `nexus-server config path` prints where the files are. From another "
        "machine, run the same commands over ssh to the server's host.";
}

} // namespace nexus::server
