// Unit tests of the window's pure logic: what it keeps across launches (src/tab_order.h) through in-memory stores, and
// the nova-config command's form and problems (src/command.h). No OS, no clock, no window, no process. The AI tabs
// from the server's list are sync/ai_strip.h's, tested in tests/sync, and here across a restart.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "command.h"
#include "sync/ai_strip.h"
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

bool has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

// The nova-config command and how its failures read (src/command.h). ------------------------------------------------

// What `nova-config service show sprint-run --json` runs with SHELL /bin/zsh, and how its problems show it.
#if defined(_WIN32)
constexpr const char* kShown = "nova-config service show sprint-run --json";
constexpr const char* kProgram = "nova-config";
#else
constexpr const char* kShown = "/bin/zsh -l -c 'exec nova-config service show sprint-run --json'";
constexpr const char* kProgram = "/bin/zsh";
#endif

const std::vector<std::string> kArguments{"service", "show", "sprint-run", "--json"};

// The command goes through the person's login shell, or /bin/sh without SHELL, or straight to the client config's
// nova-config.
void novaConfigIsTheLoginShellOrTheOverride() {
#if defined(_WIN32)
    Command onPath = novaConfigCommand(nullptr, "/bin/zsh", kArguments);
    CHECK(onPath.program == "nova-config" && onPath.arguments == kArguments);
    CHECK(onPath.shown == kShown);
    CHECK(novaConfigCommand(nullptr, nullptr, kArguments).program == "nova-config");
#else
    Command shell = novaConfigCommand(nullptr, "/bin/zsh", kArguments);
    CHECK(shell.program == "/bin/zsh");
    CHECK((shell.arguments == std::vector<std::string>{"-l", "-c", "exec nova-config service show sprint-run --json"}));
    CHECK(shell.shown == kShown);
    CHECK(novaConfigCommand(nullptr, nullptr, kArguments).program == "/bin/sh");
    CHECK(novaConfigCommand("", "", kArguments).program == "/bin/sh");
#endif
    Command direct = novaConfigCommand("/opt/nova/nova-config", "/bin/zsh", kArguments);
    CHECK(direct.program == "/opt/nova/nova-config" && direct.arguments == kArguments);
    CHECK(direct.shown == "/opt/nova/nova-config service show sprint-run --json");
}

std::string problemOf(CommandResult result) {
    return commandProblem(novaConfigCommand(nullptr, "/bin/zsh", kArguments), result, 5);
}

// Each way the command fails names the command and what to do; a success is no problem.
void commandProblemsSayWhatToDo() {
    CHECK(problemOf(CommandResult{CommandResult::End::Exited, 0, "{}", ""}).empty());
    const std::string shell = problemOf(CommandResult{CommandResult::End::Exited, 127, "",
        "zsh:1: command not found: nova-config\n"});
    CHECK(has(shell, kShown) && has(shell, "status 127") && has(shell, "command not found"));
    CHECK(has(shell, "nova-config <path>"));
    const std::string missing = problemOf(CommandResult{CommandResult::End::NotFound, 0, "", ""});
    CHECK(has(missing, kShown) && has(missing, (std::string(kProgram) + " was not found").c_str()));
    CHECK(has(missing, "nova-config <path>"));
    const std::string unstarted = problemOf(CommandResult{CommandResult::End::NotStarted, 0, "", ""});
    CHECK(has(unstarted, kShown) && has(unstarted, "could not start") && has(unstarted, "nova-config <path>"));
    const std::string silent = problemOf(CommandResult{CommandResult::End::Exited, 3, "", ""});
    CHECK(has(silent, kShown) && has(silent, "status 3") && has(silent, "nothing on standard error"));
    CHECK(has(silent, "run it in a terminal"));
    const std::string said = problemOf(CommandResult{CommandResult::End::Exited, 1, "", "connection refused\n"});
    CHECK(has(said, kShown) && has(said, "status 1: connection refused;") && has(said, "run it in a terminal"));
    const std::string late = problemOf(CommandResult{CommandResult::End::TimedOut, 0, "", ""});
    CHECK(has(late, kShown) && has(late, "within 5 seconds") && has(late, "run it in a terminal"));
    const std::string killed = problemOf(CommandResult{CommandResult::End::Signaled, 9, "", ""});
    CHECK(has(killed, kShown) && has(killed, "signal 9"));
}


