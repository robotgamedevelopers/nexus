// Unit tests of the sprint tab's logic without a page (src/sprint.h): reading the board's address from the sprint's
// record through a fake command runner, every way that lookup fails, the routing of events between the page and the
// window, and the script and style nexus adds to the page. No OS, no clock, no window, no process, no network.

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "sprint.h"
#include "style.h"

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

// What `nova-config service show sprint-run --json` prints for a service whose argv is `argv` (a JSON array).
std::string record(const std::string& argv) {
    return R"({"result":{"verb":"service show","status":"ok","exit":0},"items":[{"kind":"service","fields":{)"
        R"("name":"sprint-run","machine":"studio","argv":)" + argv + "}}]}";
}

void parsesTheListenAddress() {
    SprintBoardAddress a = parseSprintRun(record(R"(["nova-sprint","run","--listen","100.76.29.55:8080"])"));
    CHECK(a.error.empty() && a.url == "http://100.76.29.55:8080/");
    a = parseSprintRun(record(R"(["nova-sprint","run","--listen=10.0.0.2:9000"])"));
    CHECK(a.error.empty() && a.url == "http://10.0.0.2:9000/");
}

// A service listening everywhere is reached on this machine.
void listeningEverywhereIsReachedLocally() {
    CHECK(parseSprintRun(record(R"(["nova-sprint","run","--listen","0.0.0.0:8080"])")).url == "http://127.0.0.1:8080/");
    CHECK(parseSprintRun(record(R"(["nova-sprint","run","--listen",":8080"])")).url == "http://127.0.0.1:8080/");
    CHECK(parseSprintRun(record(R"(["nova-sprint","run","--listen","[::]:8080"])")).url == "http://127.0.0.1:8080/");
}

void aRecordWithoutABoardSaysWhatToDo() {
    SprintBoardAddress none = parseSprintRun(record(R"(["nova-sprint","run"])"));
    CHECK(none.url.empty() && has(none.error, "no --listen") && has(none.error, "nova-sprint up"));
    CHECK(has(none.error, "sprint-url http://<host>:<port>/"));
    SprintBoardAddress empty = parseSprintRun(R"({"result":{"status":"ok"},"items":[]})");
    CHECK(empty.url.empty() && has(empty.error, "nova-sprint up") && has(empty.error, "sprint-url http://<host>:<port>/"));
    SprintBoardAddress refused = parseSprintRun(R"({"result":{"status":"refused"},"items":[]})");
    CHECK(refused.url.empty() && has(refused.error, "status refused") && has(refused.error, "run it in a terminal"));
    SprintBoardAddress junk = parseSprintRun("not json");
    CHECK(junk.url.empty() && has(junk.error, "not JSON") && has(junk.error, "run it in a terminal"));
}

// A fake runner: records the command and gives back `result`. No process is spawned.
struct FakeRun {
    CommandResult result;
    Command ran;
    int timeout = 0;
    int calls = 0;
    CommandRunner runner() {
        return [this](const Command& c, int seconds) {
            ran = c;
            timeout = seconds;
            ++calls;
            return result;
        };
    }
};

// What the sprint's command runs (with SHELL /bin/zsh), and how its errors show it.
#if defined(_WIN32)
constexpr const char* kShown = "nova-config service show sprint-run --json";
constexpr const char* kProgram = "nova-config";
#else
constexpr const char* kShown = "/bin/zsh -l -c 'exec nova-config service show sprint-run --json'";
constexpr const char* kProgram = "/bin/zsh";
#endif

SprintBoardAddress findWith(CommandResult result, FakeRun* seen = nullptr, const char* override = nullptr) {
    FakeRun fake{result, {}, 0, 0};
    SprintBoardAddress a = findSprintBoard(override, sprintRunCommand(nullptr, "/bin/zsh"), fake.runner());
    if (seen != nullptr)
        *seen = fake;
    return a;
}

