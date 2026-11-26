// Unit tests of the relay (src/relay/relay_wire.h, src/relay/relay_ends.h): the frames cut from a stream however it
// arrives, the hello each end checks, an action and a history request across, the link's state across as diffs (the
// window's state after each frame equal to the relay's link's, through formats and sessions coming and going), and the
// window's end across a relay that ended and one started after it; and the window's side of the relays' life
// (src/relay/relay_life.h): the backoff, one relay at a time, the window's numbers across relays and the actions a relay
// that ended cut. Logic only: no process, no pipe, no time.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "relay/relay_ends.h"
#include "relay/relay_life.h"
#include "model.h"
#include "relay/relay_wire.h"
#include "relay/relay_words.h"
#include "sync/formats.h"

namespace {

using namespace nexus;
using namespace nexus::relay;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

sync::EntryRef entry(EntryKind kind, std::string text, bool closed = true) {
    auto e = std::make_shared<sync::SessionEntry>();
    e->kind = kind;
    e->text = std::move(text);
    e->closed = closed;
    return e;
}

sync::SessionRef view(sync::SessionId id, const std::string& name, std::vector<sync::EntryRef> entries,
                      std::vector<std::string> queue = {}, std::uint64_t first = 0) {
    auto v = std::make_shared<sync::SessionView>();
    v->id = id;
    v->info.aiName = name;
    v->info.harness = "claude";
    v->first = first;
    v->entries = std::move(entries);
    v->queue = std::move(queue);
    return v;
}

sync::Sessions sessions(std::vector<sync::SessionRef> views, std::uint32_t format = sync::kStateFormat) {
    sync::Sessions s(std::move(views));
    s.setFormat(format);
    return s;
}

sync::Action action(sync::ActionKind kind, sync::SessionId session, std::string text) {
    sync::Action a;
    a.kind = kind;
    a.session = session;
    a.text = std::move(text);
    return a;
}

// Every frame in `bytes`, fed `step` bytes at a time.
std::vector<Frame> cut(const std::string& bytes, std::size_t step) {
    FrameReader reader;
    std::vector<Frame> out;
    for (std::size_t at = 0; at < bytes.size(); at += step) {
        reader.feed(bytes.data() + at, std::min(step, bytes.size() - at));
        Frame f;
        std::string why;
        while (reader.next(&f, &why)) out.push_back(f);
        CHECK(why.empty());
    }
    return out;
}

void frames_cut_anywhere() {
    const std::string bytes = frameBytes(FrameKind::Hello, "a") + frameBytes(FrameKind::State, std::string(70000, 'x')) +
        frameBytes(FrameKind::Action, "") + frameBytes(FrameKind::History, "zz");
    for (std::size_t step : {std::size_t(1), std::size_t(3), std::size_t(4096), bytes.size()}) {
        const std::vector<Frame> frames = cut(bytes, step);
        CHECK(frames.size() == 4);
        if (frames.size() != 4) continue;
        CHECK(frames[0].kind == FrameKind::Hello && frames[0].body == "a");
        CHECK(frames[1].kind == FrameKind::State && frames[1].body == std::string(70000, 'x'));
        CHECK(frames[2].kind == FrameKind::Action && frames[2].body.empty());
        CHECK(frames[3].kind == FrameKind::History && frames[3].body == "zz");
    }
}

void frame_refusals() {
    std::string why;
    Frame f;
    FrameReader unknown;
    const std::string bad("\x09\x00\x00\x00\x00", 5);
    unknown.feed(bad.data(), bad.size());
    CHECK(!unknown.next(&f, &why) && why.find("kind 9") != std::string::npos);
    CHECK(unknown.failed());
    // Once failed, it stays failed, whatever follows.
    const std::string good = frameBytes(FrameKind::Hello, "");
    unknown.feed(good.data(), good.size());
    CHECK(!unknown.next(&f, &why) && !why.empty());

    FrameReader huge;
    std::string header(1, static_cast<char>(FrameKind::State));
    const std::uint32_t size = kMaxBody + 1;
    for (int i = 0; i < 4; ++i) header.push_back(static_cast<char>((size >> (8 * i)) & 0xff));
    huge.feed(header.data(), header.size());
    CHECK(!huge.next(&f, &why) && why.find("bytes, over") != std::string::npos);

    // A partial frame is no frame yet, and no failure.
    FrameReader partial;
    const std::string some = frameBytes(FrameKind::Action, "body").substr(0, 7);
    partial.feed(some.data(), some.size());
    CHECK(!partial.next(&f, &why) && why.empty() && !partial.failed());
}

void hello_checks() {
    std::string why;
    const std::vector<Frame> mine = cut(helloFrame(), 1);
    CHECK(mine.size() == 1 && mine[0].kind == FrameKind::Hello);
    CHECK(checkHello(mine[0].body, "this window", "nexus-link-relay in WSL2", &why));
    // Another version: refused, saying both ends' versions and what to do.
    std::string other = mine[0].body;
    other[11] = static_cast<char>(kRelayVersion + 1);
    CHECK(!checkHello(other, "this window", "nexus-link-relay in WSL2", &why));
    CHECK(why.find("relay version " + std::to_string(kRelayVersion + 1)) != std::string::npos);
    CHECK(why.find("relay version " + std::to_string(kRelayVersion) + ",") != std::string::npos);
    CHECK(why.find("install the same nexus") != std::string::npos);
    // Another state format: refused too.
    std::string format = mine[0].body;
    format[15] = static_cast<char>(sync::kStateFormat + 1);
    CHECK(!checkHello(format, "this window", "nexus-link-relay in WSL2", &why));
    CHECK(!checkHello("not a hello at all", "a", "b", &why) && why.find("not the relay's hello") != std::string::npos);
    CHECK(!checkHello(mine[0].body + "x", "a", "b", &why));
}

void actions_across() {
    std::vector<sync::Action> sent = {action(sync::ActionKind::Send, 3, "hello across WSL2"),
        action(sync::ActionKind::Stop, 3, "")};
    sync::Action create = action(sync::ActionKind::Create, 0, "");
    create.aiName = "ada";
    sent.push_back(create);
    sync::Action secret = action(sync::ActionKind::SecretAnswer, 0, "");
    secret.request = 42;
    secret.secret = sync::SecretValue(std::string("not a real value"));
    sent.push_back(secret);
    for (const sync::Action& a : sent) {
        const std::vector<Frame> frames = cut(actionFrame(a), 2);
        CHECK(frames.size() == 1 && frames[0].kind == FrameKind::Action);
        if (frames.size() != 1) continue;
        sync::Action read;
        std::string why;
        CHECK(readAction(frames[0].body, &read, &why));
        sync::Action want = a;
        want.num = read.num;    // the relay's link numbers it
        CHECK(read == want);
    }
    sync::Action read;
    std::string why;
    CHECK(!readAction("garbage", &read, &why) && !why.empty());

    const std::vector<Frame> h = cut(historyFrame(7, 3, 120), 5);
    CHECK(h.size() == 1);
    if (h.size() == 1) {
        sync::SessionId id = 0;
        std::uint64_t epoch = 0, before = 0;
        CHECK(readHistory(h[0].body, &id, &epoch, &before, &why) && id == 7 && epoch == 3 && before == 120);
        CHECK(!readHistory(h[0].body + "x", &id, &epoch, &before, &why));
    }
}

// The link's states in a run: a session arriving, its open entry growing, a queue, a second session, the first gone,
// the format changing (a server of another format after a redial), a failure with the sessions kept.
std::vector<LinkState> run() {
    std::vector<LinkState> states;
    LinkState s;
    states.push_back(s);
    s.phase = sync::LinkPhase::Connected;
    s.heardMsAgo = 12;
    s.loopback = true;
    s.sessions = sessions({view(1, "ada", {entry(EntryKind::Person, "hello")})});
    states.push_back(s);
    s.sessions = sessions({view(1, "ada", {entry(EntryKind::Person, "hello"),
        entry(EntryKind::AI, "hel", false)})});
    states.push_back(s);
    s.sessions = sessions({view(1, "ada", {entry(EntryKind::Person, "hello"),
        entry(EntryKind::AI, "hello back", true)}, {"for later"})});
    states.push_back(s);
    s.sessions = sessions({view(1, "ada", {entry(EntryKind::Person, "hello"),
        entry(EntryKind::AI, "hello back", true)}, {"for later"}), view(2, "bo", {})});
    states.push_back(s);
    s.sessions = sessions({view(2, "bo", {entry(EntryKind::Notice, "bo's notice")})});
    states.push_back(s);
    s.phase = sync::LinkPhase::Connecting;
    states.push_back(s);
    s.phase = sync::LinkPhase::Connected;
    s.sessions = sessions({view(2, "bo", {entry(EntryKind::Notice, "bo's notice")})}, sync::kTablesFormat);
    states.push_back(s);
    s.sessions = sessions({view(2, "bo", {entry(EntryKind::Notice, "bo's notice"),
        entry(EntryKind::Person, "in format 4")})}, sync::kTablesFormat);
    states.push_back(s);
    s.phase = sync::LinkPhase::Failed;
    s.problem = "`ssh studio` ended with 255";
    states.push_back(s);
    return states;
}

bool same(const LinkState& a, const LinkState& b) {
    return a.phase == b.phase && a.problem == b.problem && a.heardMsAgo == b.heardMsAgo && a.loopback == b.loopback &&
        a.sessions == b.sessions && a.sessions.format() == b.sessions.format();
}

void states_across() {
    StateWriter writer;
    StateReader reader;
    for (const LinkState& s : run()) {
        const std::vector<Frame> frames = cut(writer.frame(s), 7);
        CHECK(frames.size() == 1 && frames[0].kind == FrameKind::State);
        if (frames.size() != 1) continue;
        std::string why;
        CHECK(reader.apply(frames[0].body, &why));
        CHECK(why.empty());
        CHECK(same(reader.state(), s));
    }
}

// A diff is applied only over the sessions it was made from: before any whole state, or over others, it changes
// nothing and says why.
void state_refusals() {
    StateWriter writer;
    const std::vector<LinkState> states = run();
    const std::vector<Frame> first = cut(writer.frame(states[1]), 64);
    const std::vector<Frame> second = cut(writer.frame(states[2]), 64);
    CHECK(first.size() == 1 && second.size() == 1);
    if (first.size() != 1 || second.size() != 1) return;
    std::string why;
    StateReader fresh;
    CHECK(!fresh.apply(second[0].body, &why) && why.find("before any whole one") != std::string::npos);
    CHECK(!fresh.apply("\x01", &why) && why.find("does not parse") != std::string::npos);
    StateReader reader;
    CHECK(reader.apply(first[0].body, &why));
    CHECK(reader.apply(second[0].body, &why));
    // A diff over a reader that holds a whole state of another format is refused, and changes nothing.
    StateWriter older;
    LinkState four = states[1];
    four.sessions.setFormat(sync::kTablesFormat);
    const std::vector<Frame> whole4 = cut(older.frame(four), 64);
    StateReader reader4;
    CHECK(whole4.size() == 1 && reader4.apply(whole4[0].body, &why));
    CHECK(!reader4.apply(second[0].body, &why) && why.find("from sessions held in 4") != std::string::npos);
    CHECK(same(reader4.state(), four));
    CHECK(same(reader.state(), states[2]));
}

// Both ends, as the window and the relay use them: hellos first, actions down in order, states up; a relay that ends
// leaves the window's sessions as they were, Failed with why, until the next relay's first state (the numbers are
// RelayLife's: life_across).
void ends_across() {
    RelayClient window;
    RelayServer relay;
    std::string why;
    const std::string hello = window.open();
    CHECK(relay.feed(hello.data(), hello.size(), &why));
    const std::string back = relay.open();
    CHECK(window.feed(back.data(), back.size(), &why));
    LinkState shown;
    CHECK(window.poll(&shown) && shown.phase == sync::LinkPhase::Connecting);
    CHECK(!window.poll(&shown));

    std::string down = window.send(action(sync::ActionKind::Send, 1, "one"));
    down += window.send(action(sync::ActionKind::Queue, 1, "two"));
    down += window.history(1, 0, 40);
    for (std::size_t at = 0; at < down.size(); ++at) CHECK(relay.feed(down.data() + at, 1, &why));
    const std::vector<RelayServer::Request> got = relay.take();
    CHECK(got.size() == 3);
    if (got.size() == 3) {
        CHECK(got[0].kind == RelayServer::Request::Kind::Action && got[0].action.text == "one");
        CHECK(got[1].kind == RelayServer::Request::Kind::Action && got[1].action.text == "two" &&
              got[1].action.kind == sync::ActionKind::Queue);
        CHECK(got[2].kind == RelayServer::Request::Kind::History && got[2].session == 1 && got[2].before == 40);
    }
    CHECK(relay.take().empty());

    const std::vector<LinkState> states = run();
    for (std::size_t i = 1; i < 6; ++i) {
        const std::string up = relay.state(states[i]);
        CHECK(window.feed(up.data(), up.size(), &why));
        CHECK(window.poll(&shown) && same(shown, states[i]));
    }

    window.ended("nexus-link-relay in WSL2 ended (exit 1)");
    CHECK(window.poll(&shown) && shown.phase == sync::LinkPhase::Failed);
    CHECK(shown.problem == "nexus-link-relay in WSL2 ended (exit 1)" && shown.sessions == states[5].sessions);

    RelayServer next;
    const std::string again = window.open();
    CHECK(window.poll(&shown) && shown.phase == sync::LinkPhase::Connecting && shown.sessions == states[5].sessions);
    CHECK(next.feed(again.data(), again.size(), &why));
    const std::string nextHello = next.open();
    CHECK(window.feed(nextHello.data(), nextHello.size(), &why));
    // Its first state is whole: the window's sessions become the new relay's.
    const std::string up = next.state(states[7]);
    CHECK(window.feed(up.data(), up.size(), &why));
    CHECK(window.poll(&shown) && same(shown, states[7]));
}

void ends_refuse() {
    std::string why;
    // A relay that sends a state before its hello.
    RelayClient window;
    window.open();
    StateWriter writer;
    const std::string early = writer.frame(run()[1]);
    CHECK(!window.feed(early.data(), early.size(), &why) && why.find("before its hello") != std::string::npos);
    // A window that sends an action before its hello.
    RelayServer relay;
    const std::string act = actionFrame(action(sync::ActionKind::Send, 1, "x"));
    CHECK(!relay.feed(act.data(), act.size(), &why) && why.find("before its hello") != std::string::npos);
    CHECK(relay.take().empty());
    // A relay that sends what only a window sends.
    RelayClient w2;
    const std::string h = helloFrame() + historyFrame(1, 2, 3);
    CHECK(!w2.feed(h.data(), h.size(), &why) && why.find("only a window sends") != std::string::npos);
}


// The relay's state with the server's first unapplied action of the relay's link (sync::Sessions::nextAction).
std::string stateWith(RelayServer* relay, std::uint64_t next, sync::LinkPhase phase = sync::LinkPhase::Connected) {
    LinkState s;
    s.phase = phase;
    s.sessions = sessions({view(1, "ada", {entry(EntryKind::Person, "one")})});
    s.sessions.setNextAction(next);
    return relay->state(s);
}

// What the live relay is handed, all of it: the bytes and the actions' texts the relay reads from them.
std::vector<std::string> drain(RelayLife* life, RelayServer* relay) {
    std::string bytes, all, why;
    while (life->take(&bytes)) all += bytes;
    CHECK(relay->feed(all.data(), all.size(), &why));
    std::vector<std::string> texts;
    for (const RelayServer::Request& r : relay->take())
        if (r.kind == RelayServer::Request::Kind::Action) texts.push_back(r.action.text);
    return texts;
}

// The backoff between relays: the first at once, then 1 s after one ends, doubling to at most 10 s; 1 s again after
// one that ran 10 s or more. Time is an argument: no clock.
void life_backoff() {
    RelayLife life;
    CHECK(life.mayStart(0) && life.waitMs(0) == 0 && life.phase() == RelayLife::Phase::Waiting);
    LifeMs now = 0;
    const std::vector<LifeMs> waits = {1000, 2000, 4000, 8000, 10000, 10000};
    for (const LifeMs wait : waits) {
        CHECK(life.started(now));
        now += 500;
        life.ended("gone", now);
        CHECK(life.phase() == RelayLife::Phase::Waiting && life.leftMs(now) == wait);
        CHECK(!life.mayStart(now + wait - 1) && !life.started(now + wait - 1));
        now += wait;
        CHECK(life.mayStart(now));
    }
    // One that ran 10 s was working: the next waits 1 s, and the one after 2 s.
    CHECK(life.started(now));
    now += kRanWellMs;
    life.ended("gone", now);
    CHECK(life.leftMs(now) == 1000 && life.backoffMs() == 2000);
    now += 1000;
    CHECK(life.started(now));
    life.ended("gone", now + 1);
    CHECK(life.leftMs(now + 1) == 2000);
}

// One relay at a time: no start while one is live, and none after the window's exit.
void life_one_relay() {
    RelayLife life;
    CHECK(life.started(0) && life.live() && life.relays() == 1);
    CHECK(!life.mayStart(100000) && !life.started(100000) && life.relays() == 1);
    life.ended("gone", 10);
    CHECK(!life.live());
    life.ended("gone again", 20);                 // an end reported twice is one end
    CHECK(life.leftMs(20) == 990);
    CHECK(life.started(1010) && life.relays() == 2);
    life.stop();
    CHECK(life.phase() == RelayLife::Phase::Stopped && !life.wantsWrite());
    life.ended("stopped", 2000);
    CHECK(!life.mayStart(1000000) && !life.started(1000000) && life.relays() == 2);
}

// Across relays: the window numbers its actions once, from 1; each relay is handed its Hello first and then what
// waited, in order; the server's acknowledgement comes back in the window's numbers; a relay that ends cuts what it
// had not had acknowledged, in one poll, never handing it to the next.
void life_across() {
    RelayLife life;
    std::string why;
    CHECK(life.send(action(sync::ActionKind::Send, 1, "one")) == 1);
    CHECK(life.send(action(sync::ActionKind::Send, 1, "two")) == 2);
    CHECK(!life.wantsWrite());                    // no relay: they wait

    CHECK(life.started(0));
    RelayServer first;
    std::string bytes;
    CHECK(life.take(&bytes) && bytes == helloFrame());
    CHECK(first.feed(bytes.data(), bytes.size(), &why));
    std::string two;
    CHECK(life.take(&two));
    CHECK(life.take(&bytes));
    two += bytes;
    CHECK(!life.take(&bytes) && life.written() == 2);
    CHECK(first.feed(two.data(), two.size(), &why) && first.take().size() == 2);
    CHECK(life.send(action(sync::ActionKind::Send, 1, "three")) == 3);   // waits: the relay is about to end
    CHECK(life.phase() == RelayLife::Phase::Hello);
    std::string up = first.open() + stateWith(&first, 2);
    CHECK(life.feed(up.data(), up.size(), &why) && life.phase() == RelayLife::Phase::Running);
    CHECK(life.acked() == 1);
    LinkState shown;
    CHECK(life.poll(&shown) && shown.sessions.nextAction() == 2 && shown.cut.empty());

    // The relay ends: the window's 2 was written and not acknowledged; 3 was never written.
    life.ended("nexus-link-relay in WSL2 ended (exit 1)", 100);
    CHECK(life.cut() == std::vector<std::uint64_t>{2});
    CHECK(life.poll(&shown) && shown.cut == std::vector<std::uint64_t>{2} && shown.phase == sync::LinkPhase::Failed);
    CHECK(shown.problem.find("1 action the server had not acknowledged") != std::string::npos);
    CHECK(!life.poll(&shown));                    // in one poll only

    // The next relay: its Hello, then 3, its link's first; never 2.
    CHECK(life.started(1100));
    RelayServer next;
    CHECK(drain(&life, &next) == std::vector<std::string>{"three"});
    CHECK(life.base() == 2 && life.written() == 3);
    up = next.open() + stateWith(&next, 1);
    CHECK(life.feed(up.data(), up.size(), &why) && life.acked() == 2);
    CHECK(life.poll(&shown) && shown.sessions.nextAction() == 3 && shown.cut.empty());
    up = stateWith(&next, 2);
    CHECK(life.feed(up.data(), up.size(), &why) && life.acked() == 3);
    CHECK(life.poll(&shown) && shown.sessions.nextAction() == 4);
    CHECK(life.send(action(sync::ActionKind::Send, 1, "four")) == 4);
    CHECK(drain(&life, &next) == std::vector<std::string>{"four"});
    // Everything acknowledged: an end cuts nothing.
    up = stateWith(&next, 3);
    CHECK(life.feed(up.data(), up.size(), &why) && life.acked() == 4);
    life.ended("gone", 30000);
    CHECK(life.poll(&shown) && shown.cut.empty() && life.cut() == std::vector<std::uint64_t>{2});
    CHECK(shown.problem == "gone");
}

// A relay is not believed past what was written to it; its bytes not the protocol are refused; at the window's exit
// nothing more is written, and what was not acknowledged is not cut (the window is gone, as one that quits).
void life_refusals() {
    RelayLife life;
    std::string why, bytes;
    CHECK(life.started(0));
    life.send(action(sync::ActionKind::Send, 1, "one"));
    RelayServer relay;
    CHECK(drain(&life, &relay).size() == 1);
    std::string up = relay.open() + stateWith(&relay, 9);
    CHECK(life.feed(up.data(), up.size(), &why) && life.acked() == 1);
    const std::string junk = "\x09junk";
    CHECK(!life.feed(junk.data(), junk.size(), &why) && !why.empty());
    life.send(action(sync::ActionKind::Send, 1, "two"));
    life.stop();
    CHECK(!life.take(&bytes) && life.written() == 1);
    life.ended("stopped", 10);
    LinkState shown;
    life.poll(&shown);
    CHECK(shown.cut.empty() && life.cut().empty());
}
} // namespace