// The tab order (src/tab_order.h). ----------------------------------------------------------------------------------

std::vector<AIInfo> named(std::vector<std::string> names) {
    std::vector<AIInfo> out;
    for (const std::string& n : names)
        out.push_back(AIInfo{n, "m-" + n});
    return out;
}

std::string namesOf(const std::vector<AIInfo>& ais) {
    std::string out;
    for (const AIInfo& f : ais)
        out += (out.empty() ? "" : " ") + f.name;
    return out;
}

void savedOrderIsApplied() {
    const std::vector<AIInfo> loaded = named({"a", "b", "c", "d"});
    CHECK(namesOf(applyTabOrder(loaded, {})) == "a b c d");                       // nothing saved: loaded order
    CHECK(namesOf(applyTabOrder(loaded, {"c", "a", "d", "b"})) == "c a d b");
    CHECK(namesOf(applyTabOrder(loaded, {"c", "a"})) == "c a b d");              // unsaved AIs follow, in order
    CHECK(namesOf(applyTabOrder(loaded, {"gone", "d", "old"})) == "d a b c");     // vanished names dropped
    CHECK(namesOf(applyTabOrder(loaded, {"b", "b", "a", "b"})) == "b a c d");     // a duplicate counts once
    CHECK(applyTabOrder({}, {"a"}).empty());
    std::vector<AIInfo> applied = applyTabOrder(loaded, {"d"});
    CHECK(applied.size() == 4 && applied[0].model == "m-d");                      // each AI keeps its model
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
    const std::vector<AIInfo> loaded = named({"a", "b", "c"});
    MemoryStore mem;
    std::string warning;
    CHECK(namesOf(loadTabOrder(mem.store(), loaded, &warning)) == "a b c" && warning.empty());   // nothing saved
    Model model(loaded);
    CHECK(model.move_tab(2, 0));
    std::string error;
    CHECK(saveTabOrder(mem.store(), model, &error) && error.empty());
    // A relaunch: the store's AIs in the saved order, the first AI's tab first among them.
    Model again(loadTabOrder(mem.store(), loaded, &warning));
    CHECK(warning.empty());
    CHECK(again.tab_count() == 3 && again.tab(0).name == "c" && again.tab(1).name == "a" && again.tab(2).name == "b");
}

// Behind the sprint tab, as the app runs: the sprint tab is never saved, and stays first after a relaunch, so the first
// saved AI is Command-2's tab.
void sprintTabIsNeverSavedAndStaysFirst() {
    const std::vector<AIInfo> loaded = named({"a", "b", "c"});
    MemoryStore mem;
    Model model(loaded, true);
    CHECK(model.move_tab(3, 1));
    CHECK(aiTabNames(model) == (std::vector<std::string>{"c", "a", "b"}));
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
    CHECK(aiTabNames(model) == (std::vector<std::string>{"b", "a"}));
}

