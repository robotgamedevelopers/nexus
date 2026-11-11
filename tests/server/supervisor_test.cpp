// Unit tests of the driver supervisor (src/server/supervisor.h), by rule: starting a driver, turn numbers, prompts that
// wait, every protocol break that ends a driver, every deadline at its edges (one millisecond before, at, after), text
// gathering and splitting, the failed-start count, the credential, errors, stop, shutdown from every phase, and a
// restart's state. Logic only: no processes, no clock (the time is an argument, from 1000), no files.
//
//   supervisor_test           runs every group
//   supervisor_test <group>   runs one

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "driver/protocol.h"
#include "server/supervisor.h"

namespace {

using namespace nexus;
using namespace nexus::server;
using namespace nexus::driver;
using What = Effect::What;
using Kind = Command::Kind;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr Millis kStart = 1000;

Config config() {
    Config c;
    c.name = "ada";
    c.command = {"drv", "--x"};
    c.cwd = "/w";
    c.configured = "the test's table";
    c.timing = Timing{100, 200, 50, 60};
    c.maxFailedStarts = 3;
    c.maxWaiting = 2;
    c.maxWaitingBytes = 64;
    c.maxEntryBytes = 10;
    return c;
}

Supervisor make(sync::DriverState state = {}, Output* first = nullptr) {
    Output o;
    Supervisor s(config(), std::move(state), kStart, &o);
    if (first != nullptr) {
        *first = o;
    }
    return s;
}

std::vector<What> whats(const Output& o) {
    std::vector<What> w;
    for (const Effect& e : o.effects) {
        w.push_back(e.what);
    }
    return w;
}

std::vector<Kind> kinds(const Output& o) {
    std::vector<Kind> k;
    for (const Command& c : o.commands) {
        k.push_back(c.kind);
    }
    return k;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The server messages an output writes.
std::vector<ServerMessage> written(const Output& o) {
    std::vector<ServerMessage> out;
    for (const Command& c : o.commands) {
        if (c.kind != Kind::Write) {
            continue;
        }
        CHECK(!c.line.empty() && c.line.back() == '\n');
        const auto d = decodeServerMessage(std::string_view(c.line).substr(0, c.line.size() - 1));
        CHECK(d.message.has_value());
        if (d.message) {
            out.push_back(*d.message);
        }
    }
    return out;
}

bool sends(const Output& o, std::uint64_t turn, const std::string& prompt) {
    const auto w = written(o);
    return w.size() == 1 && w[0] == ServerMessage(Send{turn, prompt});
}

// A supervisor whose driver said hello and holds turn 1 of `prompt` open.
Supervisor working(const std::string& prompt = "hi") {
    Supervisor s = make();
    s.prompt(prompt, kStart);
    s.message(Hello{1, "h"}, kStart + 1);
    CHECK(s.phase() == Phase::Working);
    return s;
}

// The end of a driver: its input closed and the phase.
bool ended(const Supervisor& s, const Output& o) {
    return s.phase() == Phase::Ending && s.stage() == Stage::Closed && !o.commands.empty() &&
        o.commands.back().kind == Kind::CloseInput;
}

// ---- start ----

void testStart() {
    Output first;
    Supervisor s = make({}, &first);
    CHECK(first.effects.empty() && first.commands.empty() && !first.durable);
    CHECK(s.phase() == Phase::None && !s.deadline() && !s.alive());

    Output o = s.prompt("hello", kStart);
    CHECK(whats(o) == std::vector<What>{What::Prompt});
    CHECK(o.effects[0].text == "hello" && entryKind(What::Prompt) == EntryKind::Person);
    CHECK(o.durable && s.state().waiting == std::vector<std::string>{"hello"});
    CHECK(kinds(o) == std::vector<Kind>{Kind::Start});
    CHECK((o.commands[0].argv == std::vector<std::string>{"drv", "--x", "--cwd", "/w"}));
    CHECK(s.phase() == Phase::Starting && s.deadline() == kStart + 100);

    o = s.message(Hello{1, "h"}, kStart + 5);
    CHECK(sends(o, 1, "hello"));
    CHECK(o.durable && s.state().nextTurn == 2 && s.state().openTurn == 1 && s.state().waiting.empty());
    CHECK(s.phase() == Phase::Working && !s.deadline() && s.openPrompt() == "hello");

    // A known session id is the start's --resume.
    sync::DriverState known;
    known.session = "s-1";
    Supervisor r = make(known);
    o = r.prompt("x", kStart);
    CHECK((o.commands[0].argv == std::vector<std::string>{"drv", "--x", "--cwd", "/w", "--resume", "s-1"}));

    // configure() changes the next start's command.
    Supervisor c = make();
    c.configure({"other"}, "/v", "elsewhere");
    o = c.prompt("x", kStart);
    CHECK((o.commands[0].argv == std::vector<std::string>{"other", "--cwd", "/v"}));
}

// ---- turns ----

void testTurns() {
    Supervisor s = working("one");
    Output o = s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
    CHECK(s.phase() == Phase::Idle && s.state().openTurn == 0 && o.durable && s.finished());
    CHECK(o.commands.empty());
    o = s.prompt("two", kStart + 3);
    CHECK(whats(o) == std::vector<What>{What::Prompt});
    CHECK(sends(o, 2, "two"));     // to the idle driver at once: no start
    CHECK(s.state().nextTurn == 3);

    // Numbers continue from the durable state, never from 1.
    sync::DriverState later;
    later.nextTurn = 7;
    Supervisor r = make(later);
    r.prompt("x", kStart);
    o = r.message(Hello{1, "h"}, kStart);
    CHECK(sends(o, 7, "x") && r.state().nextTurn == 8);

    // A driver that exits mid-turn: the turn is cut and never sent again; the next prompt is the next number.
    Supervisor e = working("lost");
    o = e.exited("exited with status 1", kStart + 5);
    CHECK((whats(o) == std::vector<What>{What::Notice, What::Cut}));
    CHECK(contains(o.effects[0].text, "exited with status 1") && o.effects[1].turn == 1);
    CHECK(contains(o.effects[1].text, "unknown") && e.state().openTurn == 0 && e.state().waiting.empty());
    CHECK(o.commands.empty() && e.phase() == Phase::None);
    o = e.prompt("again", kStart + 6);
    CHECK(kinds(o) == std::vector<Kind>{Kind::Start});
    o = e.message(Hello{1, "h"}, kStart + 7);
    CHECK(sends(o, 2, "again"));
}

// ---- waiting ----

void testWaiting() {
    Supervisor s = working("one");
    Output o = s.prompt("two", kStart + 2);
    CHECK(whats(o) == std::vector<What>{What::Prompt} && o.commands.empty());
    o = s.prompt("three", kStart + 3);
    CHECK(o.commands.empty() && s.state().waiting.size() == 2);
    // Past the bound: refused with a notice, the person's entry not shown, nothing durable.
    o = s.prompt("four", kStart + 4);
    CHECK(whats(o) == std::vector<What>{What::Notice} && !o.durable);
    CHECK(contains(o.effects[0].text, "2 prompts already wait") && s.state().waiting.size() == 2);
    // In order, each when the turn before is done.
    o = s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 5);
    CHECK(sends(o, 2, "two"));
    o = s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 6);
    CHECK(sends(o, 3, "three"));

