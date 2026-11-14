// Unit tests of the driver supervisor (src/server/supervisor.h), by rule: starting a driver, turn numbers, prompts that
// wait, every protocol break that ends a driver, every deadline at its edges (one millisecond before, at, after), text
// gathering and splitting, the failed-start count, the credential, errors, stop, shutdown from every phase, and a
// restart's state. Logic only: no processes, no clock (the time is an argument, from 1000), no files.
//
//   supervisor_test           runs every group
//   supervisor_test <group>   runs one

#include <algorithm>
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
    c.configure({"other"}, "/v", "elsewhere", kStart);
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
        CHECK(o.effects[0].text == "Bash: ls -" && entryKind(What::Tool) == EntryKind::Tool);
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

// ---- activity: the window's state cursor ----

// Busy from the prompt until the turn's done (the start, the turn, the stop); Idle with nothing running, waiting or to
// be taken; Down after a driver ends before it finished a turn, while starting is held back, and with no credential;
// a driver ended for an account change stays Idle; the next prompt makes it Busy again.
void testActivity() {
    using sync::Activity;
    Supervisor s = make();
    CHECK(s.activity() == Activity::Idle);
    s.prompt("x", kStart);
    CHECK(s.phase() == Phase::Starting && s.activity() == Activity::Busy);
    s.message(Hello{1, "h"}, kStart + 1);
    CHECK(s.phase() == Phase::Working && s.activity() == Activity::Busy);
    s.stop(kStart + 2);
    CHECK(s.phase() == Phase::Stopping && s.activity() == Activity::Busy);
    s.message(Done{1, Outcome::Stopped, std::nullopt, std::nullopt}, kStart + 3);
    CHECK(s.phase() == Phase::Idle && s.activity() == Activity::Idle);
    // A prompt queued while a turn is open: still Busy at the turn's end, since the queued one is taken at once.
    s.prompt("y", kStart + 4);
    s.queued("q", kStart + 5);
    s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 6);
    CHECK(s.state().openTurn == 3 && s.activity() == Activity::Busy);
    s.message(Done{3, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 7);
    CHECK(s.activity() == Activity::Idle);
    // A crash mid-turn: Down, while it is being ended and after; the next prompt is Busy again.
    s.prompt("z", kStart + 8);
    s.refused("bad line", kStart + 9);
    CHECK(s.phase() == Phase::Ending && s.activity() == Activity::Down);
    s.exited("exited with status 1", kStart + 10);
    CHECK(s.phase() == Phase::None && s.crashed() && s.activity() == Activity::Down);
    s.prompt("again", kStart + 11);
    CHECK(s.activity() == Activity::Busy);
    // A driver that says hello is up: no longer crashed, Idle once nothing runs (here the start is stopped).
    {
        Supervisor c = s;
        c.stop(kStart + 12);
        CHECK(c.state().waiting.empty() && c.phase() == Phase::Starting && c.activity() == Activity::Busy);
        c.message(Hello{1, "h"}, kStart + 13);
        CHECK(c.phase() == Phase::Idle && !c.crashed() && c.activity() == Activity::Idle);
    }
    // Failed starts in a row, then held: Down, with the prompt still waiting.
    for (Millis at = kStart + 12; !s.held(); ++at) {
        s.exited("exited with status 2", at);
    }
    CHECK(!s.state().waiting.empty() && s.activity() == Activity::Down);
    // No credential: Down at once, even while the turn it ends is still open.
    Supervisor n = working("w");
    n.message(Error{"not signed in", "sign in", 1, ErrorCode::NoCredential}, kStart + 3);
    CHECK(n.held() && n.state().openTurn == 1 && n.activity() == Activity::Down);
    // An account change ends an idle driver quietly: never Down.
    Supervisor a = working("w");
    a.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 3);
    a.setAccount("work", kStart + 4);
    CHECK(a.retiring() && a.activity() == Activity::Idle);
    a.exited("exited with status 0", kStart + 5);
    CHECK(a.activity() == Activity::Idle);
    // The server stopping is not the friend being down.
    Supervisor d = working("w");
    d.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 3);
    d.shutdown(kStart + 4);
    CHECK(d.phase() == Phase::Ending && d.activity() == Activity::Idle);
}

// ---- the session's model, effort and account ----