void badStoreUsesTheLoadedOrderAndSaysSo() {
    const std::vector<AIInfo> loaded = named({"a", "b"});
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

// The window reads the saved order's names and the saved tab before any AI has arrived from the server: neither
// needs a model of AIs. Nothing saved is no names and the first tab; a bad file is the same, with a warning.
void savedNamesAndTabLoadAlone() {
    MemoryStore order;
    std::string warning;
    CHECK(loadTabOrderNames(order.store(), &warning).empty() && warning.empty());
    Model model(named({"a", "b", "c"}));
    CHECK(model.move_tab(2, 0) && model.select_tab(1));
    std::string error;
    CHECK(saveTabOrder(order.store(), model, &error));
    CHECK(loadTabOrderNames(order.store(), &warning) == (std::vector<std::string>{"c", "a", "b"}) && warning.empty());
    order.text = "not a tab order\n";
    CHECK(loadTabOrderNames(order.store(), &warning).empty() && has(warning, "deleting it resets"));

    MemoryStore selection;
    SavedTab saved = loadSavedTab(selection.store(), &warning);
    CHECK(saved.kind == TabKind::Sprint && warning.empty());
    CHECK(saveSelectedTab(selection.store(), model, &error));
    saved = loadSavedTab(selection.store(), &warning);
    CHECK(saved.kind == TabKind::AI && saved.name == "a" && warning.empty());
    selection.text = "junk\n";
    saved = loadSavedTab(selection.store(), &warning);
    CHECK(saved.kind == TabKind::Sprint && has(warning, "deleting it resets the selection"));
}

void selectedTabTextRoundTrips() {
    SavedTab saved;
    std::string error;
    Model model(named({"alex", "zhi yǎ"}), true);
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error) && saved.kind == TabKind::Sprint);
    model.select_tab(2);
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error));
    CHECK(saved.kind == TabKind::AI && saved.name == "zhi yǎ");
    model.add_terminal();
    CHECK(formatSelectedTab(model) == "nexus selected tab 2\nterminal 1\n");
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error) && saved.kind == TabKind::Terminal &&
        saved.terminal == 1);
    model.add_terminal();
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error) && saved.terminal == 2);
    CHECK(model.move_tab(4, 3));   // dragged in front of the other: now the first terminal
    CHECK(parseSelectedTab(formatSelectedTab(model), &saved, &error) && saved.terminal == 1);
    // Version 1 named a terminal by no place; version 2 always gives one.
    CHECK(parseSelectedTab("nexus selected tab 1\nterminal\n", &saved, &error) && saved.kind == TabKind::Terminal &&
        saved.terminal == 0);
    CHECK(parseSelectedTab("nexus selected tab 2\nterminal 12\n", &saved, &error) && saved.terminal == 12);
    // Saved before the friend names became AI: read as the AI's tab.
    CHECK(parseSelectedTab("nexus selected tab 2\nfriend ada\n", &saved, &error) && saved.kind == TabKind::AI &&
        saved.name == "ada");
    for (const char* bad : {"terminal", "terminal ", "terminal 0", "terminal 01", "terminal -1", "terminal x",
            "terminal 1234567"}) {
        CHECK(!parseSelectedTab(std::string("nexus selected tab 2\n") + bad + "\n", &saved, &error));
        CHECK(has(error, bad));
    }
    CHECK(!parseSelectedTab("nexus selected tab 1\nterminal 1\n", &saved, &error) && has(error, "terminal 1"));
    CHECK(!parseSelectedTab("nexus selected tab 3\nsprint\n", &saved, &error) && has(error, "first line"));
    CHECK(!parseSelectedTab("AI alex\n", &saved, &error) && has(error, "first line"));
    CHECK(!parseSelectedTab("nexus selected tab 1\n", &saved, &error) && has(error, "no tab"));
    CHECK(!parseSelectedTab("nexus selected tab 1\nwindow\n", &saved, &error) && has(error, "window"));
    CHECK(!parseSelectedTab("nexus selected tab 1\nai \n", &saved, &error) && has(error, "no name"));
    CHECK(!parseSelectedTab(std::string("nexus selected tab 1\nai a\0b\n", 32), &saved, &error) &&
        has(error, "NUL"));
}

// The owner's case: alex selected, then dragged to the end. A relaunch with the saved order puts alex last, and the
// selection comes back to alex by name, not to the position alex stood at.
void selectedTabComesBackByIdentity() {
    const std::vector<AIInfo> loaded = named({"alex", "emma", "rowan"});
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
    // The sprint tab saved, with alex last: the sprint tab, never the last AI.
    CHECK(model.select_tab(0) && saveSelectedTab(selection.store(), model, &error));
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && warning.empty());
    // emma saved, then the AI list changes order: emma wherever she now stands.
    CHECK(model.select_tab(1) && model.tab(1).name == "emma" && saveSelectedTab(selection.store(), model, &error));
    Model reordered(named({"rowan", "alex", "emma"}), true);
    CHECK(loadSelectedTab(selection.store(), reordered, &warning) == 3);
}