    // Prompts wait while the driver starts.
    Supervisor t = make();
    t.prompt("a", kStart);
    o = t.prompt("b", kStart);
    CHECK(o.commands.empty() && t.state().waiting.size() == 2);

    // The byte bound (64 here).
    Supervisor b = working();
    b.prompt(std::string(40, 'a'), kStart + 2);
    o = b.prompt(std::string(30, 'b'), kStart + 3);
    CHECK(whats(o) == std::vector<What>{What::Notice} && contains(o.effects[0].text, "KiB"));

    // A prompt that cannot be a protocol line (not UTF-8) is refused before it waits.
    Supervisor u = make();
    o = u.prompt("bad \xff", kStart);
    CHECK(whats(o) == std::vector<What>{What::Notice} && o.commands.empty() && u.state().waiting.empty());
    CHECK(contains(o.effects[0].text, "was not sent"));
}

// ---- protocol: what ends a driver ----

void testProtocol() {
    // Anything but hello first.
    const std::vector<DriverMessage> early = {Text{1, "x"}, Session{"s"}, Credential{Billing::Subscription, ""},
        Error{"c", "f", std::nullopt, ErrorCode::Other}, Done{1, Outcome::Ok, std::nullopt, std::nullopt}};
    for (const DriverMessage& m : early) {
        Supervisor s = make();
        s.prompt("x", kStart);
        Output o = s.message(m, kStart + 1);
        CHECK(ended(s, o) && whats(o) == std::vector<What>{What::Notice});
        CHECK(contains(o.effects[0].text, "before its hello"));
        CHECK(s.state().waiting.size() == 1);   // the prompt was never written: it waits for the next driver
    }
    // A protocol this server does not speak.
    {
        Supervisor s = make();
        s.prompt("x", kStart);
        Output o = s.message(Hello{2, "h"}, kStart + 1);
        CHECK(ended(s, o) && contains(o.effects[0].text, "version 2"));
    }
    // A second hello; a message of a turn not open; a second done. Each cuts the open turn.
    const std::vector<DriverMessage> breaks = {Hello{1, "h"}, Text{2, "x"}, TextEnd{2}, Tool{3, "n", "s"},
        Done{2, Outcome::Ok, std::nullopt, std::nullopt}};
    for (const DriverMessage& m : breaks) {
        Supervisor s = working();
        Output o = s.message(m, kStart + 2);
        CHECK(ended(s, o) && (whats(o) == std::vector<What>{What::Notice, What::Cut}));
        CHECK(contains(o.effects[0].text, "breaks the driver protocol"));
    }
    {
        Supervisor s = working();
        s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
        Output o = s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 3);
        CHECK(ended(s, o) && whats(o) == std::vector<What>{What::Notice});
        CHECK(contains(o.effects[0].text, "with no turn open"));
    }
    // An error of a turn not open is dropped; an unknown kind is skipped.
    {
        Supervisor s = working();
        Output o = s.message(Error{"c", "f", 9, ErrorCode::Protocol}, kStart + 2);
        CHECK(o.effects.empty() && o.commands.empty() && s.phase() == Phase::Working);
        o = s.message(Unknown{"later"}, kStart + 3);
        CHECK(o.effects.empty() && o.commands.empty() && s.phase() == Phase::Working);
    }
    // A refused line, or a pipe closing.
    {
        Supervisor s = working();
        Output o = s.refused("line 2: not JSON", kStart + 2);
        CHECK(ended(s, o) && contains(o.effects[0].text, "line 2: not JSON"));
        Supervisor t = working();
        o = t.closed("output", kStart + 2);
        CHECK(ended(t, o) && contains(o.effects[0].text, "closed its output"));
    }
    // Once ending, whatever the driver says is dropped, and nothing more ends it.
    {
        Supervisor s = working();
        s.refused("x", kStart + 2);
        for (const DriverMessage& m : breaks) {
            Output o = s.message(m, kStart + 3);
            CHECK(o.effects.empty() && o.commands.empty());
        }
        Output o = s.refused("y", kStart + 3);
        CHECK(o.effects.empty() && o.commands.empty());
        o = s.message(Text{1, "late"}, kStart + 3);
        CHECK(o.effects.empty());
    }
    // Nothing reaches a supervisor with no driver.
    {
        Supervisor s = make();
        Output o = s.message(Text{1, "x"}, kStart);
        CHECK(o.effects.empty() && o.commands.empty());
        o = s.exited("exited with status 0", kStart);
        CHECK(o.effects.empty() && o.commands.empty() && s.failedStarts() == 0);
    }
}

