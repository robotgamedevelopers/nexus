// nexus-server's config repository: ~/.nexus/server/nexus-config/, a git repository of its own that holds the files
// a person or an AI edits by hand, accounts.json and ais.json (accounts/registry.h, docs/accounts.md). Every
// change the server applies is one commit, whose message says what changed and who asked: an account action from a
// window, a hand edit applied by `nexus-server config reload`, an earlier version brought back by `config restore`, or
// an upgrade of an older file version. History is never rewritten: a restore is a new commit.
//
// What the server holds is what HEAD holds. A hand edit sits in the working files, uncommitted, until a reload checks
// it and applies it (committed as the person's edit), so a half-finished edit is never applied; while one sits there,
// nothing else writes the files: an account action from a window, a restore and an upgrade are refused or held.
//
// Secrets never enter it: the credentials stay in ~/.nexus/server/accounts/<name>/, outside this directory; its
// .gitignore admits only its known files; a commit names those files and nothing else; and before any push every path
// in the repository's history is checked to be one of them.
//
// A remote is optional (`nexus-server config remote <url>`, a private repository of the person's own, suggested name
// nexus-config). With one, every commit asks for a push, which runs as a child process the server's loop watches,
// never on the loop itself, with a hard deadline: the apply is reported as committed at once, and the push's result
// goes to the server's log when it ends. A push that fails never blocks or undoes the apply; the next commit pushes
// again.
//
// git runs as a child process with a clean environment: no system or global config (GIT_CONFIG_NOSYSTEM,
// GIT_CONFIG_GLOBAL=/dev/null), no prompts (GIT_TERMINAL_PROMPT=0, a no-op GIT_ASKPASS and SSH_ASKPASS), never above
// this directory (GIT_CEILING_DIRECTORIES, an explicit --git-dir and --work-tree), no hooks, no signing, the fixed
// identity "nexus", and the transports file, ssh and https only. A push alone may use credentials, and exactly these
// two (docs/accounts.md): for an ssh remote, the user's ssh keys or agent, non-interactively (BatchMode=yes,
// ConnectTimeout); for an https remote, the GitHub CLI's stored login, through `gh auth git-credential`. Every git call
// has a hard deadline. POSIX, like nexus-server.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "sync/hub.h"

namespace nexus::server {

// The files the repository holds, and the only ones it ever stages.
inline const std::vector<std::string> kConfigFiles{".gitignore", "accounts.json", "ais.json"};
// What ais.json was called before the friend names became AI (2026-11-25): open() moves it to ais.json once, readAt()
// reads it at a commit from before, and a history holding it may still be pushed. The word stays here only.
inline const std::string kOldAIsFile = "friends.json";

// The deadline of a git call on the local repository, and of a push.
constexpr int kGitMs = 30000;
constexpr int kPushMs = 60000;

// What git said: its exit status (-1 when it could not be run, or was killed at its deadline) and its output.
struct GitResult {
    int status = -1;
    std::string out;
    std::string err;
};

// Runs git on the repository in `dir` with the clean environment above, killed past `deadlineMs`.
GitResult runGit(const std::string& dir, const std::vector<std::string>& args, int deadlineMs = kGitMs);

class ConfigRepo : public sync::AccountsKeeper {
public:
    // The repository in `dir` (<server dir>/nexus-config), by its name.
    explicit ConfigRepo(std::string dir);
    // The repository wherever `dirNow` says it is now: the server's, which holds its server directory by descriptor
    // and gives the directory's present path (server.cpp), so that a server directory moved aside while the server runs
    // keeps its config with it and never reads or writes the one made at the old name.
    explicit ConfigRepo(std::function<std::string()> dirNow);
    ~ConfigRepo() override;
    ConfigRepo(const ConfigRepo&) = delete;
    ConfigRepo& operator=(const ConfigRepo&) = delete;

    std::string dir() const { return dirNow_ ? dirNow_() : dir_; }
    std::string accountsPath() const { return dir() + "/accounts.json"; }
    std::string aisPath() const { return dir() + "/ais.json"; }

    // Makes the directory (0700) and the repository when they are missing, with its .gitignore and a first commit.
    // Files already there when the repository is made are committed in it only when they read and agree (else they
    // are left as a hand edit to check and reload). A repository from before ais.json has its old file moved to
    // ais.json, once, in a commit of its own (moved() says so). False, with why, when git or the disk fails.
    bool open(std::string* why);

    // What open() did to move the old file to ais.json; empty when there was nothing to move.
    const std::string& moved() const { return moved_; }

    // Whether HEAD holds accounts.json: false for a repository made before any account was set up.
    bool hasAccounts();

    // The registry HEAD holds: what the server applies at start. With `write`, an older file version is upgraded and
    // committed as "upgrade" (`report` says so), unless hand edits are pending, which are never written over: then the
    // registry is read in the new version, and the files are written in it by the next reload. Without `write` (as
    // `config check` reads), nothing is written. False, with every error, when HEAD's files do not read.
    bool committed(accounts::Registry* out, std::vector<std::string>* report, bool write = true);