// Nothing saved, a terminal saved that did not come back, a version 1 terminal (which names none), an AI no longer
// in the list, or a file that is not a selection: the sprint tab. Only the bad file warns.
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
    again.add_terminal();
    selection.text = "nexus selected tab 1\nterminal\n";
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && warning.empty());
    selection.text = "nexus selected tab 2\nterminal 2\n";
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 0 && warning.empty());
    selection.text = "nexus selected tab 1\nai gone\n";
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

void textScaleTextRoundTrips() {
    int steps = 99;
    std::string error;
    for (int s : {0, 1, -1, 5, -5, 10, 123456, -123456}) {
        CHECK(parseTextScale(formatTextScale(s), &steps, &error) && steps == s);
    }
    CHECK(formatTextScale(-3) == "nexus text scale 1\n-3\n");
    CHECK(parseTextScale("nexus text scale 1\n4", &steps, &error) && steps == 4);   // no final line feed
    CHECK(!parseTextScale("4\n", &steps, &error) && has(error, "first line") && steps == 0);
    CHECK(!parseTextScale("nexus text scale 2\n4\n", &steps, &error) && has(error, "first line"));
    CHECK(!parseTextScale("nexus text scale 1\n", &steps, &error) && has(error, "no scale"));
    CHECK(!parseTextScale("nexus text scale 1", &steps, &error) && has(error, "no scale"));
    for (const char* bad : {"1.5", "+2", "-", "two", " 2", "2 ", "1234567", "--1"}) {
        CHECK(!parseTextScale(std::string("nexus text scale 1\n") + bad + "\n", &steps, &error));
        CHECK(has(error, "whole number") && has(error, bad));
    }
    CHECK(!parseTextScale(std::string("nexus text scale 1\n2\0\n", 22), &steps, &error) && has(error, "NUL"));
}

// Nothing saved: the default scale, and no warning. A saved scale comes back; one outside the range is clamped into it
// and says so; a bad or unreadable file warns and gives the default; a failed save names the file and why.
void textScaleLoadsClampsAndWarns() {
    MemoryStore mem;
    std::string warning;
    CHECK(loadTextScale(mem.store(), -5, 10, &warning) == 0 && warning.empty());
    std::string error;
    CHECK(saveTextScale(mem.store(), 3, &error) && mem.text == "nexus text scale 1\n3\n");
    CHECK(loadTextScale(mem.store(), -5, 10, &warning) == 3 && warning.empty());
    CHECK(saveTextScale(mem.store(), -5, &error) && loadTextScale(mem.store(), -5, 10, &warning) == -5);
    CHECK(warning.empty());
    mem.text = "nexus text scale 1\n40\n";
    CHECK(loadTextScale(mem.store(), -5, 10, &warning) == 10);
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "40") && has(warning, "using 10"));
    mem.text = "nexus text scale 1\n-9\n";
    CHECK(loadTextScale(mem.store(), -5, 10, &warning) == -5 && has(warning, "using -5"));
    mem.text = "nexus text scale 1\nbig\n";
    CHECK(loadTextScale(mem.store(), -5, 10, &warning) == 0);
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "whole number") && has(warning, "deleting it resets"));
    mem.readError = "it cannot be read";
    CHECK(loadTextScale(mem.store(), -5, 10, &warning) == 0 && has(warning, "cannot be read"));
    mem.writeError = "Permission denied";
    CHECK(!saveTextScale(mem.store(), 1, &error));
    CHECK(has(error, "text scale") && has(error, "/fake/tab-order.txt") && has(error, "Permission denied"));
}

// The terminal tabs' directories, in the order the tabs stand, among AIs and behind the sprint tab; a directory a
// line cannot hold, or none, is the home directory.
void terminalDirectoriesFollowTheStrip() {
    Model model(named({"a", "b"}), true);
    const std::size_t one = model.add_terminal();
    const std::size_t two = model.add_terminal();
    const std::size_t three = model.add_terminal();
    std::map<std::size_t, std::string> dirs{{one, "/one"}, {two, std::string("/t\nwo")}, {three, ""}};
    auto of = [&](std::size_t id) { return dirs[id]; };
    CHECK(terminalDirectories(model, of) == (std::vector<std::string>{"/one", "", ""}));
    dirs[two] = std::string("/t\0o", 4);
    dirs[three] = "/a b/three";
    CHECK(model.move_tab(5, 3));
    CHECK(terminalDirectories(model, of) == (std::vector<std::string>{"/a b/three", "/one", ""}));
    CHECK(terminalDirectories(Model(named({"a"}), true), of).empty());
}