// ---- deadlines, each one millisecond before, at, and after ----

void testDeadlines() {
    // hello: 100 from the start.
    for (Millis at : {kStart + 100, kStart + 101}) {
        Supervisor s = make();
        s.prompt("x", kStart);
        Output o = s.tick(kStart + 99);
        CHECK(o.effects.empty() && o.commands.empty() && s.phase() == Phase::Starting);
        o = s.tick(at);
        CHECK(ended(s, o) && contains(o.effects[0].text, "did not say hello within 0.1 s"));
    }
    // stop: 200 from the stop.
    for (Millis at : {kStart + 300, kStart + 301}) {
        Supervisor s = working();
        s.stop(kStart + 100);
        CHECK(s.deadline() == kStart + 300);
        Output o = s.tick(kStart + 299);
        CHECK(o.effects.empty() && s.phase() == Phase::Stopping);
        o = s.tick(at);
        CHECK(ended(s, o) && (whats(o) == std::vector<What>{What::Notice, What::Cut}));
        CHECK(contains(o.effects[0].text, "did not end the stopped turn"));
    }
    // ending: TERM 50 after the input closed, KILL 60 after TERM, then no deadline.
    {
        Supervisor s = working();
        s.refused("x", kStart + 10);
        CHECK(s.deadline() == kStart + 60);
        CHECK(s.tick(kStart + 59).commands.empty());
        Output o = s.tick(kStart + 60);
        CHECK(kinds(o) == std::vector<Kind>{Kind::Terminate} && s.stage() == Stage::Termed);
        CHECK(s.deadline() == kStart + 120);
        CHECK(s.tick(kStart + 119).commands.empty());
        o = s.tick(kStart + 121);
        CHECK(kinds(o) == std::vector<Kind>{Kind::Kill} && s.stage() == Stage::Killed && !s.deadline());
        CHECK(s.tick(kStart + 100000).commands.empty());
    }
    // A working turn has no deadline, however long it runs.
    {
        Supervisor s = working();
        CHECK(!s.deadline());
        Output o = s.tick(kStart + 1000ull * 60 * 60 * 24);
        CHECK(o.effects.empty() && o.commands.empty() && s.phase() == Phase::Working);
        s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 3);
        CHECK(!s.deadline());   // nor an idle driver
    }
}

