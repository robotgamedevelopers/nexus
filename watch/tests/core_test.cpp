// Unit tests of the watch's core (watch/core): the AI-name matcher, the voice turn, the relay's frames and both its
// ends, and the C interface the app calls, end to end in memory. No clock, no sockets, no files: time is an argument
// and the phone's link is a LinkView the test writes.
//
//   watch_core_test [group]     groups: match, turn, frames, watchlink, phone, capi, screen, accounts (all when none)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ai_match.h"
#include "nexus_watch.h"
#include "relay.h"
#include "screen.h"
#include "voice_turn.h"

using namespace nexus::watch;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

// The server's names, as accounts::isName has them.
const std::vector<std::string> kAis{"rowan", "emma", "zhi-ya-wen", "johnny-grok", "stella"};

// The watch's view of the shared matcher's answer (the matcher's own cases are tests/voice/test_name_match.cpp).
void match() {
    AiMatch m = matchAi("Emma, what's on the board?", kAis);
    CHECK(m.kind == AiMatch::Kind::Named && m.index == 1 && m.rest == "what's on the board?");

    m = matchAi("Hey Johnny Grok, tell me a joke", kAis);
    CHECK(m.kind == AiMatch::Kind::Named && m.index == 3 && m.rest == "tell me a joke");

    m = matchAi("Zhi Ya Wen, good morning", kAis);
    CHECK(m.kind == AiMatch::Kind::Named && m.index == 2 && m.rest == "good morning");

    m = matchAi("how's the sprint, Stella?", kAis);   // named at the end
    CHECK(m.kind == AiMatch::Kind::Named && m.index == 4);

    m = matchAi("  what time is it  ", kAis);
    CHECK(m.kind == AiMatch::Kind::None && m.rest == "what time is it");

    m = matchAi("Stella.", kAis);                      // a name alone
    CHECK(m.kind == AiMatch::Kind::Named && m.index == 4 && m.rest.empty());

    m = matchAi("Alexi, status", {"emma", "alexa", "alex"});  // one letter from two: ask, never guess
    CHECK(m.kind == AiMatch::Kind::Ambiguous);
    CHECK((m.candidates == std::vector<std::size_t>{1, 2}));

    m = matchAi("Emma, hello", {});                    // no AIs yet: nobody is named
    CHECK(m.kind == AiMatch::Kind::None);
}

bool hasNotice(VoiceTurn& t, const std::string& part) {
    for (const Say& s : t.says()) {
        if (s.kind == Say::Kind::Notice && s.text.find(part) != std::string::npos)
            return true;
    }
    return false;
}