void terminalsTextRoundTrips() {
    std::vector<std::string> dirs;
    std::string error;
    const std::vector<std::string> saved{"/Users/x/src", "", "/a b/ünï", "/"};
    CHECK(formatTerminals(saved) == "nexus terminals 1\nterminal /Users/x/src\nterminal\nterminal /a b/ünï\nterminal /\n");
    CHECK(parseTerminals(formatTerminals(saved), &dirs, &error) && dirs == saved);
    CHECK(parseTerminals(formatTerminals({}), &dirs, &error) && dirs.empty());
    CHECK(parseTerminals("nexus terminals 1\n\nterminal /x", &dirs, &error) &&   // blank lines; no final line feed
        dirs == (std::vector<std::string>{"/x"}));
    CHECK(!parseTerminals("terminal /x\n", &dirs, &error) && has(error, "first line") && dirs.empty());
    CHECK(!parseTerminals("nexus terminals 2\n", &dirs, &error) && has(error, "first line"));
    for (const char* bad : {"terminal ", "terminals /x", "/x", "AI a"}) {
        CHECK(!parseTerminals(std::string("nexus terminals 1\nterminal /ok\n") + bad + "\n", &dirs, &error));
        CHECK(has(error, bad) && dirs.empty());
    }
    CHECK(!parseTerminals(std::string("nexus terminals 1\nterminal /a\0b\n", 32), &dirs, &error) && has(error, "NUL"));
}

// Nothing saved: no terminals, and no warning. Saved terminals come back in order; more than the most a launch reopens
// are cut, with a warning; a bad or unreadable file warns and reopens none; a failed save names the file and why.
void terminalsLoadAndFallBack() {
    MemoryStore mem;
    std::string warning;
    CHECK(loadTerminals(mem.store(), &warning).empty() && warning.empty());
    std::string error;
    CHECK(saveTerminals(mem.store(), {"/b", "", "/a"}, &error));
    CHECK(loadTerminals(mem.store(), &warning) == (std::vector<std::string>{"/b", "", "/a"}) && warning.empty());
    mem.text = "nexus terminals 1\n";
    for (std::size_t i = 0; i < kMaxSavedTerminals + 3; ++i)
        mem.text += "terminal /" + std::to_string(i) + "\n";
    std::vector<std::string> many = loadTerminals(mem.store(), &warning);
    CHECK(many.size() == kMaxSavedTerminals && many.front() == "/0" && many.back() == "/31");
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "35") && has(warning, "first 32"));
    mem.text = "nexus terminals 1\nshell /x\n";
    CHECK(loadTerminals(mem.store(), &warning).empty());
    CHECK(has(warning, "/fake/tab-order.txt") && has(warning, "shell /x") && has(warning, "deleting it reopens none"));
    mem.readError = "it cannot be read";
    CHECK(loadTerminals(mem.store(), &warning).empty() && has(warning, "cannot be read"));
    mem.writeError = "Permission denied";
    CHECK(!saveTerminals(mem.store(), {"/x"}, &error));
    CHECK(has(error, "terminals") && has(error, "/fake/tab-order.txt") && has(error, "Permission denied"));
}

