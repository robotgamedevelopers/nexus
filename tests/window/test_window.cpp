// Unit tests of the window's pure logic: the friend list's parsing and loading (src/friends.h) through a fake command
// runner. No OS, no clock, no window, no process.

#include <cstdio>
#include <string>
#include <vector>

#include "friends.h"
#include "tab_order.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

void parsesEnabledFriendsInOrder() {
    FriendList list = parseFriendList(R"({"result":{"verb":"friend list","status":"ok","exit":0},"items":[
        {"kind":"friend","fields":{"name":"alex","model":"","enabled":true}},
        {"kind":"friend","fields":{"name":"off","model":"m","enabled":false}},
        {"kind":"friend","fields":{"name":"emma","model":"gemini \"2\"","enabled":true}},
        {"kind":"friend","fields":{"name":"nameless","model":"m"}}]})");
    CHECK(list.error.empty());
    CHECK(list.friends.size() == 2);
    CHECK(list.friends.size() == 2 && list.friends[0].name == "alex" && list.friends[0].model.empty());
    CHECK(list.friends.size() == 2 && list.friends[1].name == "emma" && list.friends[1].model == "gemini \"2\"");
}

void reportsWhatIsWrong() {
    CHECK(!parseFriendList("not json").error.empty());
    CHECK(parseFriendList("not json").friends.empty());
    CHECK(!parseFriendList(R"({"result":{"status":"refused"},"items":[]})").error.empty());
    CHECK(!parseFriendList(R"({"result":{"status":"ok"}})").error.empty());
    FriendList none = parseFriendList(R"({"result":{"status":"ok"},"items":[]})");
    CHECK(none.friends.empty());
    CHECK(none.error.find("no enabled friends") != std::string::npos);
}

bool has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

// A fake runner: records the command and gives back `result`. No process is spawned.
struct FakeRun {
    CommandResult result;
    Command ran;
    int timeout = 0;
    CommandRunner runner() {
        return [this](const Command& c, int seconds) {
            ran = c;
            timeout = seconds;
            return result;
        };
    }
};

FriendList loadWith(CommandResult result, FakeRun* seen = nullptr) {
    FakeRun fake{result, {}, 0};
    FriendList list = loadFriends(friendListCommand(nullptr, "/bin/zsh"), fake.runner());
    if (seen != nullptr)
        *seen = fake;
    return list;
}

constexpr const char* kShown = "/bin/zsh -l -c 'exec nova-config friend list --json'";

// The command goes through the person's login shell, or /bin/sh without SHELL, or straight to NEXUS_NOVA_CONFIG.
void commandIsTheLoginShellOrTheOverride() {
    Command shell = friendListCommand(nullptr, "/bin/zsh");
    CHECK(shell.program == "/bin/zsh");
    CHECK((shell.arguments == std::vector<std::string>{"-l", "-c", "exec nova-config friend list --json"}));
    CHECK(shell.shown == kShown);
    CHECK(friendListCommand(nullptr, nullptr).program == "/bin/sh");
    CHECK(friendListCommand("", "").program == "/bin/sh");
    Command direct = friendListCommand("/opt/nova/nova-config", "/bin/zsh");
    CHECK(direct.program == "/opt/nova/nova-config");
    CHECK((direct.arguments == std::vector<std::string>{"friend", "list", "--json"}));
    CHECK(direct.shown == "/opt/nova/nova-config friend list --json");
}

void validListLoads() {
    FakeRun seen;
    FriendList list = loadWith(CommandResult{CommandResult::End::Exited, 0,
        R"({"result":{"status":"ok"},"items":[{"fields":{"name":"alex","model":"m","enabled":true}}]})", ""}, &seen);
    CHECK(list.error.empty());
    CHECK(list.friends.size() == 1 && list.friends[0].name == "alex");
    CHECK(seen.timeout == 5);
}

void emptyListSaysWhatToDo() {
    FriendList list = loadWith(CommandResult{CommandResult::End::Exited, 0, R"({"result":{"status":"ok"},"items":[]})",
        ""});
    CHECK(list.friends.empty());
    CHECK(has(list.error, "nova-config friend list --json") && has(list.error, "no enabled friends"));
    CHECK(has(list.error, "nova-config friend add"));
}