void turn() {
    {
        VoiceTurn t;   // no server yet
        CHECK(t.phase() == Phase::Disconnected);
        CHECK(!t.press(0, true));
        CHECK(hasNotice(t, "No server"));
        CHECK(t.notices() == 1);
        CHECK(t.back());
        CHECK(t.phase() == Phase::Idle);
    }
    VoiceTurn t{1, true};
    t.setAis(kAis);
    // Locked (as it starts): it will not listen, and says why.
    CHECK(!t.unlocked());
    CHECK(!t.press(0, true));
    CHECK(hasNotice(t, "nexus is locked"));
    CHECK(t.phase() == Phase::Idle);
    t.unlock();
    CHECK(!t.press(0, false));
    CHECK(hasNotice(t, "microphone is off"));
    CHECK(t.phase() == Phase::Idle);

    CHECK(t.press(100, true));
    CHECK(t.phase() == Phase::Listening);
    CHECK(t.release(200));                       // 100 ms: a slip of the finger
    CHECK(t.phase() == Phase::Idle);
    CHECK(hasNotice(t, "Too short"));

    CHECK(t.press(1000, true));
    CHECK(t.release(2500));
    CHECK(t.phase() == Phase::Transcribing);
    CHECK(t.heard() == 1);
    CHECK(!t.transcript(2, "Emma, hi"));         // not this recording's
    CHECK(t.transcript(1, "Emma, how is the build?"));
    CHECK(t.phase() == Phase::Choosing);
    std::optional<Prompt> p = t.choose();
    CHECK(p.has_value());
    CHECK((*p == Prompt{1, "emma", "how is the build?"}));
    CHECK(t.phase() == Phase::Sending);
    CHECK(t.says().back() == (Say{Say::Kind::Cue, "To emma.", {}}));
    CHECK(t.current() == "emma");
    CHECK(!t.applied(0));
    CHECK(t.applied(1));
    CHECK(t.phase() == Phase::Waiting);
    CHECK(t.awaiting() == 1);
    t.reply("emma", "Green on every runner.", false);
    CHECK(t.phase() == Phase::Speaking);
    CHECK(t.awaiting() == 0);
    CHECK(t.press(9000, true));                  // a tap while it reads: stop
    CHECK(t.phase() == Phase::Interrupted);
    CHECK(t.resume());
    CHECK(t.phase() == Phase::Speaking);
    CHECK(t.speechDone());
    CHECK(t.phase() == Phase::Idle);
    CHECK(t.replies().empty());

    // No name: the first AI in the server's list, whoever was talked to last.
    CHECK(t.press(10000, true) && t.release(11000));
    CHECK(t.transcript(2, "and the tests?"));
    p = t.choose();
    CHECK(p && p->num == 2 && p->aiName == "rowan" && p->text == "and the tests?");
    CHECK(t.says().back() == (Say{Say::Kind::Cue, "To rowan.", {}}));
    // The phone restarted while it was in flight.
    CHECK(!t.lostPrompt(3));
    CHECK(t.lostPrompt(2));
    CHECK(hasNotice(t, "may not have arrived"));
    CHECK(t.phase() == Phase::Idle);

    // A name alone sends nothing, and asks for the words.
    CHECK(t.press(12000, true) && t.release(13000) && t.transcript(3, "Stella"));
    CHECK(!t.choose().has_value());
    CHECK(hasNotice(t, "What should I tell stella?"));
    CHECK(t.phase() == Phase::Idle);

    // Words that never come back.
    CHECK(t.press(14000, true) && t.release(15000));
    CHECK(!t.tick(15000 + VoiceTurn::kTranscribeMs - 1));
    CHECK(t.tick(15000 + VoiceTurn::kTranscribeMs));
    CHECK(hasNotice(t, "No words came back"));
    CHECK(t.phase() == Phase::Idle);

    // The recognizer found none.
    CHECK(t.press(40000, true) && t.release(41000));
    CHECK(t.transcriptFailed(5, "there were no words in the recording"));
    CHECK(hasNotice(t, "didn't catch that"));

    // The link goes while the person talks: said, and not sent.
    CHECK(t.press(50000, true));
    const std::uint64_t before = t.notices();
    t.lost("nexus-server has not been heard for 6 seconds");
    CHECK(t.phase() == Phase::Disconnected);
    CHECK(t.notices() == before + 1);
    CHECK(hasNotice(t, "That was not sent"));
    CHECK(t.why() == "nexus-server has not been heard for 6 seconds");
    t.lost("again");                             // once is enough
    CHECK(t.notices() == before + 1);
    CHECK(!t.release(51000));
    CHECK(t.back());
    CHECK(t.phase() == Phase::Idle);

    // A reply while the person talks waits until they are done.
    CHECK(t.press(60000, true));
    t.reply("Stella", "Hello!", false);
    CHECK(t.phase() == Phase::Listening);
    CHECK(t.release(60100));                     // too short: back to rest, and the reply is read
    CHECK(t.phase() == Phase::Speaking);
    CHECK(t.press(60200, true) && t.dismiss());
    CHECK(t.phase() == Phase::Idle && t.replies().empty());

    // The watch locks: a reply is neither read nor shown until it is unlocked; talking stops, said.
    t.reply("Stella", "The secret plan.", false);
    CHECK(t.phase() == Phase::Speaking);
    t.lock();
    CHECK(t.phase() == Phase::Idle && t.replies().size() == 1);
    CHECK(hasNotice(t, "The reply waits until you unlock it"));
    t.reply("Stella", "And more.", false);
    CHECK(t.phase() == Phase::Idle);             // locked: not read
    CHECK(!t.press(61000, true) && hasNotice(t, "nexus is locked"));
    t.unlock();
    CHECK(t.phase() == Phase::Speaking);
    CHECK(t.speechDone() && t.speechDone() && t.phase() == Phase::Idle);
    CHECK(t.press(62000, true) && t.phase() == Phase::Listening);
    t.lock();
    CHECK(t.phase() == Phase::Idle && hasNotice(t, "locked while you were talking"));
    t.unlock();

    // No AIs from the server yet: said, nothing sent. Two AIs fit: asked, never guessed.
    VoiceTurn u{1, true};
    u.unlock();
    CHECK(u.press(0, true) && u.release(1000) && u.transcript(1, "hi there"));
    CHECK(!u.choose().has_value());
    CHECK(hasNotice(u, "has not said which AIs"));
    u.setAis({"emma", "emmy"});
    CHECK(u.press(2000, true) && u.release(3000) && u.transcript(2, "Emmo, hi"));
    CHECK(!u.choose().has_value());
    CHECK(hasNotice(u, "fits emma and emmy"));
    CHECK(u.press(4000, true) && u.release(5000) && u.transcript(3, "hi there"));
    p = u.choose();
    CHECK(p && p->num == 1 && p->aiName == "emma");
}

void frames() {
    WatchFrame w;
    w.watch = "watch-1";
    w.epoch = 77;
    w.ack = 3;
    w.prompts = {{4, "Emma", "hi;: 5:x"}, {5, "Zhi Yǎ Wén", ""}};
    w.audio = Audio{2, "m4a", std::string("\0\x01:;\xff", 5)};
    WatchFrame w2;
    std::string why;
    CHECK(decode(encode(w), &w2, &why));
    CHECK(w2 == w);
    w.audio.reset();
    CHECK(decode(encode(w), &w2, &why) && w2 == w);

    PhoneFrame f;
    f.epoch = 9;
    f.applied = 4;
    f.audioTaken = 2;
    f.link = LinkPhase::Lost;
    f.why = "no route";
    f.aisKnown = true;
    f.ais = {"Emma", "Rowan"};
    f.events = {{1, Event::Kind::Transcript, 2, "", "Emma, hi"}, {2, Event::Kind::Reply, 0, "Emma", "hello"}};
    f.uncertain = 3;
    PhoneFrame f2;
    CHECK(decode(encode(f), &f2, &why));
    CHECK(f2 == f);

    // What does not read is refused, saying so.
    const std::string good = encode(f);
    CHECK(!decode(good.substr(0, good.size() - 1), &f2, &why));
    CHECK(why.find("does not read") != std::string::npos);
    CHECK(!decode(good + "x", &f2, &why));
    CHECK(!decode(std::string("nexus-watch 1;"), &f2, &why));
    CHECK(!decode(encode(w), &f2, &why));                    // a watch frame is not a phone frame
    CHECK(!decode(std::string("nexus-phone 3;"), &f2, &why));
    CHECK(why.find("protocol 3") != std::string::npos);
    CHECK(!decode(std::string("nexus-watch 1;7:watch-10;0;99999999999;"), &w2, &why));   // a count past the bytes
    CHECK(!decode(std::string("nexus-phone 1;1;0;0;0;9;0:"), &f2, &why));                // a link phase past 2
    PhoneFrame bad;
    bad.uncertain = 5;
    bad.applied = 4;
    CHECK(!decode(encode(bad), &f2, &why));                                                // vouching past what is done
    // Every prefix of a frame is refused, never read past its end.
    const std::string whole = encode(WatchFrame{1, "w", 1, 0, {{1, "a", "b"}}, Audio{1, "m4a", "xyz"}, std::nullopt});
    for (std::size_t n = 0; n < whole.size(); ++n)
        CHECK(!decode(whole.substr(0, n), &w2, &why));
}