// A relaunch as main.cpp runs it: the AIs, the saved terminals added in order, then the saved selection. A selected
// terminal comes back by its place among the terminals, wherever the AIs now stand.
void selectedTerminalComesBackByPlace() {
    MemoryStore terminals;
    MemoryStore selection;
    Model model(named({"a", "b"}), true);
    const std::size_t one = model.add_terminal();
    const std::size_t two = model.add_terminal();
    model.add_terminal();
    std::map<std::size_t, std::string> dirs{{one, "/one"}, {two, "/two"}};
    CHECK(model.select_tab(4));
    std::string error;
    CHECK(saveTerminals(terminals.store(), terminalDirectories(model, [&](std::size_t id) { return dirs[id]; }), &error));
    CHECK(saveSelectedTab(selection.store(), model, &error));
    std::string warning;
    Model again(named({"a", "b", "c"}), true);
    const std::vector<std::string> reopened = loadTerminals(terminals.store(), &warning);
    CHECK(reopened == (std::vector<std::string>{"/one", "/two", ""}));
    for (std::size_t k = 0; k < reopened.size(); ++k)
        again.add_terminal();
    CHECK(again.tab_count() == 7 && again.tab(5).kind == TabKind::Terminal);
    CHECK(loadSelectedTab(selection.store(), again, &warning) == 5 && warning.empty());
    CHECK(again.select_tab(5));
}

// The kept drafts in memory, through the file's own text: what fileKeptDraftsStore does, but the OS (the lock, the
// file), which tests/window/kept_drafts_functional.cpp covers.
struct MemoryKept {
    std::string text;                   // empty: no file yet
    std::string readError;
    std::string writeError;
    KeptDraftsStore store() {
        KeptDraftsStore s;
        s.where = "/fake/kept-drafts.txt";
        s.read = [this](KeptFile* file, std::string* error) {
            file->clear();
            if (!readError.empty()) {
                *error = readError;
                return false;
            }
            return text.empty() || parseKeptDrafts(text, file, error);
        };
        s.update = [this](const KeptChange& change, std::string* error) {
            KeptFile file;
            if (!readError.empty() || !writeError.empty()) {
                *error = readError.empty() ? writeError : readError;
                return false;
            }
            if (!text.empty() && !parseKeptDrafts(text, &file, error)) {
                return false;
            }
            applyKeptChange(&file, change);
            text = formatKeptDrafts(file);
            return true;
        };
        return s;
    }
};

// The kept drafts' file: any bytes of a draft come back as typed (newlines, a line that looks like a header, non-ASCII,
// a draft line inside a draft), each under its server and AI; a file cut short, with a bad length, a bad server or
// AI, or not of this kind reads as nothing, and says so.
void keptDraftsTextRoundTrips() {
    const KeptFile drafts{{{"studio", "ada"}, "two\nlines\n"}, {{"studio", "bo"}, "draft x y 9\nnexus kept drafts 2\n"},
        {{"me@mini", "cy"}, "zhi yǎ wén"}, {{"studio", "dee"}, std::string("a\0b", 3)}, {{"mini", "ada"}, "other"}};
    KeptFile read;
    std::string error;
    CHECK(parseKeptDrafts(formatKeptDrafts(drafts), &read, &error) && read == drafts);
    CHECK(parseKeptDrafts(formatKeptDrafts({}), &read, &error) && read.empty());
    const std::string text = formatKeptDrafts({{{"studio", "ada"}, "hello"}});
    CHECK(text == "nexus kept drafts 2\ndraft studio ada 5\nhello\n");
    CHECK(!parseKeptDrafts(text.substr(0, text.size() - 1), &read, &error) && has(error, "cut short") && read.empty());
    CHECK(!parseKeptDrafts(text.substr(0, text.size() - 3), &read, &error) && has(error, "cut short"));
    CHECK(!parseKeptDrafts("nexus kept drafts 2\ndraft s ada x\nhello\n", &read, &error) && has(error, "not a number"));
    CHECK(!parseKeptDrafts("nexus kept drafts 2\ndraft ada 5\nhello\n", &read, &error) && has(error, "<server>"));
    CHECK(!parseKeptDrafts("nexus kept drafts 2\ns ada 5\nhello\n", &read, &error) && has(error, "<server>"));
    CHECK(!parseKeptDrafts("nexus kept drafts 2\ndraft s .ada 1\na\n", &read, &error) && has(error, "<ai>"));
    CHECK(!parseKeptDrafts("nexus kept drafts 2\ndraft s ada 1\na\ndraft s ada 1\nb\n", &read, &error) &&
          has(error, "two drafts"));
    CHECK(parseKeptDrafts("nexus kept drafts 2\ndraft s ada 1\na\ndraft t ada 1\nb\n", &read, &error) &&
          read.size() == 2);
    CHECK(!parseKeptDrafts("nexus kept drafts 1\ndraft ada 1\na\n", &read, &error) && has(error, "first line"));
    CHECK(!parseKeptDrafts("alex\n", &read, &error) && has(error, "first line"));
    CHECK(!parseKeptDrafts("", &read, &error));
}