// ---- text ----

void testText() {
    // Deltas gather into one entry at text-end.
    {
        Supervisor s = working();
        CHECK(s.message(Text{1, "Let "}, kStart + 2).effects.empty());
        s.message(Text{1, "me."}, kStart + 3);
        CHECK(s.gathering());
        Output o = s.message(TextEnd{1}, kStart + 4);
        CHECK(whats(o) == std::vector<What>{What::Reply} && o.effects[0].text == "Let me." && o.effects[0].turn == 1);
        CHECK(!s.gathering() && entryKind(What::Reply) == EntryKind::Friend);
        // An empty block adds nothing.
        CHECK(s.message(TextEnd{1}, kStart + 5).effects.empty());
    }
    // Past the entry limit (10 bytes here), split between characters: "é" is two bytes.
    {
        Supervisor s = working();
        const std::string text = "ab\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9xyz";     // 2 + 10 + 3 bytes
        s.message(Text{1, text.substr(0, 4)}, kStart + 2);
        Output o = s.message(Text{1, text.substr(4)}, kStart + 3);
        CHECK(whats(o) == std::vector<What>{What::Reply});      // a full entry goes in as soon as there is one
        CHECK(o.effects[0].text == "ab\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
        o = s.message(TextEnd{1}, kStart + 4);
        CHECK(whats(o) == std::vector<What>{What::Reply} && o.effects[0].text == "\xc3\xa9xyz");
    }
    // A tool: the text before it first, then one line, cut to the limit.
    {
        Supervisor s = working();
        s.message(Text{1, "a"}, kStart + 2);
        Output o = s.message(Tool{1, "Bash", "ls"}, kStart + 3);
        CHECK((whats(o) == std::vector<What>{What::Reply, What::Tool}) && o.effects[1].text == "Bash: ls");
        o = s.message(Tool{1, "Bash", "ls -la src"}, kStart + 4);
        CHECK(o.effects[0].text == "Bash: ls -" && entryKind(What::Tool) == EntryKind::Friend);
    }
    // A cut keeps the text gathered, before the notice.
    {
        Supervisor s = working();
        s.message(Text{1, "half"}, kStart + 2);
        Output o = s.exited("was killed by signal 9", kStart + 3);
        CHECK((whats(o) == std::vector<What>{What::Reply, What::Notice, What::Cut}) && o.effects[0].text == "half");
    }
    // done: the text, then for a stopped turn a notice, and the cost.
    {
        Supervisor s = working();
        s.message(Text{1, "part"}, kStart + 2);
        Output o = s.message(Done{1, Outcome::Stopped, 0.5, std::nullopt}, kStart + 3);
        CHECK((whats(o) == std::vector<What>{What::Reply, What::Stopped}) && o.sessionCostUsd == 0.5);
        CHECK(entryKind(What::Stopped) == EntryKind::Notice);
    }
}

// ---- failed starts ----

void testFailedStarts() {
    Supervisor s = make();
    s.prompt("x", kStart);
    for (int i = 1; i <= 3; ++i) {
        Output o = s.exited("exited with status 2", kStart + Millis(i));
        CHECK(s.failedStarts() == i);
        if (i < 3) {
            CHECK(kinds(o) == std::vector<Kind>{Kind::Start});     // tried again
        } else {
            CHECK(o.commands.empty() && s.held() && s.phase() == Phase::None);
            CHECK((whats(o) == std::vector<What>{What::Notice, What::Notice}));
            CHECK(contains(o.effects[1].text, "after 3 failed starts") && contains(o.effects[1].text, "status 2"));
            CHECK(contains(o.effects[1].text, "`drv`") && contains(o.effects[1].text, "the test's table"));
        }
    }
    CHECK(s.state().waiting.size() == 1);   // still waiting
    CHECK(s.tick(kStart + 100000).commands.empty());
    // The next prompt tries again, from a count of 0.
    Output o = s.prompt("y", kStart + 10);
    CHECK(kinds(o) == std::vector<Kind>{Kind::Start} && !s.held() && s.failedStarts() == 0);

    // A hello timeout's end counts too; the driver's own fix is in the notice.
    Supervisor t = make();
    t.prompt("x", kStart);
    t.message(Hello{1, "h"}, kStart);
    t.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart);
    // A driver that finished a turn and then exits is not a failed start.
    t.exited("exited with status 0", kStart + 1);
    CHECK(t.failedStarts() == 0);
    t.prompt("y", kStart + 2);
    t.tick(kStart + 102);
    t.exited("exited with status 0", kStart + 103);
    CHECK(t.failedStarts() == 1 && t.phase() == Phase::Starting);   // "y" still waits: started again
    // A driver that says hello, reports it cannot work, and exits: the prompt it was sent is cut, and the next prompt
    // starts the next try, until the hold. (A driver that reports no credential holds at once: the credential
    // group.)
    for (Millis at = kStart + 200; t.failedStarts() < 3; at += 10) {
        if (t.phase() == Phase::None) {
            t.prompt("z", at);
        }
        t.message(Hello{1, "h"}, at + 1);
        t.message(Error{"no login", "run login", std::nullopt, ErrorCode::Harness}, at + 2);
        o = t.exited("exited with status 1", at + 3);
    }
    CHECK(t.held() && contains(o.effects.back().text, "The last: no login. run login."));
}