void watchlink() {
    WatchLink l{"watch-1"};
    l.add({1, "Emma", "one"});
    l.add({2, "Emma", "two"});
    l.record(Audio{1, "m4a", "abc"});
    CHECK(l.nextPrompt() == 3);
    WatchFrame w = l.frame();
    CHECK(w.prompts.size() == 2 && w.audio.has_value() && w.epoch == 0 && w.ack == 0);

    PhoneFrame f;
    f.epoch = 5;
    f.applied = 1;
    f.audioTaken = 1;
    f.link = LinkPhase::Connected;
    f.events = {{1, Event::Kind::Transcript, 1, "", "x"}, {2, Event::Kind::Reply, 0, "Emma", "y"}};
    WatchLink::Taken t = l.take(f);
    CHECK(t.events.size() == 2);
    CHECK(l.outbox().size() == 1 && l.outbox()[0].num == 2);
    CHECK(!l.audio().has_value());
    CHECK(l.ack() == 2 && l.epoch() == 5);
    // The same answer again: nothing new.
    t = l.take(f);
    CHECK(t.events.empty());
    // A new run of the phone: its events from 1 again; prompt 2 cannot be vouched for.
    f.epoch = 6;
    f.applied = 2;
    f.uncertain = 2;
    f.events = {{1, Event::Kind::Reply, 0, "Emma", "z"}};
    t = l.take(f);
    CHECK(t.events.size() == 1 && t.events[0].text == "z");
    CHECK(t.lost.size() == 1 && t.lost[0].num == 2);
    CHECK(l.outbox().empty());

    l.add({3, "Rowan", "three"});
    WatchLink back;
    std::string why;
    CHECK(WatchLink::load(l.save(), &back, &why));
    CHECK(back.id() == "watch-1" && back.nextPrompt() == 4 && back.outbox().size() == 1);
    CHECK(!WatchLink::load("nonsense", &back, &why));
    CHECK(!WatchLink::load(l.save().substr(0, 10), &back, &why));
    // An outbox out of order is refused.
    CHECK(!WatchLink::load("nexus-watch-outbox 1:w4;0;2;3;1:a1:b2;1:a1:b", &back, &why));
    // The recordings' number outlives the app with the outbox.
    l.record(Audio{7, "m4a", "x"});
    CHECK(WatchLink::load(l.save(), &back, &why) && back.utterances() == 7);
}

LinkView connected(std::vector<LinkSession> sessions, std::uint64_t sent, bool acknowledged) {
    LinkView v;
    v.phase = LinkPhase::Connected;
    v.aisKnown = true;
    v.ais = {"Emma", "Rowan"};
    v.sessions = std::move(sessions);
    v.sent = sent;
    v.acknowledged = acknowledged;
    return v;
}

LinkSession session(std::uint64_t id, const std::string& name, LinkSession::Activity a,
                    std::vector<LinkEntry> entries = {}) {
    LinkSession s;
    s.id = id;
    s.aiName = name;
    s.activity = a;
    s.entries = std::move(entries);
    return s;
}