void malformedJsonSaysWhatToDo() {
    FriendList list = loadWith(CommandResult{CommandResult::End::Exited, 0, "{\"items\": [", ""});
    CHECK(list.friends.empty());
    CHECK(has(list.error, "not JSON") && has(list.error, "run it in a terminal"));
}

// The shell's own "not found" (status 127), and the shell itself missing.
void commandNotFoundSaysWhatToDo() {
    FriendList shell = loadWith(CommandResult{CommandResult::End::Exited, 127, "",
        "zsh:1: command not found: nova-config\n"});
    CHECK(shell.friends.empty());
    CHECK(has(shell.error, kShown) && has(shell.error, "status 127") && has(shell.error, "command not found"));
    CHECK(has(shell.error, "NEXUS_NOVA_CONFIG"));
    FriendList missing = loadWith(CommandResult{CommandResult::End::NotFound, 0, "", ""});
    CHECK(has(missing.error, kShown) && has(missing.error, "/bin/zsh was not found"));
    CHECK(has(missing.error, "NEXUS_NOVA_CONFIG"));
}

void failureWithEmptyStderrNamesTheCommand() {
    FriendList list = loadWith(CommandResult{CommandResult::End::Exited, 3, "", ""});
    CHECK(list.friends.empty());
    CHECK(has(list.error, kShown) && has(list.error, "status 3") && has(list.error, "nothing on standard error"));
    CHECK(has(list.error, "run it in a terminal"));
}

void failureWithStderrCarriesIt() {
    FriendList list = loadWith(CommandResult{CommandResult::End::Exited, 1, "", "connection refused\n"});
    CHECK(has(list.error, kShown) && has(list.error, "status 1: connection refused;"));
    CHECK(has(list.error, "run it in a terminal"));
}

void timeoutSaysSo() {
    FriendList list = loadWith(CommandResult{CommandResult::End::TimedOut, 0, "", ""});
    CHECK(list.friends.empty());
    CHECK(has(list.error, kShown) && has(list.error, "within 5 seconds") && has(list.error, "run it in a terminal"));
}

// The tab order (src/tab_order.h). ----------------------------------------------------------------------------------

std::vector<FriendInfo> named(std::vector<std::string> names) {
    std::vector<FriendInfo> out;
    for (const std::string& n : names)
        out.push_back(FriendInfo{n, "m-" + n});
    return out;
}

std::string namesOf(const std::vector<FriendInfo>& friends) {
    std::string out;
    for (const FriendInfo& f : friends)
        out += (out.empty() ? "" : " ") + f.name;
    return out;
}

void savedOrderIsApplied() {
    const std::vector<FriendInfo> loaded = named({"a", "b", "c", "d"});
    CHECK(namesOf(applyTabOrder(loaded, {})) == "a b c d");                       // nothing saved: loaded order
    CHECK(namesOf(applyTabOrder(loaded, {"c", "a", "d", "b"})) == "c a d b");
    CHECK(namesOf(applyTabOrder(loaded, {"c", "a"})) == "c a b d");              // unsaved friends follow, in order
    CHECK(namesOf(applyTabOrder(loaded, {"gone", "d", "old"})) == "d a b c");     // vanished names dropped
    CHECK(namesOf(applyTabOrder(loaded, {"b", "b", "a", "b"})) == "b a c d");     // a duplicate counts once
    CHECK(applyTabOrder({}, {"a"}).empty());
    std::vector<FriendInfo> applied = applyTabOrder(loaded, {"d"});
    CHECK(applied.size() == 4 && applied[0].model == "m-d");                      // each friend keeps its model
}