// The relay's words: by default a plain sh -c, never a login shell (whose profile would print ahead of the relay's
// Hello), with ~/.local/bin first on the PATH and the relay's arguments through "$@".
void words_default() {
    const std::vector<std::string> local = relayCommand("localhost", {});
    CHECK((local == std::vector<std::string>{"/bin/sh", "-c", kDefaultRelayScript, "nexus-link-relay", "--local"}));
    for (const std::string& w : local) {
        CHECK(w != "-l" && w != "-lc" && w != "--login");
        CHECK(w.find("bash") == std::string::npos);
    }
    const std::string script = kDefaultRelayScript;
    CHECK(script.find("PATH=\"$HOME/.local/bin:$PATH\"; export PATH;") == 0);
    CHECK(script.find("exec nexus-link-relay \"$@\"") != std::string::npos);
    // The default program with what goes after the host.
    const std::vector<std::string> withCommand = relayCommand("box", {"", "--", "env", "A=b c"});
    CHECK((withCommand == std::vector<std::string>{"/bin/sh", "-c", kDefaultRelayScript, "nexus-link-relay",
        "--server", "box", "--", "env", "A=b c"}));
    // A program of the test's.
    CHECK((relayCommand("localhost", {"/tmp/r.sh", "--", "x"}) ==
        std::vector<std::string>{"/tmp/r.sh", "--local", "--", "x"}));
}