    // The working files: what a reload would apply. False, with every error (file, line and fix) and every
    // inconsistency, when they do not read or do not agree.
    bool working(accounts::Registry* out, std::vector<std::string>* errors);

    // The registry the files held at `commit` (a hash from history()).
    bool at(const std::string& commit, accounts::Registry* out, std::vector<std::string>* errors);

    // What `config restore <commit>` applies: at(), refused while the working files hold hand edits not yet applied (a
    // restore writes the files, so it would write over them: they are reloaded or discarded first).
    bool restoring(const std::string& commit, accounts::Registry* out, std::vector<std::string>* errors);

    // Whether the working files hold edits HEAD does not (why says so, and how to apply or drop them).
    bool pending(std::string* why);

    // sync::AccountsKeeper: writes `registry` to the files (each atomically: a temporary file, synced, renamed over,
    // mode 0600) and commits exactly them with `message`; a push follows when a remote is set. All or nothing: a write,
    // stage or commit that fails puts the files and the index back as they were. Refused while the files hold hand
    // edits not yet applied, unless a reload is applying them (applying()).
    bool keep(const accounts::Registry& registry, const std::string& message, std::string* why) override;

    // The upgrade from before accounts: `seed` (made from the drivers file) kept as keep() does, with a mark, in the
    // repository's .git and never committed, that the sessions from before accounts are still to take their AIs'
    // default accounts (sync::Hub::adoptDefaults). The mark is set before the commit and taken off when the commit
    // fails, so a server stopped between the two still adopts at its next start. adopting(): the mark is there.
    // adopted(): the sessions took them, so the mark comes off (false, with why, when it cannot).
    bool keepSeed(const accounts::Registry& seed, const std::string& message, std::string* why);
    bool adopting() const;
    bool adopted(std::string* why);

    // While set, keep() commits over the working files' hand edits: a reload is applying them.
    void applying(bool on) { applying_ = on; }

    // What the last keep() had to say beyond its commit (a push asked for). Cleared by each keep().
    const std::vector<std::string>& notes() const { return notes_; }

    // The history, newest first: one line per commit, "<hash> <date> <message>".
    bool history(std::vector<std::string>* lines, std::string* why);

    // The remote: its url as shown (empty: none; any credential in it left out, remoteShown()), set or cleared. Setting
    // checks the history first, and refuses a url that carries a credential (remoteCredential()): nexus pushes with the
    // machine's git credentials.
    bool remote(std::string* url, std::string* why);
    bool setRemote(const std::string& url, std::string* why);
    bool clearRemote(std::string* why);

    // The push, for the server's loop. pushWanted(): a commit asked for one. pushStart(): checks the history and starts
    // `git push` as a child, without waiting; false, with why, when it cannot (nothing to watch then). While it runs,
    // pushFds() are its output pipes to poll and pushWaitMs() the time to its deadline; pushServe() reads what it
    // wrote and, once it has ended (or been killed at its deadline), returns true with its result in `result`.
    bool pushWanted() const { return pushWanted_; }
    bool pushStart(std::int64_t nowMs, std::string* why);
    bool pushRunning() const { return push_.pid > 0; }
    std::vector<int> pushFds() const;
    int pushWaitMs(std::int64_t nowMs) const;
    bool pushServe(std::int64_t nowMs, std::string* result);

private:
    struct Child {
        pid_t pid = -1;
        int out = -1;
        int err = -1;
        std::string outText;
        std::string errText;
        std::int64_t deadline = 0;
        std::string url;        // as shown
        std::string raw;        // as git holds it, only to take out of what git says
    };
    bool rawRemote(std::string* url, std::string* why);
    bool readAt(const std::string& rev, accounts::Registry* out, std::vector<std::string>* errors, bool* upgraded);
    bool onlyKnownHistory(std::string* why);
    bool moveOldAIsFile(std::string* why);
    void endPush();
    std::string dir_;
    std::function<std::string()> dirNow_;
    bool applying_ = false;
    bool pushWanted_ = false;
    std::vector<std::string> notes_;
    std::string moved_;
    Child push_;
};

// The text a driver gives its AI so that it can find and change the server's accounts and AIs cold: where the
// files are, the schema's page, and the verbs (docs/accounts.md). The drivers pass it on in part 4 of the sign-in work;
// for now it is here, tested, for them to use.
std::string configBrief(const std::string& serverDir);

// Whether a remote url carries a credential: a password anywhere (`user:secret@`), or any user part on a scheme other
// than ssh (`https://token@host/...`). An ssh login (`git@host:path`, `ssh://git@host/path`) is a name, not a
// credential.
bool remoteCredential(const std::string& url);

// The url with any credential remoteCredential() finds left out (its whole user part), for printing and logging.
std::string remoteShown(const std::string& url);

} // namespace nexus::server