// What can key a draft: a host as ssh takes it, never a space or a control byte.
void keptServersAreOneWord() {
    CHECK(keptServer("studio") && keptServer("me@studio.local") && keptServer("10.0.0.2") && keptServer("[::1]"));
    CHECK(!keptServer("") && !keptServer("two words") && !keptServer("tab\there") && !keptServer("line\n") &&
          !keptServer(std::string("nul\0", 4)) && !keptServer("del\x7f") && !keptServer(std::string(256, 'h')));
    CHECK(keptServer(std::string(255, 'h')));
}

// A change of the file: a put joins another window's draft for the same server and AI rather than replacing it;
// a take leaves only what was given back, and only while the file still holds exactly that; other servers' drafts are
// never touched.
void keptChangesMerge() {
    KeptFile file{{{"mini", "ada"}, "on mini"}};
    applyKeptChange(&file, KeptChange{"studio", {{"ada", "from A"}}, {}});
    applyKeptChange(&file, KeptChange{"studio", {{"ada", "from B"}, {"bo", "to bo"}}, {}});
    CHECK(file.at({"studio", "ada"}) == "from A\n\nfrom B" && file.at({"studio", "bo"}) == "to bo");
    CHECK(file.at({"mini", "ada"}) == "on mini");
    applyKeptChange(&file, KeptChange{"studio", {{"bo", "to bo"}, {"cy", ""}}, {}});
    CHECK(file.at({"studio", "bo"}) == "to bo" && file.count({"studio", "cy"}) == 0);
    // A window that gave back only "from A" (it read the file before B's draft was joined) takes nothing.
    applyKeptChange(&file, KeptChange{"studio", {}, {{"ada", "from A"}}});
    CHECK(file.at({"studio", "ada"}) == "from A\n\nfrom B");
    applyKeptChange(&file, KeptChange{"studio", {}, {{"ada", "from A\n\nfrom B"}, {"bo", "to bo"}}});
    CHECK(file.count({"studio", "ada"}) == 0 && file.count({"studio", "bo"}) == 0);
    CHECK(file.size() == 1 && file.at({"mini", "ada"}) == "on mini");
    CHECK((keptFor(file, "mini") == std::map<std::string, std::string>{{"ada", "on mini"}}) &&
          keptFor(file, "studio").empty() && keptFor(file, "min").empty());
}

// Saved and loaded through a store, for one server; a file that does not read is left as it is: none kept, not
// readable, a warning.
void keptDraftsSaveAndLoad() {
    MemoryKept mem;
    bool readable = false;
    std::string warning;
    CHECK(loadKeptDrafts(mem.store(), "studio", &readable, &warning).empty() && readable && warning.empty());
    std::string error;
    CHECK(saveKeptChange(mem.store(), KeptChange{"studio", {{"ada", "kept"}}, {}}, &error));
    CHECK(saveKeptChange(mem.store(), KeptChange{"mini", {{"ada", "elsewhere"}}, {}}, &error));
    const std::map<std::string, std::string> back = loadKeptDrafts(mem.store(), "studio", &readable, &warning);
    CHECK(back.size() == 1 && back.at("ada") == "kept" && readable && warning.empty());
    CHECK(loadKeptDrafts(mem.store(), "mini", &readable, &warning).at("ada") == "elsewhere");
    mem.text = "something else\n";
    CHECK(loadKeptDrafts(mem.store(), "studio", &readable, &warning).empty() && !readable);
    CHECK(has(warning, "/fake/kept-drafts.txt") && has(warning, "left as it is"));
    CHECK(!saveKeptChange(mem.store(), KeptChange{"studio", {{"bo", "x"}}, {}}, &error) && has(error, "first line"));
    CHECK(mem.text == "something else\n");
    mem.readError = "permission denied";
    CHECK(loadKeptDrafts(mem.store(), "studio", &readable, &warning).empty() && !readable &&
          has(warning, "permission denied"));
    mem.readError.clear();
    mem.writeError = "disk full";
    CHECK(!saveKeptChange(mem.store(), KeptChange{"studio", {}, {}}, &error) && has(error, "disk full") &&
          has(error, "/fake/kept-drafts.txt"));
}