// ---- the credential, errors, the session ----

void testCredentialAndErrors() {
    {
        Supervisor s = working();
        Output o = s.message(Credential{Billing::Subscription, "max"}, kStart + 2);
        CHECK(o.credential && o.credential->plan == "max" && o.effects.empty() && s.phase() == Phase::Working);
    }
    for (Billing b : {Billing::ApiKey, Billing::None}) {
        Supervisor s = working();
        s.prompt("waits", kStart + 2);
        Output o = s.message(Credential{b, ""}, kStart + 3);
        CHECK(ended(s, o) && s.held() && (whats(o) == std::vector<What>{What::Notice, What::Cut}));
        CHECK(contains(o.effects[0].text, b == Billing::ApiKey ? "API key" : "no credential"));
        o = s.exited("exited with status 0", kStart + 4);
        CHECK(o.commands.empty() && s.state().waiting.size() == 1 && s.failedStarts() == 0);  // held, not counted
        o = s.prompt("again", kStart + 5);
        CHECK(kinds(o) == std::vector<Kind>{Kind::Start});
    }
    {
        Supervisor s = working();
        Output o = s.message(Error{"unknown id", "none", std::nullopt, ErrorCode::SessionNotFound}, kStart + 2);
        CHECK(whats(o) == std::vector<What>{What::Notice} && contains(o.effects[0].text, "could not be resumed"));
        o = s.message(Error{"it broke", "send again", 1, ErrorCode::Harness}, kStart + 3);
        CHECK(whats(o) == std::vector<What>{What::Error} && o.effects[0].turn == 1);
        CHECK(o.effects[0].text == "ada's driver reports: it broke. send again.");
        o = s.message(Session{"s-9"}, kStart + 4);
        CHECK(o.durable && s.state().session == "s-9" && o.effects.empty());
        o = s.message(Session{"s-9"}, kStart + 5);
        CHECK(!o.durable);
        o = s.message(Done{1, Outcome::Error, std::nullopt, std::nullopt}, kStart + 6);
        CHECK(o.effects.empty() && s.phase() == Phase::Idle);
    }
    // No credential (protocol rule 11): the error and done of the open turn, then the driver exits. The prompt
    // waiting is never written to it: the driver is ended quietly once its turn is done, and none is started again
    // until the person's next prompt.
    {
        Supervisor s = working();
        s.prompt("waits", kStart + 2);
        Output o = s.message(Error{"not signed in", "sign in", 1, ErrorCode::NoCredential}, kStart + 3);
        CHECK(whats(o) == std::vector<What>{What::Error} && s.held() && s.phase() == Phase::Working);
        o = s.message(Done{1, Outcome::Error, std::nullopt, std::nullopt}, kStart + 4);
        CHECK(written(o).empty() && ended(s, o) && o.effects.empty());
        CHECK(s.state().openTurn == 0 && s.state().nextTurn == 2 && s.state().waiting.size() == 1);
        o = s.exited("exited with status 0", kStart + 5);
        CHECK(o.commands.empty() && o.effects.empty() && s.failedStarts() == 0 && s.phase() == Phase::None);
        o = s.prompt("again", kStart + 6);
        CHECK(kinds(o) == std::vector<Kind>{Kind::Start});
    }
    // ... and with no turn open: ended at once.
    {
        Supervisor s = working();
        s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
        Output o = s.message(Error{"not signed in", "sign in", std::nullopt, ErrorCode::NoCredential}, kStart + 3);
        CHECK(whats(o) == std::vector<What>{What::Notice} && ended(s, o) && s.held());
        o = s.prompt("waits", kStart + 4);
        CHECK(written(o).empty() && s.phase() == Phase::Ending && !s.held());
        o = s.exited("exited with status 0", kStart + 5);
        CHECK(kinds(o) == std::vector<Kind>{Kind::Start});
    }
    // A driver's cause and fix go into the tab cut to kReportBytes each, however long the line that carried them, and
    // so does the cause the failed-start notice repeats.
    {
        const std::string big(100000, 'c');
        Supervisor s = working();
        Output o = s.message(Error{big, big, 1, ErrorCode::Harness}, kStart + 2);
        CHECK(o.effects.size() == 1 && o.effects[0].text.size() < 2 * kReportBytes + 100);
        o = s.message(Error{big, "f", std::nullopt, ErrorCode::SessionNotFound}, kStart + 3);
        CHECK(o.effects.size() == 1 && o.effects[0].text.size() < kReportBytes + 100);
        s.message(Error{big, big, 1, ErrorCode::Harness}, kStart + 4);
        for (int i = 0; i < 3; ++i) {
            o = s.exited("exited with status 1", kStart + 5);
            CHECK(!o.effects.empty());
            for (const Effect& e : o.effects) {
                CHECK(e.text.size() < 2 * kReportBytes + 400);
            }
            if (i < 2) {
                CHECK(kinds(s.prompt("w", kStart + 5)) == std::vector<Kind>{Kind::Start});
                s.message(Hello{1, "h"}, kStart + 5);
                s.message(Error{big, big, std::nullopt, ErrorCode::Harness}, kStart + 5);
            }
        }
        CHECK(s.held() && contains(o.effects.back().text, "stopped starting"));
    }
}

