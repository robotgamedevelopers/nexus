// nexus-server's config repository: ~/.nexus/server/nexus-config/, a git repository of its own that holds the files
// a person or an AI edits by hand, accounts.json and friends.json (accounts/registry.h, docs/accounts.md). Every
// change the server applies is one commit, whose message says what changed and who asked: an account action from a
// window, a hand edit applied by `nexus-server config reload`, an earlier version brought back by `config restore`, or
// an upgrade of an older file version. History is never rewritten: a restore is a new commit.
//
// What the server holds is what HEAD holds. A hand edit sits in the working files, uncommitted, until a reload checks
// it and applies it (committed as the person's edit), so a half-finished edit is never applied; while one sits there,
// an account action from a window is refused rather than written over it.
//
// Secrets never enter it: the credentials stay in ~/.nexus/server/accounts/<name>/, outside this directory; its
// .gitignore admits only its known files; a commit stages those files by name and nothing else; and before any push the
// repository's tracked files are checked to be exactly those.
//
// A remote is optional (`nexus-server config remote <url>`, a private repository of the person's own, suggested name
// nexus-config): with one, every commit is followed by a push, with the git credentials of the user nexus-server runs
// as. A push that fails never blocks or undoes the apply: it is reported, and the next commit pushes again.
//
// git runs as a child process with a clean environment: PATH and HOME only (HOME for the push's credentials), no system
// config (GIT_CONFIG_NOSYSTEM: the Homebrew system gitconfig's Keychain helper is how prompts appeared before), no
// terminal prompts, no hooks, no signing, and the fixed identity "nexus". POSIX, like nexus-server.

#pragma once

#include <string>
#include <vector>

#include "accounts/registry.h"
#include "sync/hub.h"

namespace nexus::server {

// The files the repository holds, and the only ones it ever stages.
inline const std::vector<std::string> kConfigFiles{".gitignore", "accounts.json", "friends.json"};

// What git said: its exit status (-1 when it could not be run) and its output.
struct GitResult {
    int status = -1;
    std::string out;
    std::string err;
};

// Runs `git -C dir <args>` with the clean environment above.
GitResult runGit(const std::string& dir, const std::vector<std::string>& args);

class ConfigRepo : public sync::AccountsKeeper {
public:
    // The repository in `dir` (<server dir>/config).
    explicit ConfigRepo(std::string dir);

    const std::string& dir() const { return dir_; }
    std::string accountsPath() const { return dir_ + "/accounts.json"; }
    std::string friendsPath() const { return dir_ + "/friends.json"; }

    // Makes the directory (0700) and the repository when they are missing, with its .gitignore and a first commit.
    // Files already there when the repository is made are committed in it only when they read and agree (else they
    // are left as a hand edit to check and reload). False, with why, when git or the disk fails.
    bool open(std::string* why);

    // The registry HEAD holds: what the server applies at start. An older file version is upgraded and committed as
    // "upgrade" (`report` says so). False, with every error, when HEAD's files do not read; the server then holds no
    // accounts until a reload applies good ones.
    bool committed(accounts::Registry* out, std::vector<std::string>* report);

    // The working files: what a reload would apply. False, with every error (file, line and fix) and every
    // inconsistency, when they do not read or do not agree.
    bool working(accounts::Registry* out, std::vector<std::string>* errors);

    // The registry the files held at `commit` (a hash from history()).
    bool at(const std::string& commit, accounts::Registry* out, std::vector<std::string>* errors);

    // Whether the working files hold edits HEAD does not.
    bool pending(std::string* why);

    // sync::AccountsKeeper: writes `registry` to the files (each atomically: a temporary file, synced, renamed over,
    // mode 0600) and commits them with `message`, then pushes when a remote is set. Refused while the files hold hand
    // edits not yet applied, unless a reload is applying them (applying()).
    bool keep(const accounts::Registry& registry, const std::string& message, std::string* why) override;

    // While set, keep() commits over the working files' hand edits: a reload or a restore is applying them.
    void applying(bool on) { applying_ = on; }

    // What the last keep() had to say beyond its commit: a push that failed, and why. Cleared by each keep().
    const std::vector<std::string>& notes() const { return notes_; }

    // The history, newest first: one line per commit, "<hash> <date> <message>".
    bool history(std::vector<std::string>* lines, std::string* why);

    // The remote: its url (empty: none), set or cleared. Setting checks the tracked files first.
    bool remote(std::string* url, std::string* why);
    bool setRemote(const std::string& url, std::string* why);
    bool clearRemote(std::string* why);

private:
    bool readAt(const std::string& rev, accounts::Registry* out, std::vector<std::string>* errors, bool* upgraded);
    bool onlyKnownTracked(std::string* why);
    void push();
    std::string dir_;
    bool applying_ = false;
    std::vector<std::string> notes_;
};

// The text a driver gives its AI so that it can find and change the server's accounts and friends cold: where the
// files are, the schema's page, and the verbs (docs/accounts.md). The drivers pass it on in part 4 of the sign-in work;
// for now it is here, tested, for them to use.
std::string configBrief(const std::string& serverDir);

} // namespace nexus::server