void phone() {
    PhoneRelay r{{}, 42};
    WatchFrame w;
    w.watch = "watch-1";
    w.prompts = {{1, "Emma", "hi"}, {2, "Rowan", "yo"}};
    w.audio = Audio{1, "m4a", "rec"};
    // Locked (as it starts): it takes nothing, hands nothing, sends nothing back, and says it is locked.
    PhoneRelay::Exchanged x = r.exchange(w);
    CHECK(!r.unlocked());
    CHECK(r.waiting().empty() && !x.transcribe && x.reply.audioTaken == 0);
    CHECK(x.reply.link == LinkPhase::Lost && x.reply.why == PhoneRelay::kLockedWhy);
    r.transcribed(9, true, "words from before the lock");
    CHECK(r.answer(w).events.empty() && !r.answer(w).aisKnown && r.answer(w).ais.empty());
    CHECK(r.link(connected({session(7, "Emma", LinkSession::Activity::Idle)}, 0, true)).empty());
    r = PhoneRelay{{}, 42};
    r.setUnlocked(true);
    w.audio.reset();
    x = r.exchange(w);
    CHECK(r.persisted().watch == "watch-1");
    CHECK(x.reply.applied == 0 && x.reply.link == LinkPhase::Connecting);
    CHECK(r.waiting().size() == 2);
    // Not connected: nothing is handed over.
    CHECK(r.link(LinkView{}).empty());
    // Emma has a session; Rowan has none yet: an ensure, once.
    using A = LinkSession::Activity;
    std::vector<LinkAction> out = r.link(connected({session(7, "Emma", A::Idle)}, 0, true));
    CHECK(out.size() == 2);
    CHECK((out[0] == LinkAction{LinkAction::Kind::Send, 7, "Emma", "hi", 1}));
    CHECK((out[1] == LinkAction{LinkAction::Kind::Ensure, 0, "Rowan", "", 0}));
    CHECK(r.persisted().handed == 1);
    CHECK(r.link(connected({session(7, "Emma", A::Idle)}, 2, false)).empty());   // the ensure is not sent twice
    // The same frame again (a carrier that delivered it twice): nothing new.
    r.exchange(w);
    CHECK(r.waiting().size() == 1);
    out = r.link(connected({session(7, "Emma", A::Busy), session(8, "Rowan", A::Idle)}, 2, true));
    CHECK(out.size() == 1 && out[0].kind == LinkAction::Kind::Send && out[0].session == 8 && out[0].prompt == 2);
    CHECK(r.persisted().handed == 2);
    CHECK(r.acked() == 1);                       // the link acknowledged its first two actions, not the third
    CHECK(r.answer(w).applied == 1);
    r.link(connected({session(7, "Emma", A::Busy), session(8, "Rowan", A::Idle)}, 3, true));
    CHECK(r.answer(w).applied == 2);

    // Emma's reply: after her prompt's own entry, once the turn is over.
    using K = LinkEntry::Kind;
    std::vector<LinkEntry> emma{{K::Person, "hi", true}, {K::Ai, "hel", false}};
    r.link(connected({session(7, "Emma", A::Busy, emma), session(8, "Rowan", A::Busy)}, 3, true));
    CHECK(r.events().empty());
    emma.back() = {K::Ai, "hello there", true};
    emma.push_back({K::Tool, "ls", true});
    emma.push_back({K::Ai, "all well", true});
    r.link(connected({session(7, "Emma", A::Idle, emma), session(8, "Rowan", A::Busy)}, 3, true));
    CHECK(r.events().size() == 1);
    CHECK((r.events().back() == Event{1, Event::Kind::Reply, 0, "Emma", "hello there\nall well"}));
    // Rowan cannot run: the person is told.
    r.link(connected({session(7, "Emma", A::Idle, emma),
                      session(8, "Rowan", A::Down, {{K::Person, "yo", true}, {K::Notice, "no login", true}})}, 3, true));
    CHECK(r.events().size() == 2);
    CHECK((r.events().back() == Event{2, Event::Kind::Notice, 0, "Rowan", "no login"}));
    // The watch has them: they go.
    w.epoch = 42;
    w.ack = 2;
    w.prompts.clear();
    x = r.exchange(w);
    CHECK(x.reply.events.empty() && r.events().empty());

    // A recording: taken once, its words back as an event.
    w.audio = Audio{1, "m4a", "rec"};
    x = r.exchange(w);
    CHECK(x.transcribe.has_value() && x.reply.audioTaken == 1);
    CHECK(!r.exchange(w).transcribe.has_value());
    r.transcribed(1, true, "Emma, hi");
    CHECK(r.answer(w).events.size() == 1 && r.answer(w).events[0].kind == Event::Kind::Transcript);

    // An AI the server does not have: told, and done with.
    w.prompts = {{3, "Nobody", "hello?"}};
    r.exchange(w);
    CHECK(r.link(connected({session(7, "Emma", A::Idle, emma)}, 3, true)).empty());
    CHECK(r.answer(w).applied == 3);
    CHECK(r.events().back().kind == Event::Kind::Notice);
    CHECK(r.events().back().text.find("no AI named Nobody") != std::string::npos);

    // The phone restarts: what it handed before it cannot vouch for; what follows is handed once.
    PhoneRelay again{r.persisted(), 43};
    again.setUnlocked(true);
    WatchFrame w3;
    w3.watch = "watch-1";
    w3.prompts = {{3, "Emma", "x"}, {4, "Emma", "y"}};
    PhoneFrame a = again.exchange(w3).reply;
    CHECK(a.uncertain == 3);
    CHECK(a.applied == 3 && a.epoch == 43);
    out = again.link(connected({session(7, "Emma", A::Idle)}, 0, true));
    CHECK(out.size() == 1 && out[0].prompt == 4);

    // The watch app restarted while the relay ran: its events go on from where the relay holds them, not from 1.
    {
        PhoneRelay running{{"watch-r", 0}, 50};
        running.setUnlocked(true);
        WatchFrame wr;
        wr.watch = "watch-r";
        WatchLink first{"watch-r"};
        running.transcribed(1, true, "one");
        running.transcribed(2, true, "two");
        CHECK(first.take(running.exchange(wr).reply).events.size() == 2);
        wr.epoch = 50;
        wr.ack = 2;
        running.exchange(wr);                    // acknowledged: the relay drops them
        running.transcribed(3, true, "three");
        WatchLink restarted{"watch-r"};          // a new app: epoch 0, nothing acknowledged
        WatchLink::Taken tr = restarted.take(running.exchange(restarted.frame()).reply);
        CHECK(tr.events.size() == 1 && tr.events[0].text == "three");
        running.transcribed(4, true, "four");
        tr = restarted.take(running.exchange(restarted.frame()).reply);
        CHECK(tr.events.size() == 1 && tr.events[0].text == "four");
    }
    again.transcribed(9, true, "words for the first watch, never taken");

    // Another watch: its numbers are its own, the events' among them, which count from 1 again for it.
    WatchFrame other;
    other.watch = "watch-2";
    other.prompts = {{1, "Emma", "new watch"}};
    a = again.exchange(other).reply;
    CHECK(a.uncertain == 0 && a.applied == 0);
    CHECK(again.persisted() == (PhoneRelay::Persisted{"watch-2", 0}));
    CHECK(a.events.empty());
    again.transcribed(1, true, "Emma, new watch");
    WatchLink fresh{"watch-2"};
    const WatchLink::Taken t2 = fresh.take(again.answer(other));
    CHECK(t2.events.size() == 1 && t2.events[0].text == "Emma, new watch");
}