void commandIsTheLoginShellOrTheOverride() {
    Command shell = sprintRunCommand(nullptr, "/bin/zsh");
    CHECK(shell.program == kProgram);
    CHECK(shell.shown == kShown);
#if defined(_WIN32)
    CHECK((shell.arguments == std::vector<std::string>{"service", "show", "sprint-run", "--json"}));
    CHECK(sprintRunCommand(nullptr, nullptr).program == "nova-config");
#else
    CHECK((shell.arguments == std::vector<std::string>{"-l", "-c", "exec nova-config service show sprint-run --json"}));
    CHECK(sprintRunCommand(nullptr, nullptr).program == "/bin/sh");
#endif
    Command direct = sprintRunCommand("/opt/nova/nova-config", "/bin/zsh");
    CHECK(direct.program == "/opt/nova/nova-config");
    CHECK((direct.arguments == std::vector<std::string>{"service", "show", "sprint-run", "--json"}));
}

void theRecordGivesTheAddress() {
    FakeRun seen;
    SprintBoardAddress a = findWith(CommandResult{CommandResult::End::Exited, 0,
        record(R"(["nova-sprint","run","--listen","127.0.0.1:8080"])"), ""}, &seen);
    CHECK(a.error.empty() && a.url == "http://127.0.0.1:8080/");
    CHECK(seen.calls == 1 && seen.timeout == kSprintRunTimeoutSeconds);
}

// The client config's sprint-url wins, and nothing is run; one that is not an http address says so.
void theOverrideWinsWithoutRunningAnything() {
    FakeRun seen;
    SprintBoardAddress a = findWith(CommandResult{}, &seen, "http://board.test:8080/");
    CHECK(a.error.empty() && a.url == "http://board.test:8080/" && seen.calls == 0);
    a = findWith(CommandResult{}, &seen, "board.test:8080");
    CHECK(a.url.empty() && has(a.error, "sprint-url http://<host>:<port>/") && has(a.error, "not an http://") && seen.calls == 0);
    // An empty override is unset.
    a = findWith(CommandResult{CommandResult::End::Exited, 0, record(R"(["x","--listen","h:1"])"), ""}, &seen, "");
    CHECK(a.url == "http://h:1/" && seen.calls == 1);
}

void notFoundSaysWhatToDo() {
    SprintBoardAddress shell = findWith(CommandResult{CommandResult::End::Exited, 127, "", "zsh: command not found"});
    CHECK(shell.url.empty() && has(shell.error, kShown) && has(shell.error, "status 127"));
    CHECK(has(shell.error, "command not found") && has(shell.error, "nova-config <path>"));
    SprintBoardAddress missing = findWith(CommandResult{CommandResult::End::NotFound, 0, "", ""});
    CHECK(has(missing.error, (std::string(kProgram) + " was not found").c_str()) && has(missing.error, "nova-config <path>"));
    SprintBoardAddress unstarted = findWith(CommandResult{CommandResult::End::NotStarted, 0, "", ""});
    CHECK(has(unstarted.error, "could not start") && has(unstarted.error, "sprint-url http://<host>:<port>/"));
}

void timeoutSaysSo() {
    SprintBoardAddress a = findWith(CommandResult{CommandResult::End::TimedOut, 0, "", ""});
    CHECK(a.url.empty() && has(a.error, kShown) && has(a.error, "within 5 seconds") && has(a.error, "run it in a terminal"));
}

void failuresCarryWhatTheCommandSaid() {
    SprintBoardAddress a = findWith(CommandResult{CommandResult::End::Exited, 1, "", "no service sprint-run\n"});
    CHECK(has(a.error, "status 1: no service sprint-run;") && has(a.error, "nova-sprint up"));
    a = findWith(CommandResult{CommandResult::End::Exited, 3, "", ""});
    CHECK(has(a.error, "status 3") && has(a.error, "nothing on standard error"));
    a = findWith(CommandResult{CommandResult::End::Signaled, 9, "", ""});
    CHECK(has(a.error, "signal 9") && has(a.error, "sprint-url http://<host>:<port>/"));
}