void tabOrderTextRoundTrips() {
    std::vector<std::string> names;
    std::string error;
    CHECK(parseTabOrder(formatTabOrder({"emma", "alex", "zhi yǎ"}), &names, &error));
    CHECK((names == std::vector<std::string>{"emma", "alex", "zhi yǎ"}));
    CHECK(parseTabOrder(formatTabOrder({}), &names, &error) && names.empty());
    CHECK(!parseTabOrder("alex\nemma\n", &names, &error) && has(error, "first line"));
    CHECK(!parseTabOrder("", &names, &error));
    CHECK(!parseTabOrder(std::string("nexus tab order 1\na\0b\n", 22), &names, &error) && has(error, "NUL"));
}

// An in-memory store: no file.
struct MemoryStore {
    bool saved = false;
    std::string text;
    std::string readError;
    std::string writeError;
    int writes = 0;
    TabOrderStore store() {
        return TabOrderStore{"/fake/tab-order.txt",
            [this](std::string* t, std::string* e) {
                *e = readError;
                if (!saved || !readError.empty())
                    return false;
                *t = text;
                return true;
            },
            [this](const std::string& t, std::string* e) {
                ++writes;
                if (!writeError.empty()) {
                    *e = writeError;
                    return false;
                }
                saved = true;
                text = t;
                return true;
            }};
    }
};

void storeSavesAndRestores() {
    const std::vector<FriendInfo> loaded = named({"a", "b", "c"});
    MemoryStore mem;
    std::string warning;
    CHECK(namesOf(loadTabOrder(mem.store(), loaded, &warning)) == "a b c" && warning.empty());   // nothing saved
    Model model(loaded);
    CHECK(model.move_tab(2, 0));
    std::string error;
    CHECK(saveTabOrder(mem.store(), model, &error) && error.empty());
    // A relaunch: the store's friends in the saved order, the first friend's tab first among them.
    Model again(loadTabOrder(mem.store(), loaded, &warning));
    CHECK(warning.empty());
    CHECK(again.tab_count() == 3 && again.tab(0).name == "c" && again.tab(1).name == "a" && again.tab(2).name == "b");
}

// Behind the sprint tab, as the app runs: the sprint tab is never saved, and stays first after a relaunch, so the first
// saved friend is Command-2's tab.
void sprintTabIsNeverSavedAndStaysFirst() {
    const std::vector<FriendInfo> loaded = named({"a", "b", "c"});
    MemoryStore mem;
    Model model(loaded, true);
    CHECK(model.move_tab(3, 1));
    CHECK(friendTabNames(model) == (std::vector<std::string>{"c", "a", "b"}));
    std::string error;
    CHECK(saveTabOrder(mem.store(), model, &error));
    std::string warning;
    Model again(loadTabOrder(mem.store(), loaded, &warning), true);
    CHECK(again.tab_count() == 4 && again.tab(0).kind == TabKind::Sprint && again.tab(1).name == "c");
}

void terminalTabsAreNeverSaved() {
    Model model(named({"a", "b"}));
    model.add_terminal();
    model.add_terminal();
    CHECK(model.move_tab(1, 0));
    CHECK(friendTabNames(model) == (std::vector<std::string>{"b", "a"}));
}

void badStoreUsesTheLoadedOrderAndSaysSo() {
    const std::vector<FriendInfo> loaded = named({"a", "b"});
    MemoryStore mem;
    mem.saved = true;
    mem.text = "not a tab order\nb\n";
    std::string warning;
    CHECK(namesOf(loadTabOrder(mem.store(), loaded, &warning)) == "a b");
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "first line") && has(warning, "deleting it resets"));
    mem.readError = "it cannot be read";
    CHECK(namesOf(loadTabOrder(mem.store(), loaded, &warning)) == "a b");
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "cannot be read") && has(warning, "deleting it resets"));
    mem.writeError = "Permission denied";
    Model model(loaded);
    std::string error;
    CHECK(!saveTabOrder(mem.store(), model, &error));
    CHECK(has(error, "/fake/tab-order.txt") && has(error, "Permission denied"));
}