// A removed AI's draft across a restart of the window: kept as the tab goes, saved, read back by the next launch
// (main.cpp loadKeptDrafts into the strip), and given back when the server's list names the AI again.
void keptDraftsAcrossARestart() {
    const auto list = [](std::initializer_list<const char*> names) {
        sync::Sessions s;
        std::vector<sync::ServerAI> ais;
        for (const char* n : names) {
            ais.push_back(sync::ServerAI{n});
        }
        s.setAIs(std::move(ais));
        return s;
    };
    MemoryKept mem;
    std::string error;
    {
        Model model({});
        sync::AIStrip strip;
        strip.take(model, list({"ada", "bo"}));
        CHECK(model.set_draft(1, "a prompt\nfor bo"));
        sync::StripTake t = strip.take(model, list({"ada"}));
        CHECK(t.keptChanged && t.put.size() == 1 && t.taken.empty());
        CHECK(saveKeptChange(mem.store(), KeptChange{"studio", std::move(t.put), std::move(t.taken)}, &error));
    }
    bool readable = false;
    std::string warning;
    // Another server's window finds nothing for its bo.
    {
        Model model({});
        sync::AIStrip strip({}, std::string(), loadKeptDrafts(mem.store(), "mini", &readable, &warning));
        strip.take(model, list({"ada", "bo"}));
        CHECK(readable && model.tab(1).name == "bo" && model.tab(1).draft.empty());
    }
    Model model({});
    sync::AIStrip strip({}, std::string(), loadKeptDrafts(mem.store(), "studio", &readable, &warning));
    CHECK(readable && warning.empty() && strip.kept().size() == 1);
    strip.take(model, list({"ada"}));
    CHECK(model.tab_count() == 1 && strip.kept().size() == 1);
    sync::StripTake back = strip.take(model, list({"ada", "bo"}));
    CHECK(back.keptChanged && model.tab_count() == 2 && model.tab(1).name == "bo");
    CHECK(model.tab(1).draft == "a prompt\nfor bo" && strip.kept().empty());
    CHECK(back.put.empty() && back.taken.size() == 1 && back.taken[0].second == "a prompt\nfor bo");
    CHECK(saveKeptChange(mem.store(), KeptChange{"studio", std::move(back.put), std::move(back.taken)}, &error));
    CHECK(loadKeptDrafts(mem.store(), "studio", &readable, &warning).empty());
}

} // namespace

int main() {
    keptDraftsAcrossARestart();
    novaConfigIsTheLoginShellOrTheOverride();
    commandProblemsSayWhatToDo();
    keptDraftsTextRoundTrips();
    keptServersAreOneWord();
    keptChangesMerge();
    keptDraftsSaveAndLoad();
    terminalDirectoriesFollowTheStrip();
    terminalsTextRoundTrips();
    terminalsLoadAndFallBack();
    selectedTerminalComesBackByPlace();
    textScaleTextRoundTrips();
    textScaleLoadsClampsAndWarns();
    selectedTabTextRoundTrips();
    selectedTabComesBackByIdentity();
    selectedTabFallsBackToTheSprintTab();
    savedOrderIsApplied();
    tabOrderTextRoundTrips();
    storeSavesAndRestores();
    terminalTabsAreNeverSaved();
    sprintTabIsNeverSavedAndStaysFirst();
    badStoreUsesTheLoadedOrderAndSaysSo();
    savedNamesAndTabLoadAlone();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window unit tests passed\n");
    return 0;
}