PrAppEvent key(PrAppKey k, PrAppModifierFlags modifiers) {
    PrAppEvent e{};
    e.kind = PrAppEventKind_KeyDown;
    e.keyboard.key = k;
    e.modifiers = modifiers;
    return e;
}

PrAppEvent pointer(PrAppEventKind kind) {
    PrAppEvent e{};
    e.kind = kind;
    return e;
}

// Every Command chord and Escape stay with the window; other keys and text go to the page; the pointer's presses and
// the wheel go where the pointer is, a release where its press went, and a move to both.
void eventsAreRouted() {
    constexpr PrAppModifierFlags kCmd = style::kCommand;
    for (char digit : std::string("1234567890"))
        CHECK(routeBoardEvent(key(PrAppKey(digit), kCmd), true, false) == BoardRoute::Window);
    for (PrAppKey k : {PrAppKey('T'), PrAppKey('W'), PrAppKey('='), PrAppKey('-'), PrAppKey('`')})
        CHECK(routeBoardEvent(key(k, kCmd), true, false) == BoardRoute::Window);
    CHECK(routeBoardEvent(key('+', PrAppModifierFlags(kCmd | PrAppModifierFlag_Shift)), true, false) == BoardRoute::Window);
    CHECK(routeBoardEvent(key(PrAppKey_Escape, 0), true, false) == BoardRoute::Window);
    CHECK(routeBoardEvent(key(PrAppKey_Down, 0), true, false) == BoardRoute::Page);
    CHECK(routeBoardEvent(key(PrAppKey(' '), 0), false, false) == BoardRoute::Page);
    // The keys that scroll the page go to it; Command-Up and Command-Down too (to its top and its bottom).
    for (PrAppKey k : std::initializer_list<PrAppKey>{PrAppKey_Up, PrAppKey_Down, PrAppKey_PageUp, PrAppKey_PageDown,
             PrAppKey_Home, PrAppKey_End})
        CHECK(routeBoardEvent(key(k, 0), false, false) == BoardRoute::Page);
    CHECK(routeBoardEvent(key(PrAppKey(' '), PrAppModifierFlag_Shift), false, false) == BoardRoute::Page);
    CHECK(routeBoardEvent(key(PrAppKey_Up, kCmd), false, false) == BoardRoute::Page);
    CHECK(routeBoardEvent(key(PrAppKey_Down, kCmd), false, false) == BoardRoute::Page);
    PrAppEvent text = pointer(PrAppEventKind_Text);
    CHECK(routeBoardEvent(text, false, false) == BoardRoute::Page);
    text.modifiers = kCmd;
    CHECK(routeBoardEvent(text, false, false) == BoardRoute::Window);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_PointerMove), false, false) == BoardRoute::Both);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_PointerDown), true, false) == BoardRoute::Page);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_PointerDown), false, false) == BoardRoute::Window);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_Scroll), true, false) == BoardRoute::Page);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_Scroll), false, false) == BoardRoute::Window);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_PointerUp), false, true) == BoardRoute::Page);
    CHECK(routeBoardEvent(pointer(PrAppEventKind_PointerUp), true, false) == BoardRoute::Window);
}

void preludeCarriesTheZonesAndEscapesThem() {
    std::string p = sprintPrelude("EST", "EDT");
    CHECK(has(p, "STANDARD = \"EST\", DAYLIGHT = \"EDT\""));
    CHECK(!has(p, "@STD@") && !has(p, "@DST@"));
    CHECK(has(p, "DateTimeFormat") && has(p, "formatToParts") && has(p, "toLocaleTimeString"));
    std::string odd = sprintPrelude("a\"b</script>", "c\\d\n");
    CHECK(has(odd, "\"a\\\"b\\u003c/script>\"") && has(odd, "\"c\\\\d\\n\""));
}