void selectedTabTextRoundTrips() {
    SavedTab saved;
    std::string error;
    Model model(named({"alex", "zhi yǎ"}), true);
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error) && saved.kind == TabKind::Sprint);
    model.select_tab(2);
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error));
    CHECK(saved.kind == TabKind::Friend && saved.name == "zhi yǎ");
    model.add_terminal();
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error) && saved.kind == TabKind::Terminal);
    CHECK(!parseSelectedTab("friend alex\n", &saved, &error) && has(error, "first line"));
    CHECK(!parseSelectedTab("nexus selected tab 1\n", &saved, &error) && has(error, "no tab"));
    CHECK(!parseSelectedTab("nexus selected tab 1\nwindow\n", &saved, &error) && has(error, "window"));
    CHECK(!parseSelectedTab("nexus selected tab 1\nfriend \n", &saved, &error) && has(error, "no name"));
    CHECK(!parseSelectedTab(std::string("nexus selected tab 1\nfriend a\0b\n", 32), &saved, &error) &&
        has(error, "NUL"));
}

// The owner's case: alex selected, then dragged to the end. A relaunch with the saved order puts alex last, and the
// selection comes back to alex by name, not to the position alex stood at.
void selectedTabComesBackByIdentity() {
    const std::vector<FriendInfo> loaded = named({"alex", "emma", "rowan"});
    MemoryStore order;
    MemoryStore selection;
    Model model(loaded, true);
    CHECK(model.select_tab(1) && model.tab(1).name == "alex");
    CHECK(model.move_tab(1, 3));
    std::string error;
    CHECK(saveTabOrder(order.store(), model, &error) && saveSelectedTab(selection.store(), model, &error));
    std::string warning;
    Model again(loadTabOrder(order.store(), loaded, &warning), true);
    CHECK(again.tab(3).name == "alex");
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 3 && warning.empty());
    // The sprint tab saved, with alex last: the sprint tab, never the last friend.
    CHECK(model.select_tab(0) && saveSelectedTab(selection.store(), model, &error));
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && warning.empty());
    // emma saved, then the friend list changes order: emma wherever she now stands.
    CHECK(model.select_tab(1) && model.tab(1).name == "emma" && saveSelectedTab(selection.store(), model, &error));
    Model reordered(named({"rowan", "alex", "emma"}), true);
    CHECK(loadSelectedTab(selection.store(), reordered, &warning) == 3);
}

// Nothing saved, a terminal saved (terminals do not survive a quit), a friend no longer in the list, or a file that is
// not a selection: the sprint tab. Only the bad file warns.
void selectedTabFallsBackToTheSprintTab() {
    MemoryStore selection;
    Model model(named({"a", "b"}), true);
    std::string warning;
    CHECK(loadSelectedTab(selection.store(), model, &warning) == 0 && warning.empty());
    model.add_terminal();
    std::string error;
    CHECK(saveSelectedTab(selection.store(), model, &error));
    Model again(named({"a", "b"}), true);
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && warning.empty());
    selection.text = "nexus selected tab 1\nfriend gone\n";
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && warning.empty());
    selection.text = "a\n";
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0);
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "first line") && has(warning, "deleting it"));
    selection.readError = "it cannot be read";
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && has(warning, "cannot be read"));
    selection.writeError = "Permission denied";
    CHECK(!saveSelectedTab(selection.store(), again, &error));
    CHECK(has(error, "/fake/tab-order.txt") && has(error, "Permission denied"));
}

} // namespace

int main() {
    selectedTabTextRoundTrips();
    selectedTabComesBackByIdentity();
    selectedTabFallsBackToTheSprintTab();
    savedOrderIsApplied();
    tabOrderTextRoundTrips();
    storeSavesAndRestores();
    terminalTabsAreNeverSaved();
    sprintTabIsNeverSavedAndStaysFirst();
    badStoreUsesTheLoadedOrderAndSaysSo();
    parsesEnabledFriendsInOrder();
    reportsWhatIsWrong();
    commandIsTheLoginShellOrTheOverride();
    validListLoads();
    emptyListSaysWhatToDo();
    malformedJsonSaysWhatToDo();
    commandNotFoundSaysWhatToDo();
    failureWithEmptyStderrNamesTheCommand();
    failureWithStderrCarriesIt();
    timeoutSaysSo();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window unit tests passed\n");
    return 0;
}