// The model and effort go to the driver as a settings line written just before the send that first runs with them
// (the driver applies it from its next send): a change applies from the next turn written, never to a turn already
// open, and writes and starts nothing when it is made. A new driver starts on the harness's defaults, so it is
// told the session's again before its first send. A change that puts a field back to the harness's default, which a
// settings line cannot say, ends the driver once no turn is open, quietly, and the next starts on the defaults.
void testSettings() {
    Supervisor s = make();
    CHECK(s.setOptions("opus", "high", kStart).commands.empty());
    s.prompt("one", kStart);
    Output o = s.message(Hello{1, "h"}, kStart + 1);
    CHECK((written(o) == std::vector<ServerMessage>{Settings{"opus", "high"}, Send{1, "one"}}));

    // Mid-turn: nothing happens, and the open turn keeps what it was written with.
    o = s.setOptions("sonnet", "high", kStart + 2);
    CHECK(o.commands.empty() && o.effects.empty() && !o.durable);
    CHECK(s.phase() == Phase::Working && s.state().openTurn == 1);
    CHECK(s.model() == "sonnet" && s.effort() == "high");
    s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 3);
    o = s.prompt("two", kStart + 4);
    CHECK((written(o) == std::vector<ServerMessage>{Settings{"sonnet", ""}, Send{2, "two"}}));

    // Unchanged: no settings line.
    s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 5);
    o = s.prompt("three", kStart + 6);
    CHECK((written(o) == std::vector<ServerMessage>{Send{3, "three"}}));

    // Back to the harness's own effort: the idle driver is ended quietly, never a failed start, and the next one is
    // told the model alone.
    s.message(Done{3, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 7);
    o = s.setOptions("sonnet", "", kStart + 8);
    CHECK(ended(s, o) && o.effects.empty() && s.retiring());
    o = s.exited("exited with status 0", kStart + 9);
    CHECK(s.phase() == Phase::None && s.failedStarts() == 0 && o.effects.empty());
    s.prompt("four", kStart + 10);
    o = s.message(Hello{1, "h"}, kStart + 11);
    CHECK((written(o) == std::vector<ServerMessage>{Settings{"sonnet", ""}, Send{4, "four"}}));

    // Back to the defaults with a turn open: the turn runs to its end first.
    o = s.setOptions("", "", kStart + 12);
    CHECK(o.commands.empty() && s.phase() == Phase::Working);
    o = s.message(Done{4, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 13);
    CHECK(ended(s, o) && s.retiring());
    s.exited("exited with status 0", kStart + 14);
    s.prompt("five", kStart + 15);
    o = s.message(Hello{1, "h"}, kStart + 16);
    CHECK((written(o) == std::vector<ServerMessage>{Send{5, "five"}}));

    // A prompt that waits while the driver starts goes with the setting as it is when it is written, not when it was
    // sent.
    Supervisor t = make();
    t.prompt("w", kStart);
    t.setOptions("haiku", "low", kStart + 1);
    o = t.message(Hello{1, "h"}, kStart + 2);
    CHECK((written(o) == std::vector<ServerMessage>{Settings{"haiku", "low"}, Send{1, "w"}}));
}

// A driver runs under the account it was started with. A change ends an idle driver quietly at once, and a busy one
// once its turn ends; the next starts under the new account. Ending it so is never a failed start.
void testAccount() {
    // Idle: ended at once, quietly.
    Supervisor s = make();
    s.prompt("one", kStart);
    s.message(Hello{1, "h"}, kStart + 1);
    s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
    CHECK(s.phase() == Phase::Idle && s.driverAccount().empty());
    Output o = s.setAccount("work", kStart + 3);
    CHECK(ended(s, o) && o.effects.empty() && s.retiring());
    CHECK(s.account() == "work");
    o = s.exited("exited with status 0", kStart + 4);
    CHECK(s.phase() == Phase::None && s.failedStarts() == 0 && !s.retiring() && o.effects.empty());
    o = s.prompt("two", kStart + 5);
    CHECK(kinds(o) == std::vector<Kind>{Kind::Start} && s.driverAccount() == "work");

    // The same account again changes nothing.
    s.message(Hello{1, "h"}, kStart + 6);
    s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 7);
    o = s.setAccount("work", kStart + 8);
    CHECK(o.commands.empty() && s.phase() == Phase::Idle);

    // Working: the turn runs to its end; then the driver is ended, and a prompt queued meanwhile goes to the next.
    Supervisor t = working("first");
    o = t.setAccount("home", kStart + 2);
    CHECK(o.commands.empty() && o.effects.empty() && t.phase() == Phase::Working);
    t.queued("later", kStart + 3);
    o = t.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 4);
    CHECK(o.taken == 1 && ended(t, o) && t.state().waiting == std::vector<std::string>{"later"});
    CHECK(written(o).empty());     // never written to the driver under the old account
    o = t.exited("exited with status 0", kStart + 5);
    CHECK(kinds(o) == std::vector<Kind>{Kind::Start} && t.driverAccount() == "home" && t.failedStarts() == 0);
    o = t.message(Hello{1, "h"}, kStart + 6);
    CHECK((written(o) == std::vector<ServerMessage>{Send{2, "later"}}));

    // Starting: the driver says hello under the old account, so it is ended before it is given the prompt.
    Supervisor u = make();
    u.prompt("p", kStart);
    u.setAccount("other", kStart + 1);
    o = u.message(Hello{1, "h"}, kStart + 2);
    CHECK(ended(u, o) && written(o).empty() && u.state().waiting.size() == 1);
    o = u.exited("exited with status 0", kStart + 3);
    CHECK(kinds(o) == std::vector<Kind>{Kind::Start} && u.driverAccount() == "other" && u.failedStarts() == 0);
}

