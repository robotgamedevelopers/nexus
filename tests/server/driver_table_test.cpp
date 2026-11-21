// Unit tests of the drivers file (src/server/driver_table.h): its two kinds of line, comments and blank lines, the home
// directory for a friend with none or one starting ~/, every line it does not understand, and what a friend with no
// driver is told. Logic only: the file's text is a string.

#include <cstdio>
#include <string>
#include <vector>

#include "server/driver_table.h"

namespace {

using namespace nexus::server;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void lines() {
    const DriverTable t = parseDriverTable("# the drivers\n\n  driver claude /bin/drv --model x\r\n"
                                           "friend ada claude /work\nfriend bo claude\n\tfriend cy claude ~/src\n"
                                           "friend dee claude ~\nfriend eve claude ~other", "/home/me");
    CHECK(t.errors.empty());
    CHECK(t.drivers.size() == 1);
    CHECK((t.drivers.at("claude") == std::vector<std::string>{"/bin/drv", "--model", "x"}));
    CHECK(t.friends.at("ada").harness == "claude");
    CHECK(t.friends.at("ada").cwd == "/work");
    CHECK(t.friends.at("bo").cwd == "/home/me");
    CHECK(t.friends.at("cy").cwd == "/home/me/src");
    CHECK(t.friends.at("dee").cwd == "/home/me");
    CHECK(t.friends.at("eve").cwd == "~other");     // only ~ and ~/ are the home directory
    // A later line for the same name replaces the earlier one.
    const DriverTable again = parseDriverTable("friend ada a\nfriend ada b\ndriver a x\ndriver a y", "/h");
    CHECK(again.friends.at("ada").harness == "b");
    CHECK((again.drivers.at("a") == std::vector<std::string>{"y"}));
    const auto enroll = parseDriverTable("signin codex /bin/login --device\nsignin grok relative\n", "/h");
    CHECK((enroll.signins.at("codex") == std::vector<std::string>{"/bin/login", "--device"}));
    CHECK(enroll.signins.size() == 1 && enroll.errors.size() == 1);
    CHECK(parseDriverTable("", "/h").friends.empty());
}

void refusals() {
    const DriverTable t = parseDriverTable("driver claude\nfriend ada\nfriend ada a b c\nthing x\n", "/h");
    CHECK(t.errors.size() == 4);
    if (t.errors.size() == 4) {
        CHECK(has(t.errors[0], "line 1: a driver line is `driver <harness> <program>"));
        CHECK(has(t.errors[1], "line 2: a friend line is `friend <name> <harness> [<directory>]`"));
        CHECK(has(t.errors[2], "line 3: a friend line"));
        CHECK(has(t.errors[3], "line 4: \"thing\" is not `driver`, `signin` or `friend`"));
    }
    CHECK(t.drivers.empty() && t.friends.empty());
}

void resolve() {
    const DriverTable t = parseDriverTable("driver claude /bin/drv\nfriend ada claude /w\nfriend bo codex\nbad\n", "/h");
    const ResolvedDriver ada = resolveDriver(t, "ada", "/d/drivers");
    CHECK(ada.problem.empty());
    CHECK(ada.command == std::vector<std::string>{"/bin/drv"});
    CHECK(ada.cwd == "/w");
    // A friend the file does not name: the cause, then the two lines that fix it, and the file's own errors.
    const ResolvedDriver cy = resolveDriver(t, "cy", "/d/drivers");
    CHECK(cy.command.empty());
    CHECK(has(cy.problem, "cy has no driver, so the prompt was not sent"));
    CHECK(has(cy.problem, "`friend cy <harness> [<directory>]` in /d/drivers"));
    CHECK(has(cy.problem, "`driver <harness> <program> [<argument>...]`"));
    CHECK(has(cy.problem, "/d/drivers line 4: \"bad\" is not `driver`, `signin` or `friend`."));
    // A friend whose harness has no driver.
    const ResolvedDriver bo = resolveDriver(t, "bo", "/d/drivers");
    CHECK(bo.command.empty());
    CHECK(has(bo.problem, "bo's harness, codex, has no driver, so the prompt was not sent"));
    CHECK(has(bo.problem, "`driver codex <program> [<argument>...]` to /d/drivers"));
    const auto templated = parseDriverTable("driver engine /program {friend} {harness}\nfriend cy engine /work\n", "/h");
    CHECK((resolveDriver(templated, "cy", "/d/drivers").command ==
        std::vector<std::string>{"/program", "cy", "engine"}));
    // No file at all is an empty table.
    CHECK(has(resolveDriver(parseDriverTable("", "/h"), "ada", "/d/drivers").problem, "ada has no driver"));
}

// The accounts and friends the drivers file implies, for a server that had none: each line's --account, its friends
// linked to it, and each friend's one instance, on this machine in its directory.
void seed() {
    const DriverTable t = parseDriverTable(
        "driver claude /bin/drv --account work --model x\n"
        "driver codex /bin/cx\n"
        "driver odd /bin/odd --account -rf\n"
        "friend ada claude\nfriend bo claude /w/bo\nfriend cy codex\nfriend dee odd\n", "/h");
    using nexus::accounts::Friend;
    using nexus::accounts::Instance;
    const nexus::accounts::Registry r = accountsFromDrivers(t, "studio");
    CHECK((r.accounts == std::vector<nexus::accounts::Account>{{"work", "claude"}}));
    // Each friend not limited, as it ran before accounts and friends (docs/accounts.md, "limit").
    CHECK((r.friends == std::vector<Friend>{
        {"ada", {"work"}, "work", {Instance{"studio", "studio", "/h"}}, false},
        {"bo", {"work"}, "work", {Instance{"studio", "studio", "/w/bo"}}, false},
        {"cy", {}, "", {Instance{"studio", "studio", "/h"}}, false},
        {"dee", {}, "", {Instance{"studio", "studio", "/h"}}, false}}));
    CHECK(nexus::accounts::problems(r).empty());
    CHECK(accountsFromDrivers(parseDriverTable("", "/h"), "studio") == nexus::accounts::Registry{});
    // A host name that is not a machine's: no instances, the accounts as before.
    const nexus::accounts::Registry odd = accountsFromDrivers(t, "");
    CHECK(odd.friends.size() == 2 && odd.friends[0].instances.empty());
}

} // namespace

int main() {
    lines();
    refusals();
    resolve();
    seed();
    if (failures != 0) {
        std::fprintf(stderr, "driver_table_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("driver_table_test: ok\n");
    return 0;
}
