// Functional test of nexus-server's config repository (src/server/config_repo.h): real git, as a child process, on a
// repository in a directory of the test's own under the build tree, with HOME there too (a git config written there
// shows that git never reads it). No network: the remote is a bare repository beside it, by path; the push runs as
// the server's loop runs it, a child polled to its end.
//
//   config_repo_test <work dir>

#include <poll.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "server/config_repo.h"

namespace {

using namespace nexus;
using accounts::Registry;
using server::ConfigRepo;
using server::runGit;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& l : lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

void spit(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

std::vector<std::string> history(ConfigRepo& repo) {
    std::vector<std::string> lines;
    std::string why;
    CHECK(repo.history(&lines, &why));
    return lines;
}

// git on the bare repository that stands for the remote (made at `path` by "init"): what it wrote. The test's own
// look at the remote, outside the server's runGit, which is for the config repository's shape alone.
std::string bare(const std::string& path, const std::string& args) {
    FILE* p = ::popen(("git --git-dir='" + path + "' " + args + " 2>&1").c_str(), "r");
    if (p == nullptr) {
        return "popen failed";
    }
    std::string out;
    char buf[512];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        out.append(buf, n);
    }
    ::pclose(p);
    return out;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The push the last commit asked for, run as the server's loop runs it, to its end: what it said.
std::string pushed(ConfigRepo& repo) {
    std::string why;
    if (!repo.pushStart(nowMs(), &why)) {
        return "not started: " + why;
    }
    std::string result;
    while (!repo.pushServe(nowMs(), &result)) {
        std::vector<pollfd> fds;
        for (int fd : repo.pushFds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::min(repo.pushWaitMs(nowMs()), 100));
    }
    return result;
}

Registry sample() {
    Registry r;
    r.accounts = {{"work", "claude"}, {"cx", "codex"}};
    r.ais = {{"ada", {"work", "cx"}, "work"}};
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: config_repo_test <work dir>\n");
        return 2;
    }
    const std::string work = argv[1];
    std::filesystem::remove_all(work);
    std::filesystem::create_directories(work + "/home");
    ::setenv("HOME", (work + "/home").c_str(), 1);
    const std::string dir = work + "/nexus-config";
    ConfigRepo repo(dir);
    std::string why;
    std::vector<std::string> report;

    // Made on first open: its .gitignore, a first commit, no accounts.
    CHECK(repo.open(&why));
    CHECK(slurp(dir + "/.gitignore").find("!accounts.json") != std::string::npos);
    CHECK(history(repo).size() == 1 && has(history(repo), "nexus-server made its config repository"));
    Registry held;
    CHECK(repo.committed(&held, &report) && held == Registry{});
    CHECK(repo.open(&why));     // opening again changes nothing
    CHECK(history(repo).size() == 1);

    // A change kept: the files in the one layout, mode 0600, one commit with its message; the same again, no commit.
    CHECK(repo.keep(sample(), "account work set up (asked by the window w)", &why));
    CHECK(slurp(repo.accountsPath()) == accounts::accountsText(sample()));
    CHECK(slurp(repo.aisPath()) == accounts::aisText(sample()));
    struct stat st{};
    CHECK(::stat(repo.aisPath().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(history(repo).size() == 2 && history(repo)[0].find("account work set up (asked by the window w)") !=
          std::string::npos);
    CHECK(repo.keep(sample(), "nothing", &why));
    CHECK(history(repo).size() == 2);
    CHECK(repo.committed(&held, &report) && held == sample());
    CHECK(repo.notes().empty());

    // A hand edit: pending, so a window's change is refused rather than written over it; a reload applies it.
    Registry edited = sample();
    edited.ais[0].preferred = "cx";
    spit(repo.aisPath(), accounts::aisText(edited));
    CHECK(repo.pending(&why) && why.find("config reload") != std::string::npos);
    Registry other = sample();
    other.accounts.push_back({"home", "claude"});
    CHECK(!repo.keep(other, "account home set up", &why) && why.find("hand edits not yet applied") != std::string::npos);
    Registry read;
    std::vector<std::string> errors;
    CHECK(repo.working(&read, &errors) && read == edited);
    repo.applying(true);
    CHECK(repo.keep(read, "a hand edit applied by `nexus-server config reload` (the person's edit)", &why));
    repo.applying(false);
    CHECK(!repo.pending(&why));
    CHECK(history(repo).size() == 3 && has(history(repo), "the person's edit"));

    // An edit that does not read: every error with the file and line; the committed state is untouched.
    spit(repo.aisPath(), "{\n  \"version\": 1,\n  \"ais\": [\n    {\"name\": \"ada\" \"accounts\": []}\n  ]\n}\n");
    errors.clear();
    CHECK(!repo.working(&read, &errors) && has(errors, "ais.json:4:"));
    // An edit that reads but does not agree: the problem says which file to change.
    spit(repo.aisPath(), "{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"accounts\": [\"gone\"]}]}\n");
    errors.clear();
    CHECK(!repo.working(&read, &errors) && has(errors, "\"gone\""));
    CHECK(repo.committed(&held, &report) && held == edited);
    CHECK(runGit(dir, {"checkout", "--", "ais.json"}).status == 0);
    CHECK(!repo.pending(&why));

    // An earlier version: read at its commit, brought back as a new commit; history never rewritten. Never over a
    // hand edit: a restore while one is pending is refused, and the edit is left as it is.
    const std::vector<std::string> before = history(repo);
    const std::string second = before[1].substr(0, before[1].find(' '));
    spit(repo.aisPath(), accounts::aisText(sample()));
    errors.clear();
    CHECK(!repo.restoring(second, &read, &errors) && has(errors, "no restore while hand edits are pending"));
    CHECK(slurp(repo.aisPath()) == accounts::aisText(sample()));
    CHECK(runGit(dir, {"checkout", "--", "ais.json"}).status == 0);
    errors.clear();
    CHECK(repo.restoring(second, &read, &errors) && read == sample());
    errors.clear();
    CHECK(!repo.at("nope", &read, &errors) && has(errors, "no commit nope"));
    CHECK(!repo.at("--all", &read, &errors));
    CHECK(repo.keep(sample(), "restore " + second + " (asked by the person)", &why));
    CHECK(history(repo).size() == 4 && history(repo)[3] == before[2]);
    CHECK(repo.committed(&held, &report) && held == sample());
    // `config check` reads HEAD and writes nothing.
    CHECK(repo.committed(&held, &report, false) && held == sample() && history(repo).size() == 4);

    // Never a secret: the .gitignore admits only its files.
    std::filesystem::create_directories(dir + "/accounts/work");
    spit(dir + "/accounts/work/credential", "x");
    spit(dir + "/notes.txt", "x");
    CHECK(runGit(dir, {"status", "--porcelain"}).out.empty());

    // A commit names its files and nothing else: a file staged by hand is not in it, and stays staged.
    CHECK(runGit(dir, {"add", "-f", "notes.txt"}).status == 0);
    CHECK(repo.keep(other, "account home set up (asked by the window w)", &why));
    CHECK(runGit(dir, {"show", "--name-only", "--format=", "HEAD"}).out == "accounts.json\n");
    CHECK(runGit(dir, {"diff", "--cached", "--name-only"}).out == "notes.txt\n");
    CHECK(runGit(dir, {"reset", "-q", "--", "notes.txt"}).status == 0);

    // A commit that fails puts the files and the index back: nothing is half applied.
    const std::string accountsBefore = slurp(repo.accountsPath());
    spit(dir + "/.git/index.lock", "");
    CHECK(!repo.keep(sample(), "never committed", &why) && why.find("put back as they were") != std::string::npos);
    std::filesystem::remove(dir + "/.git/index.lock");
    CHECK(slurp(repo.accountsPath()) == accountsBefore);
    CHECK(!repo.pending(&why));
    CHECK(history(repo).size() == 5);

    // A remote: every commit asks for a push, which keep() never runs: the server's loop starts it as a child and
    // watches it.
    CHECK(bare(work + "/remote.git", "init -q --bare") == "");
    std::string url;
    CHECK(repo.remote(&url, &why) && url.empty());
    CHECK(!repo.setRemote("--upload-pack=x", &why));
    CHECK(repo.setRemote(work + "/remote.git", &why));
    CHECK(repo.remote(&url, &why) && url == work + "/remote.git");
    CHECK(!repo.pushWanted());
    CHECK(repo.keep(sample(), "account home removed (asked by the window w)", &why));
    CHECK(repo.pushWanted() && !repo.pushRunning());
    CHECK(has(repo.notes(), "pushing to " + work + "/remote.git in the background"));
    CHECK(pushed(repo) == "pushed to " + work + "/remote.git");
    CHECK(!repo.pushWanted() && !repo.pushRunning());
    CHECK(bare(work + "/remote.git", "log -1 --format=%s main") ==
          "account home removed (asked by the window w)\n");

    // The person's own git config is never read: here a url rewrite in it would send the push for the missing remote
    // to the real one. A push that fails leaves the commit standing, and says the next commit pushes again.
    spit(work + "/home/.gitconfig", "[url \"" + work + "/remote.git\"]\n\tinsteadOf = " + work + "/missing.git\n");
    CHECK(repo.setRemote(work + "/missing.git", &why));
    CHECK(repo.keep(other, "account home set up again", &why));
    const std::string failed = pushed(repo);
    CHECK(failed.find("pushing to " + work + "/missing.git") != std::string::npos);
    CHECK(failed.find("the next commit pushes again") != std::string::npos);
    CHECK(repo.committed(&held, &report) && held == other);
    CHECK(bare(work + "/remote.git", "log -1 --format=%s main") ==
          "account home removed (asked by the window w)\n");

    // A push past its deadline is ended, and said so.
    CHECK(repo.setRemote(work + "/remote.git", &why));
    CHECK(repo.keep(sample(), "account home removed again", &why));
    CHECK(repo.pushStart(nowMs() - server::kPushMs - 1, &why) && repo.pushRunning());
    std::string result;
    CHECK(repo.pushServe(nowMs(), &result) && result.find("did not finish within 60 s and was ended") !=
          std::string::npos);
    CHECK(!repo.pushRunning());

    // A remote url with a credential in it is refused, saying why, and the remote stays as it was; one put in by hand
    // is never printed with it: not by remote(), the note of a commit, nor what the push says.
    CHECK(!repo.setRemote("https://user:s3cret@github.com/owner/nexus-config.git", &why));
    CHECK(why.find("credentials never go in the remote URL; nexus pushes with the machine's git credentials") !=
          std::string::npos && why.find("s3cret") == std::string::npos);
    CHECK(!repo.setRemote("https://t0ken@github.com/owner/nexus-config.git", &why) && why.find("t0ken") ==
          std::string::npos);
    CHECK(repo.remote(&url, &why) && url == work + "/remote.git");
    CHECK(runGit(dir, {"config", "remote.origin.url", "file://user:s3cret@localhost" + work + "/missing.git"}).status ==
          0);
    CHECK(repo.remote(&url, &why) && url == "file://localhost" + work + "/missing.git");
    CHECK(repo.keep(other, "account home set up once more", &why));
    CHECK(has(repo.notes(), "pushing to file://localhost" + work + "/missing.git") && !has(repo.notes(), "s3cret"));
    const std::string hidden = pushed(repo);
    CHECK(hidden.find("pushing to file://localhost" + work + "/missing.git") != std::string::npos);
    CHECK(hidden.find("s3cret") == std::string::npos);
    CHECK(repo.setRemote(work + "/remote.git", &why));
    CHECK(repo.keep(sample(), "account home removed once more", &why));
    CHECK(pushed(repo) == "pushed to " + work + "/remote.git");
    // An ssh login is a name, not a credential; a password is, on any scheme.
    CHECK(!server::remoteCredential("git@github.com:owner/nexus-config.git"));
    CHECK(!server::remoteCredential("ssh://git@github.com/owner/nexus-config.git"));
    CHECK(!server::remoteCredential("https://github.com/owner/nexus-config.git"));
    CHECK(!server::remoteCredential(work + "/remote.git") && !server::remoteCredential("host:dir/a@b"));
    CHECK(server::remoteCredential("ssh://git:pw@github.com/owner/x.git"));
    CHECK(server::remoteShown("ssh://git:pw@github.com/owner/x.git") == "ssh://github.com/owner/x.git");
    CHECK(server::remoteShown("HTTPS://tok@github.com/x.git?a@b") == "HTTPS://github.com/x.git?a@b");
    CHECK(server::remoteShown("git@github.com:owner/x.git") == "git@github.com:owner/x.git");

    // A credential forced into the history: nothing is pushed, and no remote is set, even once it is out of the index;
    // the refusal never suggests a push.
    CHECK(runGit(dir, {"add", "-f", "accounts/work/credential"}).status == 0);
    CHECK(runGit(dir, {"commit", "-q", "-m", "a credential by mistake"}).status == 0);
    CHECK(repo.keep(other, "account home set up a third time", &why) && repo.pushWanted());
    CHECK(!repo.pushStart(nowMs(), &why) && why.find("holds accounts/work/credential") != std::string::npos);
    CHECK(why.find("rm --cached") == std::string::npos && !repo.pushRunning());
    CHECK(runGit(dir, {"rm", "-q", "--cached", "accounts/work/credential"}).status == 0);
    CHECK(runGit(dir, {"commit", "-q", "-m", "the credential out of the index"}).status == 0);
    CHECK(repo.keep(sample(), "account home removed a third time", &why));
    CHECK(!repo.pushStart(nowMs(), &why) && why.find("holds accounts/work/credential") != std::string::npos);
    CHECK(!repo.setRemote(work + "/remote.git", &why) && why.find("holds accounts/work/credential") !=
          std::string::npos);
    CHECK(bare(work + "/remote.git", "log --all --name-only --format=").find("credential") ==
          std::string::npos);
    CHECK(repo.clearRemote(&why) && repo.remote(&url, &why) && url.empty() && !repo.pushWanted());

    // A file that only a merge commit brought in (added while the merge was made by hand) is in the history too: no
    // remote is set. A log that leaves out merges' changes would not list it.
    {
        const std::string mdir = work + "/merged";
        std::filesystem::create_directories(mdir);
        ConfigRepo merged(mdir);
        CHECK(merged.open(&why));
        CHECK(merged.keep(sample(), "the accounts", &why));
        CHECK(runGit(mdir, {"checkout", "-q", "-b", "side"}).status == 0);
        CHECK(runGit(mdir, {"commit", "-q", "--allow-empty", "-m", "a side commit"}).status == 0);
        CHECK(runGit(mdir, {"checkout", "-q", "-"}).status == 0);
        CHECK(runGit(mdir, {"merge", "-q", "--no-ff", "--no-commit", "side"}).status == 0);
        spit(mdir + "/token.txt", "x");
        CHECK(runGit(mdir, {"add", "-f", "token.txt"}).status == 0);
        CHECK(runGit(mdir, {"commit", "-q", "-m", "the merge"}).status == 0);
        // Taken out again by a second merge, so that no ordinary commit names it.
        CHECK(runGit(mdir, {"checkout", "-q", "-b", "side2"}).status == 0);
        CHECK(runGit(mdir, {"commit", "-q", "--allow-empty", "-m", "a second side commit"}).status == 0);
        CHECK(runGit(mdir, {"checkout", "-q", "-"}).status == 0);
        CHECK(runGit(mdir, {"merge", "-q", "--no-ff", "--no-commit", "side2"}).status == 0);
        CHECK(runGit(mdir, {"rm", "-q", "--cached", "token.txt"}).status == 0);
        CHECK(runGit(mdir, {"commit", "-q", "-m", "the token out of the index, in a merge"}).status == 0);
        CHECK(runGit(mdir, {"log", "--all", "--format=", "--name-only"}).out.find("token.txt") ==
              std::string::npos);    // what a plain log lists: the merge's file is not there
        CHECK(!merged.setRemote(work + "/remote.git", &why) && why.find("holds token.txt") != std::string::npos);
    }

    // The upgrade's mark: set with the seed's commit, still there for a server that opens the repository again (one
    // stopped before its sessions adopted), and off once they have; a seed whose commit fails leaves no mark.
    {
        const std::string sdir = work + "/seeded";
        std::filesystem::create_directories(sdir);
        {
            ConfigRepo seeded(sdir);
            CHECK(seeded.open(&why));
            CHECK(!seeded.adopting());
            spit(sdir + "/.git/index.lock", "");
            CHECK(!seeded.keepSeed(sample(), "upgrade", &why) && !seeded.adopting());
            std::filesystem::remove(sdir + "/.git/index.lock");
            CHECK(seeded.keepSeed(sample(), "upgrade", &why) && seeded.adopting());
        }
        ConfigRepo again(sdir);
        CHECK(again.open(&why) && again.adopting());
        CHECK(again.adopted(&why) && !again.adopting());
        CHECK(again.adopted(&why));     // twice: nothing to take off
        CHECK(runGit(sdir, {"status", "--porcelain"}).out.empty());     // never in the repository's files
    }

    // A version 1 AIs file committed: its AIs read unlimited, as they ran. An AI added by hand while the
    // file is still version 1 never ran: it reads limited, while the committed AI stays as it was.
    {
        const std::string v1 = "{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"accounts\": [\"work\"]}]}\n";
        spit(repo.aisPath(), v1);
        CHECK(runGit(dir, {"commit", "-q", "-am", "a version 1 AIs file"}).status == 0);
        report.clear();
        CHECK(repo.committed(&held, &report, false) && held.aiNamed("ada") != nullptr &&
              !held.aiNamed("ada")->limit);
        spit(repo.aisPath(), "{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"accounts\": [\"work\"]}, "
                                 "{\"name\": \"bo\", \"accounts\": [\"work\"]}]}\n");
        errors.clear();
        CHECK(repo.working(&read, &errors));
        CHECK(read.aiNamed("ada") != nullptr && !read.aiNamed("ada")->limit);
        CHECK(read.aiNamed("bo") != nullptr && read.aiNamed("bo")->limit);
        CHECK(runGit(dir, {"checkout", "--", "ais.json"}).status == 0);
    }

    // A separate private config retains account metadata through commits, a fresh read, and a later AI edit.
    {
        ConfigRepo labeled(work + "/labeled-config");
        Registry expected = sample();
        expected.accounts[0].label = "work";
        expected.accounts[0].email = "dev+work@example.test";
        expected.accounts[0].signInOnUse = true;
        CHECK(labeled.open(&why) && labeled.keep(expected, "account options", &why));
        CHECK(labeled.committed(&held, &report) && held == expected);
        held.ais[0].preferred = "cx";
        CHECK(labeled.keep(held, "switch preferred account", &why));
        Registry restored;
        CHECK(labeled.committed(&restored, &report) && restored == held);
    }

    // A version newer than this server reads, committed by hand: refused, and the file left as it was.
    const std::string newer = "{\n  \"version\": 4,\n  \"accounts\": []\n}\n";
    spit(repo.accountsPath(), newer);
    CHECK(runGit(dir, {"commit", "-q", "-am", "a newer nexus-server's files"}).status == 0);
    report.clear();
    CHECK(!repo.committed(&held, &report) && has(report, "newer than this nexus-server reads"));
    CHECK(slurp(repo.accountsPath()) == newer);

    // Never above its own directory: one with no repository of its own, inside another repository, reads nothing.
    CHECK(runGit(work, {"init", "-q"}).status == 0);
    spit(work + "/outer.txt", "x");
    CHECK(runGit(work, {"add", "outer.txt"}).status == 0);
    CHECK(runGit(work, {"commit", "-q", "-m", "the enclosing repository"}).status == 0);
    std::filesystem::create_directories(work + "/inner");
    ConfigRepo inner(work + "/inner");
    std::vector<std::string> lines;
    CHECK(!inner.history(&lines, &why) && lines.empty());

    // A repository from before ais.json: friends.json under its old .gitignore, its list under "friends". The first open
    // moves it to ais.json in a commit of its own, the text as it was (a hand edit still pending); HEAD reads the same
    // registry, the commit before reads under the old name, and the next committed() writes the list as "ais".
    {
        const std::string olddir = work + "/old-config";
        std::filesystem::create_directories(olddir);
        CHECK(runGit(olddir, {"init", "-q"}).status == 0);
        std::string oldText = accounts::aisText(sample());
        oldText.replace(oldText.find("\"ais\""), 5, "\"friends\"");
        spit(olddir + "/.gitignore", "*\n!.gitignore\n!accounts.json\n!friends.json\n");
        spit(olddir + "/accounts.json", accounts::accountsText(sample()));
        spit(olddir + "/friends.json", oldText);
        CHECK(runGit(olddir, {"add", "--", ".gitignore", "accounts.json", "friends.json"}).status == 0);
        CHECK(runGit(olddir, {"commit", "-q", "-m", "a repository from before"}).status == 0);
        ConfigRepo old(olddir);
        const std::string first = history(old)[0].substr(0, history(old)[0].find(' '));
        Registry edited = sample();
        edited.ais[0].preferred = "cx";
        std::string editedText = accounts::aisText(edited);
        editedText.replace(editedText.find("\"ais\""), 5, "\"friends\"");
        spit(olddir + "/friends.json", editedText);       // a hand edit not yet applied
        CHECK(old.open(&why) && old.moved().find("moved to ais.json, committed") != std::string::npos);
        CHECK(!std::filesystem::exists(olddir + "/friends.json") && slurp(old.aisPath()) == editedText);
        CHECK(slurp(olddir + "/.gitignore").find("!ais.json") != std::string::npos);
        CHECK(history(old).size() == 2 && has(history(old), "nexus-server moved friends.json to ais.json"));
        CHECK(runGit(olddir, {"cat-file", "-e", "HEAD:ais.json"}).status == 0);
        CHECK(runGit(olddir, {"cat-file", "-e", "HEAD:friends.json"}).status != 0);
        CHECK(old.pending(&why));                          // the hand edit is still one
        Registry got;
        std::vector<std::string> errs;
        CHECK(old.working(&got, &errs) && got == edited);
        CHECK(old.at(first, &got, &errs) && got == sample());
        CHECK(runGit(olddir, {"checkout", "--", "ais.json"}).status == 0);
        std::vector<std::string> said;
        CHECK(old.committed(&got, &said) && got == sample() && has(said, "upgraded accounts.json and ais.json"));
        CHECK(slurp(old.aisPath()) == accounts::aisText(sample()));
        CHECK(old.open(&why) && old.moved().empty() && history(old).size() == 3);
        // Both names there: refused, nothing moved, the fix named.
        spit(olddir + "/friends.json", oldText);
        CHECK(!old.open(&why) && why.find("holds both friends.json and ais.json") != std::string::npos);
        std::filesystem::remove(olddir + "/friends.json");
    }

    // What a driver tells its AI.
    const std::string brief = server::configBrief("/srv/.nexus/server");
    CHECK(brief.find("/srv/.nexus/server/nexus-config/accounts.json") != std::string::npos);
    CHECK(brief.find("nexus-server config reload") != std::string::npos);
    CHECK(brief.find("docs/accounts.md") != std::string::npos);

    if (failures != 0) {
        std::fprintf(stderr, "config_repo_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("config_repo_test: ok\n");
    return 0;
}