// ---- stop ----

void testStop() {
    Supervisor none = make();
    CHECK(none.stop(kStart).commands.empty());
    Supervisor starting = make();
    starting.prompt("x", kStart);
    CHECK(starting.stop(kStart).commands.empty() && starting.phase() == Phase::Starting);
    Supervisor s = working();
    Output o = s.stop(kStart + 2);
    CHECK(written(o) == std::vector<ServerMessage>{Stop{1}} && s.phase() == Phase::Stopping);
    CHECK(s.stop(kStart + 3).commands.empty());     // one stop out at a time
    s.message(Text{1, "so far"}, kStart + 4);
    o = s.message(Done{1, Outcome::Stopped, std::nullopt, std::nullopt}, kStart + 5);
    CHECK((whats(o) == std::vector<What>{What::Reply, What::Stopped}) && s.phase() == Phase::Idle && !s.deadline());
    Supervisor idle = working();
    idle.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
    CHECK(idle.stop(kStart + 3).commands.empty());
}

// ---- shutdown, from every phase ----

void testShutdown() {
    {
        Supervisor s = make();
        Output o = s.shutdown(kStart);
        CHECK(o.effects.empty() && o.commands.empty() && s.shuttingDown());
        o = s.prompt("x", kStart);     // waits for the next server
        CHECK(o.commands.empty() && s.state().waiting.size() == 1);
        CHECK(s.shutdown(kStart).commands.empty());
    }
    auto quiet = [](Supervisor& s) {
        Output o = s.shutdown(kStart + 50);
        CHECK(o.effects.empty() && ended(s, o));
        o = s.exited("exited with status 0", kStart + 51);
        CHECK(o.effects.empty() && o.commands.empty() && s.phase() == Phase::None && s.failedStarts() == 0);
    };
    Supervisor starting = make();
    starting.prompt("x", kStart);
    quiet(starting);
    CHECK(starting.state().waiting.size() == 1);    // durable, for the next server
    Supervisor idle = working();
    idle.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
    quiet(idle);
    for (bool stopping : {false, true}) {
        Supervisor s = working();
        if (stopping) {
            s.stop(kStart + 2);
        }
        s.prompt("waits", kStart + 3);
        Output o = s.shutdown(kStart + 4);
        CHECK(ended(s, o) && (whats(o) == std::vector<What>{What::Notice, What::Cut}));
        CHECK(contains(o.effects[0].text, "nexus-server is stopping"));
        o = s.exited("exited with status 0", kStart + 5);
        CHECK(o.commands.empty() && s.state().waiting.size() == 1);
    }
    {
        Supervisor s = working();
        s.refused("x", kStart + 2);
        Output o = s.shutdown(kStart + 3);
        CHECK(o.effects.empty() && o.commands.empty() && s.stage() == Stage::Closed);
        CHECK(s.tick(kStart + 52).commands.size() == 1);    // the grace periods still run
    }
}