// Each word quoted as CommandLineToArgvW reads it back.
void words_quoted() {
    CHECK(windowsCommandLine({"wsl.exe", "-e", "plain"}) == "wsl.exe -e plain");
    CHECK(windowsCommandLine({"a b", ""}) == "\"a b\" \"\"");
    CHECK(windowsCommandLine({"say \"hi\""}) == "\"say \\\"hi\\\"\"");
    CHECK(windowsCommandLine({"a\\\"b"}) == "\"a\\\\\\\"b\"");
    CHECK(windowsCommandLine({"dir with space\\"}) == "\"dir with space\\\\\"");
    CHECK(windowsCommandLine({"no\\space"}) == "no\\space");
    CHECK(windowsCommandLine({"$x 'y'"}) == "\"$x 'y'\"");
}

std::string utf16(const std::string& ascii) {
    std::string out;
    for (const char c : ascii) {
        out += c;
        out += '\0';
    }
    return out;
}

// What a process wrote, as text: wsl.exe's UTF-16 decoded, control bytes as spaces, trimmed and capped.
void words_plain() {
    CHECK(plainText(utf16("The Windows Subsystem for Linux has no installed distributions.\r\n")) ==
        "The Windows Subsystem for Linux has no installed distributions.");
    CHECK(plainText("\xFF\xFE" + utf16("Error code: Wsl/Service/E_UNEXPECTED")) == "Error code: Wsl/Service/E_UNEXPECTED");
    // A line cut after a UTF-16 '\n' starts on that '\n''s zero.
    CHECK(plainText(std::string(1, '\0') + utf16("next line")) == "next line");
    CHECK(plainText(std::string("caf\xC3\xA9 \x1b[1mbold\x1b[0m\r\n")) == "caf\xC3\xA9 [1mbold [0m");
    CHECK(plainText(" \t profile says hi \n\n") == "profile says hi");
    CHECK(plainText(utf16("\xE9")) == "\xC3\xA9");
    CHECK(plainText("") == "");
    const std::string longer = plainText(std::string(400, 'x'));
    CHECK(longer == std::string(300, 'x') + "...");
    // Not cut inside a character.
    std::string accents;
    for (int i = 0; i < 200; ++i) accents += "\xC3\xA9";
    const std::string cut = plainText(accents);
    CHECK(cut.size() == 300 + 3 && cut.substr(0, 300) == accents.substr(0, 300));
}

int main() {
    words_default();
    words_quoted();
    words_plain();
    frames_cut_anywhere();
    frame_refusals();
    hello_checks();
    actions_across();
    states_across();
    state_refusals();
    ends_across();
    ends_refuse();
    life_backoff();
    life_one_relay();
    life_across();
    life_refusals();
    if (failures != 0) {
        std::fprintf(stderr, "relay_unit: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("relay_unit: all passed\n");
    return 0;
}