// An AI's accounts on the watch (docs/watch.md, "Switching an AI's account"): the list, the switch's frames both
// ways, the phone handing it to the link, and the watch's choice of the account it is on already sending nothing.
AiAccounts emmaAccounts() {
    AiAccounts a;
    a.aiName = "Emma";
    a.current = "work";
    a.accounts = {{"work", "Work", "emma@work.example", true},
                  {"home", "Home", "", true},
                  {"spare", "", "", true},
                  {"lapsed", "Lapsed", "lapsed@example.com", false}};
    return a;
}

void accounts() {
    // The list: only signed-in accounts, in the server's order; the current one marked; email, else label, else name.
    const std::vector<AccountRow> rows = accountRows(emmaAccounts(), std::nullopt);
    CHECK(rows.size() == 3);
    CHECK((rows[0] == AccountRow{"work", "emma@work.example", true, false}));
    CHECK((rows[1] == AccountRow{"home", "Home", false, false}));
    CHECK((rows[2] == AccountRow{"spare", "spare", false, false}));
    // A switch held marks its row; one for another AI marks nothing.
    const std::vector<AccountRow> held = accountRows(emmaAccounts(), AccountSwitch{4, "Emma", "home"});
    CHECK(held.size() == 3 && held[1].switching && !held[0].switching && held[0].current);
    CHECK(!accountRows(emmaAccounts(), AccountSwitch{4, "Rowan", "home"})[1].switching);

    // The switch encoded and decoded, both ways.
    WatchFrame w;
    w.watch = "watch-1";
    w.accountSwitch = AccountSwitch{3, "Zhi Yǎ Wén", "a;: 2:b"};
    WatchFrame w2;
    std::string why;
    CHECK(decode(encode(w), &w2, &why));
    CHECK(w2 == w);
    CHECK(w2.accountSwitch && w2.accountSwitch->num == 3 && w2.accountSwitch->account == "a;: 2:b");
    PhoneFrame f;
    f.epoch = 2;
    f.switched = 3;
    f.rosters = {emmaAccounts(), AiAccounts{"Rowan", "", {}}};
    f.events = {{1, Event::Kind::AccountRefused, 0, "Emma", "lapsed is not signed in"}};
    PhoneFrame f2;
    CHECK(decode(encode(f), &f2, &why));
    CHECK(f2 == f);
    CHECK(f2.switched == 3 && f2.rosters.size() == 2 && f2.rosters[0].accounts[3].email == "lapsed@example.com" &&
          !f2.rosters[0].accounts[3].signedIn);
    // A frame of protocol 1 still reads, without them; one of 2 cut before them does not.
    WatchFrame old;
    old.proto = 1;
    old.watch = "watch-1";
    CHECK(decode(encode(old), &w2, &why) && !w2.accountSwitch);
    const std::string two = encode(w);
    CHECK(!decode(two.substr(0, two.find("1;3;")), &w2, &why));
    CHECK(!decode(std::string("nexus-watch 2;1:w0;0;0;0;1;0;1:a1:b"), &w2, &why));   // a switch numbered 0
    CHECK(!decode(std::string("nexus-watch 2;1:w0;0;0;0;2;"), &w2, &why));            // a switch flag past 1
    PhoneFrame oldPhone;
    oldPhone.proto = 1;
    oldPhone.events = {{1, Event::Kind::AccountRefused, 0, "Emma", "x"}};
    CHECK(!decode(encode(oldPhone), &f2, &why));     // an event kind protocol 1 does not have

    // The watch: nothing to choose from before the phone sends the list; then a choice of the account the AI is on
    // already sends nothing, and says nothing.
    WatchLink l{"watch-1"};
    CHECK(!l.choose("Emma", "home", &why) && !why.empty());
    PhoneFrame withList;
    withList.rosters = {emmaAccounts()};
    l.take(withList);
    CHECK(l.accounts("Emma").size() == 3 && l.accounts("Rowan").empty());
    CHECK(!l.choose("Emma", "work", &why));
    CHECK(why.empty());
    CHECK(!l.accountSwitch() && !l.frame().accountSwitch);
    CHECK(l.nextSwitch() == 1);
    // Not signed in, or not there: refused, saying why.
    CHECK(!l.choose("Emma", "lapsed", &why) && why.find("not signed in") != std::string::npos);
    CHECK(!l.choose("Emma", "nobody", &why) && !why.empty());
    // Another account: held and sent until the phone is done with it.
    CHECK(l.choose("Emma", "home", &why));
    CHECK((l.frame().accountSwitch == AccountSwitch{1, "Emma", "home"}));
    CHECK(l.accounts("Emma")[1].switching);
    CHECK(!l.choose("Emma", "home", &why) && why.empty());          // the one held already: nothing more
    CHECK(!l.choose("Rowan", "x", &why));                           // another AI's while one is under way: refused
    CHECK(l.choose("Emma", "work", &why));                          // back to the current, while home is held: sent
    CHECK((l.frame().accountSwitch == AccountSwitch{2, "Emma", "work"}));
    // Kept across a relaunch, numbers and all.
    WatchLink back;
    CHECK(WatchLink::load(l.save(), &back, &why));
    CHECK(back.accountSwitch() == l.accountSwitch() && back.nextSwitch() == 3);
    // A save from before the switch still loads.
    CHECK(WatchLink::load("nexus-watch-outbox 1:w1;0;0;", &back, &why) && back.nextSwitch() == 1);
    withList.switched = 1;
    l.take(withList);
    CHECK(l.accountSwitch().has_value());                           // done with 1, not 2
    withList.switched = 2;
    l.take(withList);
    CHECK(!l.accountSwitch());

    // The phone: the switch goes to the AI's first session as a SetAccount, and is done with when acknowledged.
    using A = LinkSession::Activity;
    PhoneRelay r{{}, 7};
    r.setUnlocked(true);
    WatchFrame in;
    in.watch = "watch-1";
    in.accountSwitch = AccountSwitch{5, "Emma", "home"};
    r.exchange(in);
    CHECK(r.switchSeen() == 5 && r.switchWaiting());
    LinkView v = connected({session(9, "Emma", A::Idle), session(4, "Emma", A::Idle)}, 0, true);
    v.rosters = {emmaAccounts()};
    std::vector<LinkAction> out = r.link(v);
    CHECK(out.size() == 1);
    CHECK((out[0] == LinkAction{LinkAction::Kind::SetAccount, 4, "Emma", "home", 5}));
    CHECK(r.switched() == 0 && r.answer(in).switched == 0);
    r.exchange(in);                                                 // the same switch again: not handed twice
    CHECK(r.link(v).empty());
    v.sent = 1;
    r.link(v);
    CHECK(r.switched() == 5 && r.answer(in).switched == 5);
    CHECK(r.answer(in).rosters == v.rosters);
    // Signed out since the watch's list: done with, and said.
    in.accountSwitch = AccountSwitch{6, "Emma", "lapsed"};
    r.exchange(in);
    CHECK(r.link(v).empty() && r.switched() == 6);
    CHECK(r.events().back().kind == Event::Kind::AccountRefused);
    // Locked: no list goes to the watch, and no switch is taken.
    r.setUnlocked(false);
    in.accountSwitch = AccountSwitch{7, "Emma", "spare"};
    r.exchange(in);
    CHECK(r.answer(in).rosters.empty() && r.switchSeen() == 6);
    // An AI with no session yet: ensured, and the switch waits for it.
    PhoneRelay q{{}, 8};
    q.setUnlocked(true);
    in.accountSwitch = AccountSwitch{1, "Emma", "home"};
    q.exchange(in);
    v = connected({}, 0, true);
    v.rosters = {emmaAccounts()};
    out = q.link(v);
    CHECK(out.size() == 1 && out[0].kind == LinkAction::Kind::Ensure && q.switchWaiting());

    // The C interface: the AI shown, its list, a choice of the current one sending nothing, another one sending.
    nw_client* c = nw_client_new("watch-3", nullptr, 0);
    char* shownAi = nw_shown_ai(c);
    CHECK(std::strcmp(shownAi, "") == 0);
    nw_free(shownAi);
    PhoneFrame up;
    up.link = LinkPhase::Connected;
    up.aisKnown = true;
    up.ais = {"Emma", "Rowan"};
    up.rosters = {emmaAccounts()};
    const std::string upBytes = encode(up);
    // Still frames: a relaunch shows the last launch's first AI from its first frame, and the phone's first answer
    // keeps it. Once the list is known it rules, as the words with no name do: a remembered AI the list no longer
    // starts with gives way, and a known empty list shows none.
    {
        nw_client* r = nw_client_new("watch-3", nullptr, 0);
        nw_remember_ai(r, "Emma");
        CHECK(nw_ais_known(r) == 0);
        char* first = nw_shown_ai(r);
        CHECK(std::strcmp(first, "Emma") == 0);
        CHECK(nw_take(r, upBytes.data(), upBytes.size()) == 1);
        CHECK(nw_ais_known(r) == 1);
        char* after = nw_shown_ai(r);
        CHECK(std::strcmp(after, first) == 0);
        nw_free(first);
        nw_free(after);
        nw_client_free(r);
        nw_client* s = nw_client_new("watch-3", nullptr, 0);
        nw_remember_ai(s, "Stella");
        CHECK(nw_take(s, upBytes.data(), upBytes.size()) == 1);
        after = nw_shown_ai(s);
        CHECK(std::strcmp(after, "Emma") == 0);
        nw_free(after);
        PhoneFrame none = up;
        none.ais = {};
        none.rosters = {};
        const std::string noneBytes = encode(none);
        CHECK(nw_take(s, noneBytes.data(), noneBytes.size()) == 1);
        after = nw_shown_ai(s);
        CHECK(std::strcmp(after, "") == 0);
        nw_free(after);
        nw_client_free(s);
    }
    CHECK(nw_take(c, upBytes.data(), upBytes.size()) == 1);
    shownAi = nw_shown_ai(c);
    CHECK(std::strcmp(shownAi, "Emma") == 0);
    nw_free(shownAi);
    CHECK(nw_account_count(c, "Emma") == 3);
    char* account = nullptr;
    char* shown = nullptr;
    int current = 0, switching = 0;
    CHECK(nw_account(c, "Emma", 0, &account, &shown, &current, &switching) == 1);
    CHECK(std::strcmp(account, "work") == 0 && std::strcmp(shown, "emma@work.example") == 0 && current == 1);
    nw_free(account);
    nw_free(shown);
    CHECK(nw_account(c, "Emma", 3, &account, &shown, &current, &switching) == 0);
    int kind = 0;
    char* text = nullptr;
    while (nw_next_say(c, &kind, &text) == 1)
        nw_free(text);
    CHECK(nw_choose_account(c, "Emma", "work") == 0);
    CHECK(nw_next_say(c, &kind, &text) == 0);                       // nothing sent, nothing said
    char* bytes = nullptr;
    std::size_t n = nw_frame(c, &bytes);
    WatchFrame sent;
    CHECK(decode(std::string(bytes, n), &sent, &why) && !sent.accountSwitch);
    nw_free(bytes);
    CHECK(nw_choose_account(c, "Emma", "home") == 1);
    CHECK(nw_next_say(c, &kind, &text) == 1 && kind == NW_CUE && std::string(text) == "Switching Emma to Home.");
    nw_free(text);
    n = nw_frame(c, &bytes);
    CHECK(decode(std::string(bytes, n), &sent, &why) && (sent.accountSwitch == AccountSwitch{1, "Emma", "home"}));
    nw_free(bytes);
    CHECK(nw_choose_account(c, "Emma", "lapsed") == 0);
    CHECK(nw_next_say(c, &kind, &text) == 1 && kind == NW_NOTICE);
    nw_free(text);
    nw_client_free(c);
}