// ---- restart: what the server finds ----

void testRestart() {
    sync::DriverState open;
    open.nextTurn = 5;
    open.openTurn = 4;
    open.session = "s";
    Output first;
    Supervisor s = make(open, &first);
    CHECK(whats(first) == std::vector<What>{What::Cut} && first.effects[0].turn == 4 && first.durable);
    CHECK(contains(first.effects[0].text, "nexus-server restarted"));
    CHECK(s.state().openTurn == 0 && s.state().nextTurn == 5 && s.state().session == "s" && first.commands.empty());

    sync::DriverState waiting;
    waiting.nextTurn = 3;
    waiting.waiting = {"w1", "w2"};
    Supervisor w = make(waiting, &first);
    CHECK(first.effects.empty() && kinds(first) == std::vector<Kind>{Kind::Start});
    Output o = w.message(Hello{1, "h"}, kStart + 1);
    CHECK(sends(o, 3, "w1"));
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"start", testStart},
    {"turns", testTurns},
    {"waiting", testWaiting},
    {"protocol", testProtocol},
    {"deadlines", testDeadlines},
    {"text", testText},
    {"failed_starts", testFailedStarts},
    {"credential", testCredentialAndErrors},
    {"stop", testStop},
    {"shutdown", testShutdown},
    {"restart", testRestart},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0) {
            continue;
        }
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "supervisor_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "supervisor_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("supervisor_test: ok\n");
    return 0;
}