// The transfer animation may paint a card. The page retains its theme and its unmeasured layout.
void styleLeavesThePageBackgroundAlone() {
    const auto base = sprintPageStyle(SpendFit{});
    CHECK(base.starts_with(kSprintRepoLinkStyle));
    CHECK(has(base, ".cell.flash { animation-name:nexus-transfer; }"));
    CHECK(!has(base, "min-height: calc("));
    const auto paint = base.find("background-color:");
    CHECK(paint != std::string::npos && has(base, "@keyframes nexus-transfer { from {background-color:"));
    CHECK(base.find("background", paint + 1) == std::string::npos);
    CHECK(!has(base, "--bg") && !has(base, "transparent"));
}

// Measured: the open Cost breakdown reaches one gap above the viewport's bottom from its top, its body takes the
// height, and the margin below it is at least the gap, so the next block starts at or below the bottom; folded, it is
// left alone.
void styleStretchesTheOpenCostBreakdown() {
    std::string s = sprintPageStyle(SpendFit{547.5, 32, 64});
    CHECK(has(s, ".panel.spend:not(.folded) { display: flex; flex-direction: column; box-sizing: border-box; "
        "min-height: calc(100vh - 579.50px); margin-bottom: 64.00px; }"));
    CHECK(has(s, ".panel.spend:not(.folded) > .spend-body { flex: 1 1 auto; }"));
    // A page whose gap below is smaller than its gap between blocks: the margin grows to the gap.
    CHECK(has(sprintPageStyle(SpendFit{100, 32, 8}), "calc(100vh - 132.00px); margin-bottom: 32.00px;"));
    CHECK(has(sprintPageStyle(SpendFit{0, 0, 0}), "calc(100vh - 0.00px); margin-bottom: 0.00px;"));
}

// A link leaves the board for another document, or with target _blank; a place on the board, no URL, or a javascript:
// link stays.
void linksLeaveForAnotherDocument() {
    const std::string_view board = "http://studio.local:7300/";
    CHECK(linkLeavesBoard("https://github.com/mas-bandwidth/nova-sprint", board, ""));
    CHECK(linkLeavesBoard("http://studio.local:7300/cards/12", board, ""));
    CHECK(linkLeavesBoard("http://studio.local:7301/", board, ""));
    CHECK(linkLeavesBoard("mailto:someone@example.test", board, ""));
    CHECK(linkLeavesBoard("http://studio.local:7300/#later", board, "_blank"));
    CHECK(linkLeavesBoard("http://studio.local:7300/#later", board, "_BLANK"));
    CHECK(!linkLeavesBoard("http://studio.local:7300/#later", board, ""));
    CHECK(!linkLeavesBoard("http://studio.local:7300/#later", board, "_self"));
    CHECK(!linkLeavesBoard("http://studio.local:7300/", board, ""));
    CHECK(!linkLeavesBoard("http://studio.local:7300/#", "http://studio.local:7300/#earlier", ""));
    CHECK(!linkLeavesBoard("", board, "_blank"));
    CHECK(!linkLeavesBoard("javascript:void(0)", board, ""));
    CHECK(!linkLeavesBoard("JavaScript:go()", board, "_blank"));
    CHECK(!linkLeavesBoard("http://studio.local:7300/#a", "http://studio.local:7300/#b", "_top"));
}

} // namespace

int main() {
    parsesTheListenAddress();
    listeningEverywhereIsReachedLocally();
    aRecordWithoutABoardSaysWhatToDo();
    commandIsTheLoginShellOrTheOverride();
    theRecordGivesTheAddress();
    theOverrideWinsWithoutRunningAnything();
    notFoundSaysWhatToDo();
    timeoutSaysSo();
    failuresCarryWhatTheCommandSaid();
    eventsAreRouted();
    preludeCarriesTheZonesAndEscapesThem();
    styleLeavesThePageBackgroundAlone();
    styleStretchesTheOpenCostBreakdown();
    linksLeaveForAnotherDocument();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("sprint unit tests passed\n");
    return 0;
}