bool hasArgs(const Output& o, const std::vector<std::string>& args) {
    for (const Command& c : o.commands) {
        if (c.kind == Kind::Start &&
                std::search(c.argv.begin(), c.argv.end(), args.begin(), args.end()) != c.argv.end()) {
            return true;
        }
    }
    return false;
}

bool resumes(const Output& o) {
    return hasArgs(o, {"--resume"});
}

void testPlace() {
    Config c = config();
    int summaries = 0;
    c.summarize = [&summaries] {
        ++summaries;
        return std::string("SUM ");
    };
    Output o;
    Supervisor s(c, {}, kStart, &o);
    // A driver in /w reports its harness session: kept with its directory.
    s.prompt("one", kStart);
    s.message(Hello{1, "h"}, kStart + 1);
    o = s.message(Session{"s-1"}, kStart + 2);
    CHECK(o.durable && s.state().session == "s-1" && s.state().sessionDir == "/w");
    s.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 3);
    CHECK(s.phase() == Phase::Idle && s.driverCwd() == "/w");

    // The same directory, still here: nothing changes.
    o = s.setPlace("/w", true, kStart + 4);
    CHECK(o.commands.empty() && o.effects.empty() && s.phase() == Phase::Idle);

    // Another directory: the idle driver is ended quietly, not counted as a failed start. The next starts there,
    // afresh (the harness session was made in /w), with a notice; its first send carries the summary, once.
    o = s.setPlace("/v", true, kStart + 5);
    CHECK(ended(s, o) && s.retiring() && o.effects.empty() && s.cwd() == "/v");
    o = s.exited("exited with status 0", kStart + 6);
    CHECK(s.phase() == Phase::None && s.failedStarts() == 0 && o.commands.empty());
    o = s.prompt("two", kStart + 7);
    CHECK(hasArgs(o, {"--cwd", "/v"}) && !resumes(o) && s.driverCwd() == "/v");
    CHECK(o.durable && s.state().session.empty() && s.state().sessionDir.empty() && s.state().summary);
    CHECK(o.effects.size() == 2 && o.effects[1].what == What::Notice);
    CHECK(contains(o.effects[1].text, "ada now works in /v, and the conversation so far was held in /w"));
    CHECK(contains(o.effects[1].text, "starts afresh, given a summary of it with the next prompt"));
    CHECK(summaries == 0);
    o = s.message(Hello{1, "h"}, kStart + 8);
    CHECK((written(o) == std::vector<ServerMessage>{Send{2, "SUM two"}}));
    CHECK(o.durable && !s.state().summary && summaries == 1);
    s.message(Session{"s-2"}, kStart + 9);
    CHECK(s.state().sessionDir == "/v");
    s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 10);

    // A restart in the same directory (here, for an account) resumes, with no summary.
    o = s.setAccount("work", kStart + 11);
    CHECK(ended(s, o));
    s.exited("exited with status 0", kStart + 12);
    o = s.prompt("three", kStart + 13);
    CHECK(hasArgs(o, {"--cwd", "/v", "--resume", "s-2"}) && o.effects.size() == 1);
    o = s.message(Hello{1, "h"}, kStart + 14);
    CHECK((written(o) == std::vector<ServerMessage>{Send{3, "three"}}) && summaries == 1);

    // Working: a move waits for the turn's end; the driver is then ended, never given another prompt in /v.
    o = s.setPlace("/u", true, kStart + 15);
    CHECK(o.commands.empty() && s.phase() == Phase::Working);
    s.queued("later", kStart + 16);
    o = s.message(Done{3, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 17);
    CHECK(o.taken == 1 && ended(s, o) && written(o).empty());

    // On another machine: the idle driver is ended, and no driver starts and nothing queued is taken until the
    // instance is here again.
    Supervisor u = make();
    u.prompt("one", kStart);
    u.message(Hello{1, "h"}, kStart + 1);
    u.message(Done{1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 2);
    o = u.setPlace("/w", false, kStart + 3);
    CHECK(ended(u, o) && !u.here());
    u.exited("exited with status 0", kStart + 4);
    o = u.queued("later", kStart + 5);
    CHECK(o.taken == 0 && o.commands.empty());
    o = u.setPlace("/w", true, kStart + 6);
    CHECK(o.taken == 1 && kinds(o) == std::vector<Kind>{Kind::Start});

    // A harness session from before its directory was kept (none recorded) is resumed; with no turns yet, a move
    // gives no summary and no notice.
    sync::DriverState old;
    old.nextTurn = 3;
    old.session = "old";
    Supervisor v = make(old);
    o = v.prompt("p", kStart);
    CHECK(hasArgs(o, {"--resume", "old"}));
    Supervisor w = make();
    w.setPlace("/v", true, kStart);
    o = w.prompt("p", kStart + 1);
    CHECK(hasArgs(o, {"--cwd", "/v"}) && o.effects.size() == 1 && !w.state().summary);
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
    {
        // A stop while the driver starts cancels the turn about to begin: its prompt is never written, and the turn
        // number is not used. The driver keeps starting; the next prompt waiting, or queued, goes once it is up.
        Supervisor starting = make();
        starting.prompt("x", kStart);
        starting.prompt("y", kStart);
        starting.queued("q", kStart);
        Output o = starting.stop(kStart + 1);
        CHECK(whats(o) == std::vector<What>{What::Stopped} && o.effects[0].turn == 0 && o.durable);
        CHECK(o.commands.empty() && starting.phase() == Phase::Starting);
        CHECK(starting.state().waiting == std::vector<std::string>{"y"} && starting.queue().size() == 1);
        o = starting.stop(kStart + 2);
        CHECK(whats(o) == std::vector<What>{What::Stopped} && starting.state().waiting.empty());
        o = starting.message(Hello{1, "h"}, kStart + 3);
        CHECK(o.taken == 1 && sends(o, 1, "q") && starting.queue().empty());
    }
    {
        // A stop while starting with nothing left waiting changes nothing; the cancelled prompt never comes back: not
        // when the start fails (a failed start, and no other started, as nothing waits), nor after a restart.
        Supervisor s = make();
        s.prompt("x", kStart);
        s.stop(kStart + 1);
        Output o = s.stop(kStart + 2);
        CHECK(o.effects.empty() && o.commands.empty() && !o.durable && s.phase() == Phase::Starting);
        o = s.exited("exited with status 1", kStart + 3);
        CHECK(s.failedStarts() == 1 && o.commands.empty() && s.phase() == Phase::None);
        Output first;
        Supervisor r(config(), s.state(), kStart + 4, &first);
        CHECK(first.effects.empty() && first.commands.empty() && r.state().waiting.empty());
    }
    {
        // Nothing running, with a prompt waiting: a driver being ended (a new one starts for it once it is gone), or
        // starting held back after failed starts. The stop changes nothing, and the prompt still goes.
        Supervisor ending = working();
        ending.prompt("b", kStart + 2);
        ending.refused("line 3: not JSON", kStart + 3);
        CHECK(ending.phase() == Phase::Ending);
        Output o = ending.stop(kStart + 4);
        CHECK(o.effects.empty() && o.commands.empty() && !o.durable);
        CHECK(ending.state().waiting == std::vector<std::string>{"b"});
        Supervisor held = make();
        held.prompt("x", kStart);
        for (int i = 0; i < 3; ++i) {
            held.exited("could not be started: no such file", kStart + 1);
        }
        CHECK(held.held() && held.phase() == Phase::None);
        o = held.stop(kStart + 2);
        CHECK(o.effects.empty() && o.commands.empty() && !o.durable);
        CHECK(held.state().waiting == std::vector<std::string>{"x"});
    }
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

// Stops hammered, in order, with prompts queued: each ends the turn running when it arrives, those that find the turn
// already stopping or nothing running change nothing, and none is kept for a later turn. Nothing queued is lost.
void testStopHammered() {
    Supervisor s = working("go");
    for (const char* q : {"q1", "q2", "q3"}) {
        s.queued(q, kStart + 2);
    }
    std::uint64_t turn = 1;
    for (int round = 0; round < 3; ++round) {
        Output o = s.stop(kStart + 3);
        CHECK(written(o) == std::vector<ServerMessage>{Stop{turn}});
        for (int i = 0; i < 4; ++i) {
            const sync::DriverState before = s.state();
            o = s.stop(kStart + 4);
            CHECK(o.effects.empty() && o.commands.empty() && !o.durable && s.state().waiting == before.waiting);
        }
        o = s.message(Done{turn, Outcome::Stopped, std::nullopt, std::nullopt}, kStart + 5);
        ++turn;
        CHECK(o.taken == 1 && sends(o, turn, "q" + std::to_string(round + 1)));
        CHECK(s.queue().size() == static_cast<std::size_t>(2 - round));
    }
    Output o = s.message(Done{turn, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 6);
    CHECK(o.taken == 0 && s.phase() == Phase::Idle);
    for (int i = 0; i < 5; ++i) {
        o = s.stop(kStart + 7);
        CHECK(o.effects.empty() && o.commands.empty() && !o.durable);
    }
    o = s.prompt("last", kStart + 8);
    CHECK(sends(o, turn + 1, "last"));
    o = s.message(Done{turn + 1, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 9);
    CHECK(whats(o).empty() && s.phase() == Phase::Idle && s.state().nextTurn == turn + 2);
}

// ---- the queue: one per turn, at the turn's end, oldest first, each once ----

void testQueue() {
    const Done ok{1, Outcome::Ok, std::nullopt, std::nullopt};
    {
        // No turn open and nothing waiting: taken at once, and a driver started for it.
        Supervisor s = make();
        Output o = s.queued("q1", kStart);
        CHECK(o.taken == 1 && whats(o) == std::vector<What>{What::Prompt} && o.effects[0].text == "q1" && o.durable);
        CHECK(s.queue().empty() && s.state().waiting == std::vector<std::string>{"q1"});
        CHECK(kinds(o) == std::vector<Kind>{Kind::Start});
        o = s.message(Hello{1, "h"}, kStart + 1);
        CHECK(sends(o, 1, "q1") && o.taken == 0);
    }
    {
        // While a turn is open the queue holds; each turn's end takes the oldest, and writes it as the next turn.
        Supervisor s = working();
        Output o = s.queued("q1", kStart + 2);
        CHECK(o.taken == 0 && o.effects.empty() && o.commands.empty() && !o.durable);
        o = s.queued("q2", kStart + 3);
        CHECK(o.taken == 0 && (s.queue() == std::vector<std::string>{"q1", "q2"}));
        o = s.message(ok, kStart + 4);
        CHECK(o.taken == 1 && whats(o) == std::vector<What>{What::Prompt} && o.effects[0].text == "q1");
        CHECK(sends(o, 2, "q1") && s.state().openTurn == 2 && s.queue() == std::vector<std::string>{"q2"});
        o = s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 5);
        CHECK(o.taken == 1 && sends(o, 3, "q2") && s.queue().empty());
        o = s.message(Done{3, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 6);
        CHECK(o.taken == 0 && o.commands.empty() && s.phase() == Phase::Idle);
    }
    {
        // A prompt sent while a turn is open goes before the queue: it was sent, the queue is for later.
        Supervisor s = working();
        s.queued("q1", kStart + 2);
        s.prompt("s1", kStart + 3);
        Output o = s.message(ok, kStart + 4);
        CHECK(o.taken == 0 && sends(o, 2, "s1") && s.queue() == std::vector<std::string>{"q1"});
        o = s.message(Done{2, Outcome::Ok, std::nullopt, std::nullopt}, kStart + 5);
        CHECK(o.taken == 1 && sends(o, 3, "q1"));
    }
    {
        // A stopped turn is a turn's end: the next queued prompt goes.
        Supervisor s = working();
        s.queued("q1", kStart + 2);
        Output o = s.stop(kStart + 3);
        CHECK(o.taken == 0 && written(o) == std::vector<ServerMessage>{Stop{1}});
        o = s.message(Done{1, Outcome::Stopped, std::nullopt, std::nullopt}, kStart + 4);
        CHECK((whats(o) == std::vector<What>{What::Stopped, What::Prompt}) && o.taken == 1 && sends(o, 2, "q1"));
    }
    {
        // So is a turn cut by the driver's exit: the prompt waits for the next driver, which is started for it.
        Supervisor s = working();
        s.queued("q1", kStart + 2);
        Output o = s.exited("exited with status 1", kStart + 3);
        CHECK((whats(o) == std::vector<What>{What::Notice, What::Cut, What::Prompt}) && o.taken == 1);
        CHECK(s.state().waiting == std::vector<std::string>{"q1"} && kinds(o) == std::vector<Kind>{Kind::Start});
    }
    {
        // Queuing lifts the hold that failed starts put on starting drivers, as a prompt does.
        Supervisor s = make();
        s.prompt("x", kStart);
        for (int i = 0; i < 3; ++i) {
            s.exited("could not be started: no such file", kStart + 1);
        }
        CHECK(s.held() && s.state().waiting == std::vector<std::string>{"x"});
        Output o = s.queued("q1", kStart + 2);
        CHECK(!s.held() && s.failedStarts() == 0 && o.taken == 0 && kinds(o) == std::vector<Kind>{Kind::Start});
        CHECK(s.queue() == std::vector<std::string>{"q1"});     // "x" waits ahead of it
    }
    {
        // With no driver configured nothing is taken, at a turn's end or a restart, and the friend shows down once no
        // turn runs: the queue stays as it is until a driver is configured again, which takes the oldest. A prompt
        // still waiting fails its start, honestly.
        Supervisor s = working();
        Output o = s.configure({}, "/w", "the test's table", kStart + 1);
        CHECK(o.taken == 0 && o.commands.empty() && s.activity() == sync::Activity::Busy);
        s.queued("q1", kStart + 2);
        o = s.message(ok, kStart + 3);
        CHECK(o.taken == 0 && o.commands.empty() && s.queue() == std::vector<std::string>{"q1"});
        CHECK(s.activity() == sync::Activity::Down);
        o = s.configure({"drv"}, "/w", "the test's table", kStart + 4);
        CHECK(o.taken == 1 && sends(o, 2, "q1") && s.queue().empty() && s.activity() == sync::Activity::Busy);
        Config none = config();
        none.command.clear();
        sync::DriverState open;
        open.nextTurn = 4;
        open.openTurn = 3;
        Output first;
        Supervisor r(none, open, kStart, &first, {"q1"});
        CHECK(whats(first) == std::vector<What>{What::Cut} && first.taken == 0 && first.commands.empty());
        CHECK(r.queue() == std::vector<std::string>{"q1"} && r.state().waiting.empty());
        Output started;
        Supervisor w(none, sync::DriverState{}, kStart, &started);
        CHECK(w.activity() == sync::Activity::Down);     // no driver configured: down, not waiting for the person
        o = w.prompt("x", kStart);
        CHECK(kinds(o) == std::vector<Kind>{Kind::Start} && o.commands[0].argv.empty());
    }
    {
        // A server stopping takes nothing from the queue: it stays, durable, for the next server.
        Supervisor s = make();
        s.shutdown(kStart);
        Output o = s.queued("q1", kStart + 1);
        CHECK(o.taken == 0 && o.effects.empty() && o.commands.empty() && s.queue() == std::vector<std::string>{"q1"});
    }
    {
        // A restart cuts the open turn, and that end takes the oldest queued prompt.
        sync::DriverState open;
        open.nextTurn = 4;
        open.openTurn = 3;
        Output first;
        Supervisor s(config(), open, kStart, &first, {"q1", "q2"});
        CHECK((whats(first) == std::vector<What>{What::Cut, What::Prompt}) && first.taken == 1 && first.durable);
        CHECK(s.queue() == std::vector<std::string>{"q2"} && kinds(first) == std::vector<Kind>{Kind::Start});
    }
    {
        // A queued prompt that cannot be written as a protocol line (not UTF-8) leaves the queue with a notice, and the
        // next is taken in its place.
        Supervisor s = working();
        s.queued("\xff", kStart + 2);
        s.queued("q2", kStart + 3);
        Output o = s.message(ok, kStart + 4);
        CHECK((whats(o) == std::vector<What>{What::Notice, What::Prompt}) && o.taken == 2 && sends(o, 2, "q2"));
        CHECK(contains(o.effects[0].text, "A queued prompt to ada was not sent") && s.queue().empty());
    }
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
    {"stop_hammered", testStopHammered},
    {"queue", testQueue},
    {"shutdown", testShutdown},
    {"restart", testRestart},
    {"settings", testSettings},
    {"account", testAccount},
    {"place", testPlace},
    {"activity", testActivity},
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
