// Functional test of nexus-server's config repository (src/server/config_repo.h): real git, as a child process, on a
// repository in a directory of the test's own under the build tree. HOME is that directory too, so the person's git
// config is never read. No network: the remote is a bare repository beside it, by path.
//
//   config_repo_test <work dir>

#include <sys/stat.h>

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

Registry sample() {
    Registry r;
    r.accounts = {{"work", "claude"}, {"cx", "codex"}};
    r.friends = {{"ada", {"work", "cx"}, "work"}};
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
    CHECK(slurp(repo.friendsPath()) == accounts::friendsText(sample()));
    struct stat st{};
    CHECK(::stat(repo.friendsPath().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(history(repo).size() == 2 && history(repo)[0].find("account work set up (asked by the window w)") !=
          std::string::npos);
    CHECK(repo.keep(sample(), "nothing", &why));
    CHECK(history(repo).size() == 2);
    CHECK(repo.committed(&held, &report) && held == sample());
    CHECK(repo.notes().empty());

    // A hand edit: pending, so a window's change is refused rather than written over it; a reload applies it.
    Registry edited = sample();
    edited.friends[0].preferred = "cx";
    spit(repo.friendsPath(), accounts::friendsText(edited));
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
    spit(repo.friendsPath(), "{\n  \"version\": 1,\n  \"friends\": [\n    {\"name\": \"ada\" \"accounts\": []}\n  ]\n}\n");
    errors.clear();
    CHECK(!repo.working(&read, &errors) && has(errors, "friends.json:4:"));
    // An edit that reads but does not agree: the problem says which file to change.
    spit(repo.friendsPath(), "{\"version\": 1, \"friends\": [{\"name\": \"ada\", \"accounts\": [\"gone\"]}]}\n");
    errors.clear();
    CHECK(!repo.working(&read, &errors) && has(errors, "\"gone\""));
    CHECK(repo.committed(&held, &report) && held == edited);
    CHECK(runGit(dir, {"checkout", "--", "friends.json"}).status == 0);
    CHECK(!repo.pending(&why));

    // An earlier version: read at its commit, brought back as a new commit; history never rewritten.
    const std::vector<std::string> before = history(repo);
    const std::string second = before[1].substr(0, before[1].find(' '));
    CHECK(repo.at(second, &read, &errors) && read == sample());
    errors.clear();
    CHECK(!repo.at("nope", &read, &errors) && has(errors, "no commit nope"));
    repo.applying(true);
    CHECK(repo.keep(sample(), "restore " + second + " (asked by the person)", &why));
    repo.applying(false);
    CHECK(history(repo).size() == 4 && history(repo)[3] == before[2]);
    CHECK(repo.committed(&held, &report) && held == sample());

    // Never a secret: the .gitignore admits only its files.
    std::filesystem::create_directories(dir + "/accounts/work");
    spit(dir + "/accounts/work/credential", "x");
    spit(dir + "/notes.txt", "x");
    CHECK(runGit(dir, {"status", "--porcelain"}).out.empty());

    // A remote: every commit is pushed to it.
    CHECK(runGit(work, {"init", "-q", "--bare", "remote.git"}).status == 0);
    std::string url;
    CHECK(repo.remote(&url, &why) && url.empty());
    CHECK(!repo.setRemote("--upload-pack=x", &why));
    CHECK(repo.setRemote(work + "/remote.git", &why));
    CHECK(repo.remote(&url, &why) && url == work + "/remote.git");
    CHECK(repo.keep(other, "account home set up (asked by the window w)", &why));
    CHECK(repo.notes().empty());
    CHECK(runGit(work + "/remote.git", {"log", "-1", "--format=%s", "main"}).out ==
          "account home set up (asked by the window w)\n");

    // A push that fails: applied all the same, and said so.
    CHECK(repo.setRemote(work + "/missing.git", &why));
    CHECK(repo.keep(sample(), "account home removed", &why));
    CHECK(has(repo.notes(), "applied all the same; the next commit pushes again"));
    CHECK(repo.committed(&held, &report) && held == sample());

    // A file tracked that is not one of its files: no remote is set and nothing is pushed.
    CHECK(runGit(dir, {"add", "-f", "notes.txt"}).status == 0);
    CHECK(runGit(dir, {"commit", "-q", "-m", "a stray file"}).status == 0);
    CHECK(!repo.setRemote(work + "/remote.git", &why) && why.find("tracks notes.txt") != std::string::npos);
    CHECK(repo.keep(other, "account home set up again", &why));
    CHECK(has(repo.notes(), "not pushed:"));
    CHECK(repo.clearRemote(&why) && repo.remote(&url, &why) && url.empty());

    // A version newer than this server reads, committed by hand: refused, and the file left as it was.
    const std::string newer = "{\n  \"version\": 2,\n  \"accounts\": []\n}\n";
    spit(repo.accountsPath(), newer);
    CHECK(runGit(dir, {"commit", "-q", "-am", "a newer nexus-server's files"}).status == 0);
    report.clear();
    CHECK(!repo.committed(&held, &report) && has(report, "newer than this nexus-server reads"));
    CHECK(slurp(repo.accountsPath()) == newer);

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