// The C interface end to end in memory: the watch's client, a phone relay, and a link the test plays.
void capi() {
    nw_client* c = nw_client_new("watch-9", nullptr, 0);
    nw_unlock(c);
    PhoneRelay phone{{}, 1};
    phone.setUnlocked(true);
    using A = LinkSession::Activity;
    using K = LinkEntry::Kind;
    LinkView view = connected({session(3, "Emma", A::Idle)}, 0, true);
    std::uint64_t handed = 0;
    std::vector<LinkAction> server;
    const auto round = [&] {
        char* bytes = nullptr;
        const std::size_t n = nw_frame(c, &bytes);
        WatchFrame in;
        std::string why;
        CHECK(decode(std::string(bytes, n), &in, &why));
        nw_free(bytes);
        PhoneRelay::Exchanged x = phone.exchange(in);
        if (x.transcribe)
            phone.transcribed(x.transcribe->utterance, true, "Emma, is main green?");
        for (LinkAction& a : phone.link(view)) {
            ++handed;
            server.push_back(a);
        }
        view.sent = handed;
        phone.link(view);
        const std::string out = encode(phone.answer(in));
        CHECK(nw_take(c, out.data(), out.size()) == 1);
    };
    CHECK(nw_phase(c) == NW_DISCONNECTED);
    round();
    CHECK(nw_phase(c) == NW_IDLE);
    CHECK(nw_ai_count(c) == 2);
    int kind = 0;
    char* text = nullptr;
    CHECK(nw_next_say(c, &kind, &text) == 1 && kind == NW_CUE);
    nw_free(text);
    CHECK(nw_press(c, 1000, 1) == 1 && nw_phase(c) == NW_LISTENING);
    CHECK(nw_release(c, 3000, "audio", 5, "m4a") == 1 && nw_phase(c) == NW_TRANSCRIBING);
    round();   // the recording goes, its words come back in the same answer, and the prompt is made
    CHECK(nw_phase(c) == NW_SENDING);
    CHECK(nw_outbox(c) == 1);
    round();   // the prompt goes to the link, which acknowledges it
    CHECK(server.size() == 1 && server[0].text == "is main green?" && server[0].session == 3);
    round();
    CHECK(nw_phase(c) == NW_WAITING && nw_outbox(c) == 0);
    view.sessions[0].entries = {{K::Person, "is main green?", true}, {K::Ai, "Yes, all green.", true}};
    round();
    CHECK(nw_phase(c) == NW_SPEAKING);
    char* who = nullptr;
    CHECK(nw_reply(c, &kind, &text, &who) == 1);
    CHECK(kind == NW_REPLY && std::strcmp(text, "Yes, all green.") == 0 && std::strcmp(who, "Emma") == 0);
    nw_free(text);
    nw_free(who);
    CHECK(nw_speech_done(c) == 1 && nw_phase(c) == NW_IDLE);
    char* current = nw_current(c);
    CHECK(std::strcmp(current, "Emma") == 0);
    nw_free(current);

    // The phone unreachable: said, shown, and the outbox kept across a relaunch.
    CHECK(nw_press(c, 9000, 1) == 1 && nw_release(c, 10000, "a", 1, "m4a") == 1);
    nw_carrier_failed(c, "your iPhone is not reachable");
    CHECK(nw_phase(c) == NW_DISCONNECTED);
    char* why = nw_why(c);
    CHECK(std::strcmp(why, "your iPhone is not reachable") == 0);
    nw_free(why);
    CHECK(nw_take(c, "garbage", 7) == 0);
    char* saved = nullptr;
    const std::size_t n = nw_saved(c, &saved);
    char* before = nullptr;
    const std::size_t bn = nw_frame(c, &before);
    WatchFrame last;
    CHECK(decode(std::string(before, bn), &last, nullptr) && last.audio && last.audio->utterance >= 1);
    nw_free(before);
    nw_client* d = nw_client_new("watch-9", saved, n);
    // Relaunched, its next recording is numbered past the last: the phone took that number already, and would drop a
    // recording that reused it as heard.
    PhoneFrame up;
    up.epoch = 1;
    up.link = LinkPhase::Connected;
    up.aisKnown = true;
    up.ais = {"Emma"};
    const std::string upBytes = encode(up);
    CHECK(nw_take(d, upBytes.data(), upBytes.size()) == 1 && nw_phase(d) == NW_IDLE);
    nw_unlock(d);
    CHECK(nw_press(d, 20000, 1) == 1 && nw_release(d, 21000, "b", 1, "m4a") == 1);
    char* after = nullptr;
    const std::size_t an = nw_frame(d, &after);
    WatchFrame next;
    CHECK(decode(std::string(after, an), &next, nullptr) && next.audio &&
          next.audio->utterance == last.audio->utterance + 1);
    nw_free(after);
    nw_client* e = nw_client_new("watch-9", "bad", 3);
    nw_free(saved);
    CHECK(nw_next_say(e, &kind, &text) == 1 && kind == NW_NOTICE);
    nw_free(text);
    nw_client_free(c);
    nw_client_free(d);
    nw_client_free(e);
}

// The screen (screen.h) and the still-frames rule: a fresh launch whose phone reaches nexus-server settles at once, so
// its first frame is already its last; a link that stays down, or is lost after, shows at once.
std::pair<std::string, std::string> shown(const nw_client* c, const char* notice = "", const char* lockWhy = "",
                                          const char* replyFrom = "") {
    char* status = nullptr;
    char* problem = nullptr;
    nw_screen(c, notice, lockWhy, replyFrom, &status, &problem);
    std::pair<std::string, std::string> s{status, problem};
    nw_free(status);
    nw_free(problem);
    return s;
}

void takeLink(nw_client* c, LinkPhase link, const std::string& why = {}) {
    PhoneFrame up;
    up.epoch = 1;
    up.link = link;
    up.why = why;
    up.aisKnown = true;
    up.ais = {"Emma"};
    const std::string bytes = encode(up);
    CHECK(nw_take(c, bytes.data(), bytes.size()) == 1);
}

void screen() {
    const std::pair<std::string, std::string> idle{"Hold and talk to your AIs", ""};

    // A healthy fresh launch: the phone's session still starting, then its link connecting, then connected. Every
    // frame on the way is the first one.
    {
        nw_client* c = nw_client_new("watch-s", nullptr, 0);
        const auto first = shown(c);
        CHECK(first == idle);
        nw_carrier_failed(c, "the link to your iPhone is still starting");
        CHECK(nw_phase(c) == NW_DISCONNECTED);
        CHECK(shown(c) == first);
        nw_tick(c, 500);
        takeLink(c, LinkPhase::Connecting);
        CHECK(nw_phase(c) == NW_DISCONNECTED);
        CHECK(shown(c) == first);
        nw_tick(c, 1500);
        takeLink(c, LinkPhase::Connected);
        CHECK(nw_phase(c) == NW_IDLE);
        CHECK(shown(c) == first);
        nw_tick(c, kSettleMs + 1000);
        CHECK(shown(c) == first);
        // Reached once, a loss after shows at once, inside the settle's time or not.
        takeLink(c, LinkPhase::Lost, "nexus-server stopped");
        CHECK(shown(c) == (std::pair<std::string, std::string>{"No server", "No server: nexus-server stopped"}));
        nw_client_free(c);
    }
    // Reached early, then lost before kSettleMs: shown at once too.
    {
        nw_client* c = nw_client_new("watch-s", nullptr, 0);
        nw_tick(c, 300);
        takeLink(c, LinkPhase::Connected);
        nw_tick(c, 600);
        nw_carrier_failed(c, "your iPhone is not reachable");
        CHECK(shown(c) == (std::pair<std::string, std::string>{"No server", "No server: your iPhone is not reachable"}));
        nw_client_free(c);
    }
    // Never reached: the idle screen until kSettleMs, then "No server" and why, never hidden past it.
    {
        nw_client* c = nw_client_new("watch-s", nullptr, 0);
        nw_carrier_failed(c, "your iPhone is not reachable");
        nw_tick(c, kSettleMs - 1);
        CHECK(shown(c) == idle);
        nw_tick(c, kSettleMs);
        CHECK(shown(c) == (std::pair<std::string, std::string>{"No server", "No server: your iPhone is not reachable"}));
        nw_client_free(c);
    }
    // A notice shows from the first frame (a press refused while the launch settles), with the owner's check's why.
    {
        nw_client* c = nw_client_new("watch-s", nullptr, 0);
        CHECK(shown(c, "nexus is locked", "no passcode") ==
              (std::pair<std::string, std::string>{"Hold and talk to your AIs", "nexus is locked (no passcode)"}));
        nw_unlock(c);
        CHECK(shown(c, "nexus is locked", "no passcode").second == "nexus is locked");
        nw_client_free(c);
    }
    // The pure mapping, phase by phase.
    ScreenIn in;
    in.settled = true;
    in.unlocked = true;
    in.current = "Emma";
    in.replyFrom = "Rowan";
    const std::vector<std::pair<Phase, std::string>> lines{
        {Phase::Idle, "Hold and talk to your AIs"}, {Phase::Listening, "Listening…"},
        {Phase::Transcribing, "Hearing you…"}, {Phase::Choosing, "Hearing you…"},
        {Phase::Sending, "To Emma…"}, {Phase::Waiting, "Emma is thinking…"}, {Phase::Speaking, "Rowan"},
        {Phase::Interrupted, "Stopped"}, {Phase::Disconnected, "No server"}};
    for (const auto& [phase, line] : lines) {
        in.phase = phase;
        CHECK(screenOf(in).status == line);
    }
    in.phase = Phase::Disconnected;
    in.why = "";
    in.notice = "No server yet";
    CHECK(screenOf(in).problem == "No server yet");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, std::function<void()>>> groups{
        {"match", match}, {"turn", turn}, {"frames", frames}, {"watchlink", watchlink}, {"phone", phone},
        {"capi", capi}, {"screen", screen}, {"accounts", accounts}};
    bool ran = false;
    for (const auto& [name, run] : groups) {
        if (argc < 2 || name == argv[1]) {
            run();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "watch_core_test: no group %s\n", argv[1]);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "watch_core_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("watch_core_test: all checks passed\n");
    return 0;
}
