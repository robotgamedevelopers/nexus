// Unit tests of the states mosh's transport carries between the window and nexus-server (src/sync): their diffs on
// random pairs from fixed seeds, the properties mosh's Transport relies on, the server's rules and the window's mirror.
// Logic only: no time, no sockets, no network, no files.
//
//   sync_test            runs every case
//   sync_test <case>     runs one

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "fake_mosh.h"
#include "model.h"
#include "sync/actions.h"
#include "sync/client.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#include "sync/sessions.h"
#include "sync/wire.h"

namespace {

using namespace nexus;
using namespace nexus::sync;
using nexus::test::FakeReceiver;
using nexus::test::FakeSender;
using nexus::test::Packet;
using nexus::test::Rng;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

const std::vector<std::string> kTabNames = {"ada", "bo", "cy"};

// The sessions these tests use: ada's is 1, bo's 2, cy's 3 (as ensures in that order make them); any other name 99.
SessionId sid(const std::string& name) {
    for (std::size_t i = 0; i < kTabNames.size(); ++i) {
        if (kTabNames[i] == name) {
            return i + 1;
        }
    }
    return 99;
}

SessionInfo infoOf(const std::string& name) {
    return SessionInfo{name, kDefaultHarness, {}, {}, {}};
}

Action act(ActionKind kind, SessionId session, std::string text, std::string friendName = {}) {
    Action a;
    a.kind = kind;
    a.session = session;
    a.friendName = std::move(friendName);
    a.text = std::move(text);
    return a;
}

Action numbered(std::uint64_t num, Action a) {
    a.num = num;
    return a;
}

Action ensure(std::uint64_t num, const std::string& friendName) {
    return numbered(num, act(ActionKind::Ensure, 0, "", friendName));
}

Action create(std::uint64_t num, const std::string& friendName, SessionId from) {
    return numbered(num, act(ActionKind::Create, from, "", friendName));
}

// The sessions named, made by the server's own ensures (no client counted) before a test's actions.
void withSessions(Hub& hub, const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (!hub.apply(ensure(0, name))) {
            std::fprintf(stderr, "withSessions: no session for %s\n", name.c_str());
            ++failures;
        }
    }
}

std::string randomText(Rng& rng) {
    static const std::vector<std::string> words = {"hi", "two words", "", "a\nb", "\xc3\xa9t\xc3\xa9", "\"q\" \\"};
    std::string s = words[rng.below(words.size())];
    if (rng.chance(4))
        s += std::string(rng.below(300), 'x');
    return s;
}

// ---- wire ----

void wire_roundtrip() {
    Writer w;
    const std::vector<std::uint64_t> nums = {0, 1, 127, 128, 300, 1ull << 35, ~0ull};
    for (std::uint64_t n : nums)
        w.u64(n);
    w.str("hello");
    w.byte(7);
    const std::string bytes = w.take();
    Reader r(bytes);
    for (std::uint64_t n : nums)
        CHECK(r.u64() == n);
    CHECK(r.str() == "hello");
    CHECK(r.byte() == 7);
    CHECK(r.done());
}

void wire_refuses_truncation() {
    Writer w;
    w.str("hello");
    const std::string bytes = w.take();
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        const std::string cut = bytes.substr(0, n);
        Reader r(cut);
        bool threw = false;
        try {
            r.str();
        } catch (const BadDiff&) {
            threw = true;
        }
        CHECK(threw);
    }
    // eleven bytes of continuation: past 64 bits
    const std::string huge(11, '\xff');
    Reader r(huge);
    bool threw = false;
    try {
        r.u64();
    } catch (const BadDiff&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- up: Actions ----

Actions randomActions(Rng& rng, std::size_t n) {
    Actions a;
    for (std::size_t i = 0; i < n; ++i)
        a.add(act(rng.chance(3) ? ActionKind::Queue : ActionKind::Send, rng.below(3) + 1, randomText(rng)));
    return a;
}

void actions_numbered_from_one() {
    Actions a;
    CHECK(a.next() == 1);
    CHECK(a.add(act(ActionKind::Send, sid("ada"), "hi")) == 1);
    CHECK(a.add(act(ActionKind::Queue, sid("bo"), "later")) == 2);
    CHECK(a.next() == 3);
    CHECK(a.held().size() == 2);
    CHECK(a.held()[1].num == 2 && a.held()[1].kind == ActionKind::Queue && a.held()[1].session == 2);
}

// For random a, and b = a extended, apply(diff_from(b, a)) on a gives b; with both after the same subtraction too.
void actions_diff_apply_random_pairs() {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng rng{seed};
        // b grows from p through a: p a prefix of a, a a prefix of b
        Actions b;
        Actions a;
        Actions p;
        const std::size_t n = rng.below(8);
        const std::size_t k = rng.below(n + 1);
        const std::size_t j = rng.below(k + 1);
        for (std::size_t i = 0; i < n; ++i) {
            if (i == j)
                p = b;
            if (i == k)
                a = b;
            b.add(act(rng.chance(3) ? ActionKind::Queue : ActionKind::Send, rng.below(3) + 1, randomText(rng)));
        }
        if (j == n)
            p = b;
        if (k == n)
            a = b;
        if (rng.chance(2)) {
            // both rationalized against the same acknowledged prefix, as mosh's sender does
            b.subtract(&p);
            a.subtract(&p);
        }
        const std::string d = b.diff_from(a);
        CHECK(d.empty() == (a == b));
        Actions c = a;
        c.apply_string(d);
        CHECK(c == b);
        CHECK(!c.compare(b));
        if (b.first() == 1) {
            Actions e;
            e.apply_string(b.init_diff());
            CHECK(e == b);
        }
    }
}

void actions_apply_is_idempotent_and_ignores_old() {
    Rng rng{7};
    Actions b = randomActions(rng, 5);
    Actions a;
    const std::string d = b.diff_from(a);
    a.apply_string(d);
    CHECK(a == b);
    a.apply_string(d);
    CHECK(a == b);
    Actions older;
    older.add(act(ActionKind::Send, sid("ada"), "x"));
    const std::string first = Actions().diff_from(Actions());
    CHECK(first.empty());
    // a diff of actions 1..2 on a state that holds 1..5: nothing changes
    Actions two;
    two.add(act(ActionKind::Send, sid("ada"), "a"));
    two.add(act(ActionKind::Send, sid("ada"), "b"));
    Actions copy = a;
    copy.apply_string(two.init_diff());
    CHECK(copy == a);
}

void actions_gap_refused() {
    Actions b;
    for (int i = 0; i < 4; ++i)
        b.add(act(ActionKind::Send, sid("ada"), std::to_string(i)));
    Actions two;
    two.add(act(ActionKind::Send, sid("ada"), "0"));
    two.add(act(ActionKind::Send, sid("ada"), "1"));
    const std::string tail = b.diff_from(two);   // actions 3 and 4
    Actions one;
    one.add(act(ActionKind::Send, sid("ada"), "0"));
    const Actions before = one;
    bool threw = false;
    try {
        one.apply_string(tail);
    } catch (const BadDiff& e) {
        threw = std::string(e.what()).find("missing") != std::string::npos;
    }
    CHECK(threw);
    CHECK(one == before);
}

void actions_subtract() {
    Actions a;
    for (int i = 0; i < 5; ++i)
        a.add(act(ActionKind::Send, sid("ada"), std::to_string(i)));
    Actions p;
    p.add(act(ActionKind::Send, sid("ada"), "0"));
    p.add(act(ActionKind::Send, sid("ada"), "1"));
    a.subtract(&p);
    CHECK(a.first() == 3 && a.next() == 6 && a.held().size() == 3 && a.held()[0].num == 3);
    a.subtract(&p);   // again: nothing more
    CHECK(a.first() == 3 && a.held().size() == 3);
    a.subtract(&a);   // itself: empty, numbers kept
    CHECK(a.first() == 6 && a.next() == 6 && a.held().empty());
    CHECK(a.add(act(ActionKind::Queue, sid("bo"), "x")) == 6);
}

void actions_diff_from_misuse_throws() {
    Actions a;
    for (int i = 0; i < 3; ++i)
        a.add(act(ActionKind::Send, sid("ada"), "x"));
    Actions b = a;
    b.subtract(&b);
    bool threw = false;
    try {
        (void)b.diff_from(Actions());   // b no longer holds 1..3
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)Actions().diff_from(a);   // a is ahead
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

void actions_malformed_refused_unchanged() {
    Rng rng{11};
    Actions b = randomActions(rng, 4);
    const std::string d = b.init_diff();
    for (std::size_t n = 0; n < d.size(); ++n) {
        Actions a;
        a.add(act(ActionKind::Send, sid("ada"), "keep"));
        Actions before = a;
        try {
            a.apply_string(d.substr(0, n));
        } catch (const BadDiff&) {
        }
        // a cut diff either fails whole or (cut at 0 bytes) is empty: never half applied
        CHECK(a == before || n == 0);
    }
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng r{seed};
        std::string junk(r.below(40), '\0');
        for (char& c : junk)
            c = static_cast<char>(r.below(256));
        Actions a;
        const Actions before = a;
        try {
            a.apply_string(junk);
        } catch (const BadDiff&) {
            CHECK(a == before);
        }
    }
    // an unknown kind
    Writer w;
    w.u64(1);
    w.u64(1);
    w.byte(9);
    w.u64(1);
    w.str("");
    w.str("x");
    Actions a;
    bool threw = false;
    try {
        a.apply_string(w.take());
    } catch (const BadDiff&) {
        threw = true;
    }
    CHECK(threw && a.next() == 1);
}

void applier_exactly_once() {
    Applier ap;
    auto range = [](std::uint64_t from, std::uint64_t to) {
        std::vector<Action> v;
        for (std::uint64_t n = from; n <= to; ++n)
            v.push_back(Action{n, ActionKind::Send, sid("ada"), "", std::to_string(n), {}});
        return v;
    };
    CHECK(ap.apply(range(1, 2)).size() == 2);
    CHECK(ap.apply(range(1, 2)).empty());           // again
    const auto fresh = ap.apply(range(2, 4));       // overlapping
    CHECK(fresh.size() == 2 && fresh[0].num == 3 && fresh[1].num == 4);
    std::string why;
    CHECK(ap.apply(range(6, 7), &why).empty());     // a gap: refused whole
    CHECK(ap.applied() == 4);
    CHECK(why.find("missing") != std::string::npos);
    CHECK(ap.apply(range(5, 7)).size() == 3);
    CHECK(ap.applied() == 7);
    CHECK(ap.apply({}).empty());
}

// The up direction through mosh's bookkeeping (fake_mosh.h) under loss, duplication and reordering: the server
// applies the client's actions exactly once, in order, never sees a gap, and every action arrives.
void actions_through_mosh_bookkeeping() {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        Rng rng{seed};
        FakeSender<Actions> client;
        FakeReceiver<Actions> server;
        Applier applier;
        std::vector<Action> made;
        std::vector<Action> applied;
        std::vector<Packet> flight;
        std::vector<std::uint64_t> acks;
        std::size_t refusals = 0;
        auto serverTakes = [&] {
            std::string why;
            for (const Action& a : applier.apply(Actions::parse(server.remoteDiff()), &why))
                applied.push_back(a);
            refusals += why.empty() ? 0 : 1;
        };
        for (int step = 0; step < 400; ++step) {
            switch (rng.below(6)) {
                case 0: {
                    const std::uint64_t n = client.current.add(act(ActionKind::Send, rng.below(3) + 1,
                        randomText(rng)));
                    made.push_back(client.current.held().back());
                    CHECK(made.back().num == n);
                    break;
                }
                case 1: flight.push_back(client.send(rng)); break;
                case 2:
                case 3:
                    if (!flight.empty()) {
                        const std::size_t i = rng.below(flight.size());
                        acks.push_back(server.receive(flight[i]));
                        if (!rng.chance(3))   // else it stays in flight: delivered again later
                            flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(i));
                        serverTakes();
                    }
                    break;
                case 4:
                    if (!flight.empty())
                        flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(rng.below(flight.size())));
                    break;
                default:
                    if (!acks.empty())
                        client.acknowledge(acks[rng.below(acks.size())]);   // any ack, late or out of order
                    break;
            }
        }
        // then a good network: send, deliver, acknowledge, until the server has every action
        for (int round = 0; round < 50 && applied.size() < made.size(); ++round) {
            client.acknowledge(server.receive(client.send(rng, true)));
            serverTakes();
        }
        CHECK(refusals == 0);
        CHECK(applied.size() == made.size());
        for (std::size_t i = 0; i < applied.size() && i < made.size(); ++i)
            CHECK(applied[i] == made[i]);
    }
}

// ---- down: Sessions ----

EntryRef entry(EntryKind kind, std::string text, bool closed = true) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, std::move(text), closed});
}

// The tail of the session sid(name), with name's friend.
SessionView tailFor(const std::string& name, const std::vector<EntryRef>& t, std::vector<std::string> queue, Tail tail,
                    EntryRef open = nullptr) {
    return tailOf(sid(name), infoOf(name), t, std::move(queue), tail, std::move(open));
}

// A random server history: each step appends (an entry of any kind, a tool line included), grows the open entry,
// closes it, queues, changes the session's activity or makes a session.
struct History {
    std::map<std::string, std::vector<EntryRef>> transcripts;
    std::map<std::string, std::vector<std::string>> queues;
    std::map<std::string, Activity> activities;
    Tail tail{3, 400};
    Sessions state() const {
        std::vector<SessionRef> tabs;
        for (const auto& [name, t] : transcripts) {
            SessionView v = tailFor(name, t, queues.at(name), tail);
            v.activity = activities.at(name);
            tabs.push_back(std::make_shared<const SessionView>(std::move(v)));
        }
        return Sessions(std::move(tabs));
    }
    void step(Rng& rng) {
        const std::string name = kTabNames[rng.below(3)];
        auto& t = transcripts[name];
        queues[name];
        activities[name];
        const std::size_t what = rng.below(6);
        if (what == 0 && !t.empty() && !t.back()->closed) {
            t.back() = entry(t.back()->kind, t.back()->text + randomText(rng), false);
        } else if (what == 1 && !t.empty() && !t.back()->closed) {
            t.back() = entry(t.back()->kind, t.back()->text, true);
        } else if (what == 2) {
            queues[name].push_back(randomText(rng));
        } else if (what == 3) {
            activities[name] = static_cast<Activity>(rng.below(3));
        } else {
            if (!t.empty() && !t.back()->closed) {
                t.back() = entry(t.back()->kind, t.back()->text, true);
            }
            t.push_back(entry(static_cast<EntryKind>(rng.below(4)), randomText(rng), rng.chance(3) == false));
        }
    }
};

void sessions_diff_apply_random_pairs() {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        Rng rng{seed};
        History h;
        std::vector<Sessions> states{Sessions()};
        const std::size_t steps = rng.below(40);
        for (std::size_t i = 0; i < steps; ++i) {
            h.step(rng);
            states.push_back(h.state());
        }
        // every pair, in both orders: mosh only diffs newer from older, but the diff holds for any two
        for (int k = 0; k < 20; ++k) {
            const Sessions& a = states[rng.below(states.size())];
            const Sessions& b = states[rng.below(states.size())];
            Sessions c = a;
            const std::string d = b.diff_from(a);
            c.apply_string(d);
            CHECK(c == b);
            CHECK(d.empty() == (a == b));
            CHECK(!b.compare(c));
            Sessions e;
            e.apply_string(b.init_diff());
            CHECK(e == b);
        }
    }
}

void sessions_open_entry_sends_only_growth() {
    std::vector<EntryRef> t = {entry(EntryKind::Person, "hi"), entry(EntryKind::Friend, std::string(2000, 'a'), false)};
    const Sessions a({std::make_shared<const SessionView>(tailFor("ada", t, {}, Tail{}))});
    t.back() = entry(EntryKind::Friend, std::string(2000, 'a') + "bcd", false);
    const Sessions b({std::make_shared<const SessionView>(tailFor("ada", t, {}, Tail{}))});
    const std::string d = b.diff_from(a);
    CHECK(d.size() < 40);
    Sessions c = a;
    c.apply_string(d);
    CHECK(c == b);
    // and a diff that claims growth from an entry this state does not hold open is refused
    Sessions closed({std::make_shared<const SessionView>(tailFor("ada",
        {entry(EntryKind::Person, "hi"), entry(EntryKind::Friend, std::string(2000, 'a'), true)}, {}, Tail{}))});
    const Sessions before = closed;
    bool threw = false;
    try {
        closed.apply_string(d);
    } catch (const BadDiff&) {
        threw = true;
    }
    CHECK(threw && closed == before);
}

void sessions_diff_bounded_by_tail() {
    std::vector<EntryRef> t;
    for (int i = 0; i < 10000; ++i)
        t.push_back(entry(i % 2 ? EntryKind::Friend : EntryKind::Person, "entry " + std::to_string(i) + " " +
            std::string(100, 'x')));
    const Tail tail;   // 16 entries, 16 KiB
    const Sessions b({std::make_shared<const SessionView>(tailFor("ada", t, {}, tail))});
    CHECK(b.find(sid("ada"))->first == 10000 - 16);
    CHECK(b.find(sid("ada"))->total() == 10000);
    const std::string fromEmpty = b.init_diff();
    CHECK(fromEmpty.size() < 16 * 120 + 100);
    // far behind: the tail, not the history
    std::vector<EntryRef> old(t.begin(), t.begin() + 10);
    const Sessions a({std::make_shared<const SessionView>(tailFor("ada", old, {}, tail))});
    CHECK(b.diff_from(a).size() < 16 * 120 + 100);
    // the byte budget: entries of 6000 bytes, at most 16 KiB, so two; the newest whole even when larger
    std::vector<EntryRef> big = {entry(EntryKind::Friend, std::string(6000, 'a')),
        entry(EntryKind::Friend, std::string(6000, 'b')), entry(EntryKind::Friend, std::string(6000, 'c'))};
    CHECK(tailFor("ada", big, {}, tail).entries.size() == 2);
    std::vector<EntryRef> huge = {entry(EntryKind::Friend, "x"), entry(EntryKind::Friend, std::string(40000, 'h'))};
    CHECK(tailFor("ada", huge, {}, tail).entries.size() == 1);
    CHECK(tailFor("ada", {}, {}, tail).entries.empty());
    CHECK(tailFor("ada", {}, {}, tail).first == 0);
}

void sessions_copy_is_cheap_and_shared() {
    std::vector<EntryRef> t = {entry(EntryKind::Person, "hi")};
    const Sessions a({std::make_shared<const SessionView>(tailFor("ada", t, {"q"}, Tail{}))});
    const Sessions b = a;
    CHECK(&a.sessions() == &b.sessions());   // one shared list
    CHECK(a == b);
    CHECK(b.diff_from(a).empty());
}

void sessions_removed_tab() {
    const Sessions a({std::make_shared<const SessionView>(tailFor("ada", {entry(EntryKind::Person, "x")}, {}, Tail{})),
        std::make_shared<const SessionView>(tailFor("bo", {}, {}, Tail{}))});
    const Sessions b({std::make_shared<const SessionView>(tailFor("bo", {}, {}, Tail{}))});
    Sessions c = a;
    c.apply_string(b.diff_from(a));
    CHECK(c == b);
    CHECK(c.find(sid("ada")) == nullptr);
}

void sessions_malformed_refused_unchanged() {
    Rng rng{3};
    History h;
    for (int i = 0; i < 20; ++i)
        h.step(rng);
    const Sessions b = h.state();
    const std::string d = b.init_diff();
    for (std::size_t n = 1; n < d.size(); ++n) {
        Sessions a;
        bool threw = false;
        try {
            a.apply_string(d.substr(0, n));
        } catch (const BadDiff&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(a == Sessions());
    }
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng r{seed};
        std::string junk(1 + r.below(60), '\0');
        for (char& c : junk)
            c = static_cast<char>(r.below(256));
        Sessions a = b;
        try {
            a.apply_string(junk);
        } catch (const BadDiff&) {
            CHECK(a == b);
        }
    }
    // a diff whose entry numbers run past 2^64 - 1: refused, not read as no entries
    {
        Writer w;
        w.u64(1);
        w.u64(1);
        w.byte(1);
        for (const char* s : {"ada", "claude", "", "", ""}) {
            w.str(s);
        }
        w.u64(~std::uint64_t(0));   // first
        w.u64(~std::uint64_t(0));   // k
        w.u64(1);                   // one entry, which k + 1 would wrap past
        w.byte(1);                  // then a queue of none, where the entry should be
        w.u64(0);
        Sessions a = b;
        bool threw = false;
        try {
            a.apply_string(w.take());
        } catch (const BadDiff&) {
            threw = true;
        }
        CHECK(threw && a == b);
    }
    // a diff applied to a state other than the one it is from: refused, unchanged, when it keeps what that state lacks
    std::size_t refused = 0;
    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
        Rng r{seed};
        History hist;
        for (int i = 0; i < 25; ++i)
            hist.step(r);
        const Sessions mid = hist.state();
        for (int i = 0; i < 5; ++i)
            hist.step(r);
        const Sessions later = hist.state();
        Sessions other;
        try {
            other.apply_string(later.diff_from(mid));
            CHECK(other == later);   // the diff happened to keep nothing
        } catch (const BadDiff&) {
            CHECK(other == Sessions());
            ++refused;
        }
    }
    CHECK(refused > 0);
}

// The down direction through mosh's bookkeeping: the window's state ends equal to the server's, and every state it
// held on the way was one the server had.
void sessions_through_mosh_bookkeeping() {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        Rng rng{seed};
        History h;
        FakeSender<Sessions> server;
        FakeReceiver<Sessions> client;
        std::vector<Sessions> had{Sessions()};
        std::vector<Packet> flight;
        std::vector<std::uint64_t> acks;
        for (int step = 0; step < 300; ++step) {
            switch (rng.below(6)) {
                case 0:
                    h.step(rng);
                    server.current = h.state();
                    had.push_back(server.current);
                    break;
                case 1: flight.push_back(server.send(rng)); break;
                case 2:
                case 3:
                    if (!flight.empty()) {
                        const std::size_t i = rng.below(flight.size());
                        acks.push_back(client.receive(flight[i]));
                        if (!rng.chance(3))
                            flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(i));
                        bool known = false;
                        for (const Sessions& t : had) {
                            known = known || t == client.latest();
                        }
                        CHECK(known);
                    }
                    break;
                case 4:
                    if (!flight.empty())
                        flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(rng.below(flight.size())));
                    break;
                default:
                    if (!acks.empty())
                        server.acknowledge(acks[rng.below(acks.size())]);
                    break;
            }
        }
        for (int round = 0; round < 10; ++round)
            server.acknowledge(client.receive(server.send(rng, true)));
        CHECK(client.latest() == server.current);
    }
}

// ---- the server's rules ----

void hub_send_and_queue() {
    MemoryStore store;
    Hub hub(store);
    withSessions(hub, {"ada"});
    CHECK(hub.apply(Action{1, ActionKind::Send, sid("ada"), "", "hello", {}}));
    CHECK(hub.apply(Action{2, ActionKind::Queue, sid("ada"), "", "later", {}}));
    const SessionView* t = hub.state().find(sid("ada"));
    CHECK(t != nullptr && t->entries.size() == 2 && t->first == 0);
    CHECK(t->entries[0]->kind == EntryKind::Person && t->entries[0]->text == "hello" && t->entries[0]->closed);
    CHECK(t->entries[1]->kind == EntryKind::Friend && t->entries[1]->text == kReceived);
    CHECK(t->queue == std::vector<std::string>{"later"});
    CHECK(t->info == infoOf("ada"));
    CHECK(hub.state().find(sid("bo")) == nullptr);
}

void hub_limits() {
    MemoryStore store;
    Limits limits;
    limits.text = 10;
    limits.queue = 2;
    limits.queueBytes = 12;
    limits.sessions = 2;
    Hub hub(store, limits);
    withSessions(hub, {"ada", "bo"});
    std::string why;
    CHECK(!hub.apply(Action{1, ActionKind::Send, sid("ada"), "", std::string(11, 'x'), {}}, &why));
    CHECK(why.find("limit") != std::string::npos);
    CHECK(hub.state().find(sid("ada"))->entries.back()->kind == EntryKind::Notice);
    CHECK(!hub.apply(Action{2, ActionKind::Send, sid("ada"), "", " \t\n", {}}, &why));
    CHECK(hub.apply(Action{3, ActionKind::Queue, sid("ada"), "", "123456", {}}));
    CHECK(!hub.apply(Action{4, ActionKind::Queue, sid("ada"), "", "1234567", {}}, &why));   // 13 bytes queued: over
    CHECK(hub.apply(Action{5, ActionKind::Queue, sid("ada"), "", "12345", {}}));
    CHECK(!hub.apply(Action{6, ActionKind::Queue, sid("ada"), "", "1", {}}, &why));         // a third prompt: over
    CHECK(hub.state().find(sid("ada"))->queue.size() == 2);
    CHECK(hub.apply(Action{7, ActionKind::Send, sid("bo"), "", "hi", {}}));
    // a third session: refused, and none made
    CHECK(!hub.apply(ensure(8, "cy"), &why));
    CHECK(why.find("limit") != std::string::npos);
    CHECK(!hub.apply(create(9, "ada", 0), &why));
    CHECK(why.find("limit") != std::string::npos);
    CHECK(store.size() == 2 && store.nextId() == 3 && hub.state().sessions().size() == 2);
}

void hub_shares_untouched_sessions() {
    MemoryStore store;
    Hub hub(store);
    withSessions(hub, {"ada", "bo"});
    hub.apply(Action{1, ActionKind::Send, sid("ada"), "", "a", {}});
    hub.apply(Action{2, ActionKind::Send, sid("bo"), "", "b", {}});
    const Sessions before = hub.state();
    hub.apply(Action{3, ActionKind::Send, sid("bo"), "", "c", {}});
    CHECK(before.find(sid("ada")) == hub.state().find(sid("ada")));
    CHECK(before.find(sid("bo")) != hub.state().find(sid("bo")));
}

// A responder that records what reaches it: a send gives the prompt and the placeholder reply, a queue the prompt at
// the queue's end and, when `deliver` is set, the oldest taken at once (as a free friend does).
class RecordingResponder : public Responder {
public:
    SessionInfo defaults(const std::string& friendName) const override {
        return SessionInfo{friendName, "harness-" + friendName, "acct", "m0", "e0"};
    }
    std::vector<SessionChange> send(SessionId, const std::string& prompt) override {
        return {SessionChange{entry(EntryKind::Person, prompt), {}},
                SessionChange{entry(EntryKind::Friend, kReceived), {}}};
    }
    std::vector<SessionChange> queue(SessionId, const std::string& prompt) override {
        std::vector<SessionChange> out = {SessionChange{nullptr, prompt}};
        if (deliver) {
            out.push_back(takeQueued());
            out.push_back(SessionChange{entry(EntryKind::Person, prompt), {}});
        }
        return out;
    }
    // As the drivers do: a stop acts only once the store took the step (sent).
    std::vector<SessionChange> stop(SessionId id) override {
        pending = id;
        return {};
    }
    void sent(SessionId id, bool committed) override {
        sentTo.push_back(std::to_string(id) + (committed ? " committed" : " not committed"));
        if (committed && pending == id) {
            stopped.push_back(id);
        }
        pending = 0;
    }
    void configured(SessionId id, const SessionInfo& info) override {
        configuredTo.push_back({id, info});
    }
    bool deliver = false;
    SessionId pending = 0;
    std::vector<SessionId> stopped;
    std::vector<std::string> sentTo;
    std::vector<std::pair<SessionId, SessionInfo>> configuredTo;
};

void actions_stop_kind() {
    Actions a;
    a.add(act(ActionKind::Stop, sid("ada"), ""));
    const std::vector<Action> parsed = Actions::parse(a.init_diff());
    CHECK(parsed.size() == 1 && parsed[0].kind == ActionKind::Stop && parsed[0].session == 1 && parsed[0].text.empty());
}

// Every kind of action goes up and comes back as it was: its kind, session, friend and text; the kind past set effort
// is refused.
void actions_session_kinds() {
    Actions a;
    a.add(act(ActionKind::Create, 3, "", "ada"));
    a.add(act(ActionKind::Ensure, 0, "", "bo"));
    a.add(act(ActionKind::SetAccount, 7, "work"));
    a.add(act(ActionKind::SetModel, 7, "opus"));
    a.add(act(ActionKind::SetEffort, 7, "high"));
    const std::vector<Action> parsed = Actions::parse(a.init_diff());
    CHECK(parsed == a.held());
    CHECK(parsed.size() == 5 && parsed[0].session == 3 && parsed[0].friendName == "ada");
    CHECK(parsed[1].kind == ActionKind::Ensure && parsed[1].friendName == "bo" && parsed[1].session == 0);
    CHECK(parsed[4].kind == ActionKind::SetEffort && parsed[4].text == "high");
    Writer w;
    w.u64(1);
    w.u64(1);
    w.byte(9);
    w.u64(1);
    w.str("");
    w.str("");
    bool threw = false;
    try {
        Actions::parse(w.take());
    } catch (const BadDiff&) {
        threw = true;
    }
    CHECK(threw);
}

// An ensure makes a session only for a friend that has none: two windows ensuring one friend give it one session, and
// both are counted. Ids are minted from 1, in order.
void hub_ensure_makes_one() {
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    std::string why;
    CHECK(hub.apply(ensure(1, "ada"), &why, "w1"));
    CHECK(hub.apply(ensure(1, "ada"), &why, "w2"));
    CHECK(hub.apply(ensure(2, "bo"), &why, "w1"));
    CHECK(hub.apply(ensure(2, "bo"), &why, "w2"));
    CHECK(store.size() == 2 && store.nextId() == 3);
    CHECK(store.applied("w1") == 2 && store.applied("w2") == 2);
    CHECK(hub.state().find(1)->info == (SessionInfo{"ada", "harness-ada", "acct", "m0", "e0"}));
    CHECK(hub.state().find(2)->info.friendName == "bo");
    CHECK(hub.state().first("ada")->id == 1 && hub.state().first("cy") == nullptr);
    // refused, counted, nothing made: a friend named by nothing, or past the limit, or text carried
    CHECK(!hub.apply(ensure(3, ""), &why, "w1"));
    CHECK(!hub.apply(ensure(4, std::string(65, 'x')), &why, "w1"));
    CHECK(!hub.apply(numbered(5, act(ActionKind::Ensure, 0, "x", "cy")), &why, "w1"));
    CHECK(store.size() == 2 && store.applied("w1") == 5);
}

// A create makes a new session, even for a friend that has one, copying the account, model and effort of the session
// it names, or the friend's defaults; one naming a session the server does not hold is refused and counted.
void hub_create_copies_settings() {
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    std::string why;
    CHECK(hub.apply(create(1, "ada", 0), &why, "w"));
    CHECK(hub.apply(numbered(2, act(ActionKind::SetModel, 1, "opus")), &why, "w"));
    CHECK(hub.apply(numbered(3, act(ActionKind::SetEffort, 1, "high")), &why, "w"));
    CHECK(hub.apply(numbered(4, act(ActionKind::SetAccount, 1, "work")), &why, "w"));
    CHECK(hub.apply(create(5, "ada", 1), &why, "w"));
    CHECK(hub.apply(create(6, "bo", 1), &why, "w"));    // another friend: its own harness, the settings copied
    CHECK(!hub.apply(create(7, "ada", 9), &why, "w"));
    CHECK(why == "no session 9 to copy");
    CHECK(store.size() == 3 && store.applied("w") == 7);
    CHECK(hub.state().find(2)->info == (SessionInfo{"ada", "harness-ada", "work", "opus", "high"}));
    CHECK(hub.state().find(3)->info == (SessionInfo{"bo", "harness-bo", "work", "opus", "high"}));
    // the friend's first session is still the lowest id
    CHECK(hub.state().first("ada")->id == 1);
    // without a responder, the default harness and no settings
    MemoryStore plain;
    Hub bare(plain);
    CHECK(bare.apply(create(1, "ada", 0)));
    CHECK(bare.state().find(1)->info == infoOf("ada"));
}

// Ids are never reused: the store's counter outlives the hub, and a session once made stays.
void hub_ids_never_reused() {
    MemoryStore store;
    {
        Hub hub(store);
        withSessions(hub, {"ada", "bo"});
    }
    Hub again(store);
    CHECK(again.state().sessions().size() == 2);
    CHECK(again.apply(create(0, "ada", 0)));
    CHECK(again.state().find(3) != nullptr && store.nextId() == 4);
    // a store refuses to make a session out of order, or one it holds again
    std::string why;
    CHECK(!store.commit(9, {infoChange(infoOf("cy"))}, "", 0, &why));
    CHECK(!store.commit(5, {infoChange(infoOf("cy"))}, "", 0, &why));
    CHECK(store.commit(4, {infoChange(infoOf("cy"))}, "", 0, &why));
    CHECK(store.commit(2, {infoChange(infoOf("bo"))}, "", 0, &why));   // held: its info, replaced
    CHECK(store.size() == 4 && store.nextId() == 5);
    // a step that does not start with the info makes no session
    CHECK(!store.commit(5, {SessionChange{entry(EntryKind::Person, "x"), {}}, infoChange(infoOf("cy"))}, "", 0, &why));
    CHECK(store.size() == 4);
}

// A send, queue, stop or set naming a session the server does not hold changes nothing and makes nothing: refused
// and counted.
void hub_unknown_session_refused() {
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    std::string why;
    const ActionKind kinds[] = {ActionKind::Send, ActionKind::Queue, ActionKind::Stop, ActionKind::SetModel};
    std::uint64_t n = 0;
    for (ActionKind k : kinds) {
        CHECK(!hub.apply(numbered(++n, act(k, 1, k == ActionKind::Stop ? "" : "x")), &why, "w"));
        CHECK(why == "no session 1");
    }
    CHECK(store.size() == 0 && store.applied("w") == 4 && hub.state().sessions().empty());
    CHECK(r.sentTo.empty() && r.configuredTo.empty());
}

// A set changes the session's account, model or effort, durably, and only then tells the responder; a value too long
// or not printable is refused with a notice; the value it already has changes nothing.
void hub_settings() {
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    std::string why;
    CHECK(hub.apply(ensure(1, "ada"), &why, "w"));
    CHECK(hub.apply(numbered(2, act(ActionKind::SetModel, 1, "sonnet")), &why, "w"));
    CHECK(store.find(1)->info.model == "sonnet" && hub.state().find(1)->info.model == "sonnet");
    CHECK(r.configuredTo.size() == 1 && r.configuredTo[0].first == 1 && r.configuredTo[0].second.model == "sonnet");
    CHECK(hub.apply(numbered(3, act(ActionKind::SetModel, 1, "sonnet")), &why, "w"));   // already so
    CHECK(r.configuredTo.size() == 1);
    CHECK(!hub.apply(numbered(4, act(ActionKind::SetEffort, 1, std::string(65, 'e'))), &why, "w"));
    CHECK(why.find("The effort was not changed") != std::string::npos);
    CHECK(!hub.apply(numbered(5, act(ActionKind::SetAccount, 1, "a\nb")), &why, "w"));
    CHECK(hub.state().find(1)->entries.size() == 2 && hub.state().find(1)->entries[1]->kind == EntryKind::Notice);
    CHECK(store.find(1)->info.effort == "e0" && store.find(1)->info.account == "acct");
    CHECK(hub.apply(numbered(6, act(ActionKind::SetAccount, 1, "")), &why, "w"));      // back to the default
    CHECK(store.find(1)->info.account.empty() && r.configuredTo.size() == 2);
    CHECK(store.applied("w") == 6);
}

void hub_stop_reaches_the_responder_once() {
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    std::string why;
    // a session the server does not hold: refused, counted, and no session made
    CHECK(!hub.apply(Action{1, ActionKind::Stop, sid("ada"), "", "", {}}, &why, "w"));
    CHECK(why == "no session 1");
    CHECK(store.applied("w") == 1);
    CHECK(store.find(sid("ada")) == nullptr && hub.state().find(sid("ada")) == nullptr);
    CHECK(r.stopped.empty() && r.sentTo.empty());
    CHECK(hub.apply(ensure(2, "ada"), &why, "w"));
    CHECK(hub.apply(Action{3, ActionKind::Send, sid("ada"), "", "hi", {}}, &why, "w"));
    // a stop that carries text: refused, counted, and not passed on
    why.clear();
    CHECK(!hub.apply(Action{4, ActionKind::Stop, sid("ada"), "", "x", {}}, &why, "w"));
    CHECK(why == "a stop that carried text was not applied: a stop carries none");
    CHECK(store.applied("w") == 4);
    CHECK(r.stopped.empty());
    // a stop: to the responder, once, with nothing in the store
    const std::size_t entries = store.find(sid("ada"))->transcript.size();
    CHECK(hub.apply(Action{5, ActionKind::Stop, sid("ada"), "", "", {}}, &why, "w"));
    CHECK(r.stopped == std::vector<SessionId>{1});
    CHECK(r.sentTo.back() == "1 committed");
    CHECK(store.applied("w") == 5);
    CHECK(store.find(sid("ada"))->transcript.size() == entries);
    // the same number again is the store's to refuse: the Applier never gives it twice
    CHECK(!hub.apply(Action{5, ActionKind::Stop, sid("ada"), "", "", {}}, &why, "w"));
    CHECK(r.stopped.size() == 1 && r.sentTo.back() == "1 not committed");
}

void hub_stop_without_responder_changes_nothing() {
    MemoryStore store;
    Hub hub(store);
    withSessions(hub, {"ada"});
    CHECK(hub.apply(Action{1, ActionKind::Send, sid("ada"), "", "hi", {}}));
    const Sessions before = hub.state();
    CHECK(hub.apply(Action{2, ActionKind::Stop, sid("ada"), "", "", {}}));
    CHECK(hub.state() == before);
}

void hub_queue_goes_to_the_responder() {
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    withSessions(hub, {"ada"});
    CHECK(hub.apply(Action{1, ActionKind::Queue, sid("ada"), "", "later", {}}));
    CHECK(hub.state().find(sid("ada"))->queue == std::vector<std::string>{"later"});
    r.deliver = true;
    CHECK(hub.apply(Action{2, ActionKind::Queue, sid("ada"), "", "now", {}}));
    // the oldest left the queue, as the person's entry; the new one waits
    const SessionView* t = hub.state().find(sid("ada"));
    CHECK(t->queue == std::vector<std::string>{"now"});
    CHECK(t->entries.size() == 1 && t->entries[0]->kind == EntryKind::Person && t->entries[0]->text == "now");
    CHECK(r.sentTo.size() == 2 && r.sentTo[1] == "1 committed");
}

void memory_store_take_refusals() {
    MemoryStore store;
    std::string why;
    CHECK(store.commit(1, {infoChange(infoOf("ada"))}, "", 0, &why));
    CHECK(!store.commit(1, {takeQueued()}, "", 0, &why));
    CHECK(why == "session 1: a take from a queue with nothing left in it");
    // two takes of one prompt: the second finds nothing, and the step is taken whole or not at all
    CHECK(!store.commit(1, {SessionChange{nullptr, "one"}, takeQueued(), takeQueued()}, "", 0, &why));
    CHECK(store.find(1)->queue.empty());
    CHECK(store.commit(1, {SessionChange{nullptr, "one"}, SessionChange{nullptr, "two"}}, "", 0, &why));
    CHECK(store.commit(1, {takeQueued()}, "", 0, &why));
    CHECK(store.find(1)->queue == std::vector<std::string>{"two"});
}

void hub_open_entry() {
    MemoryStore store;
    Limits limits;
    limits.text = 8;
    Hub hub(store, limits);
    CHECK(!hub.setOpen(sid("ada"), "part"));                 // a session the store does not hold
    CHECK(hub.state().find(sid("ada")) == nullptr);
    withSessions(hub, {"ada"});
    CHECK(hub.apply(Action{1, ActionKind::Send, sid("ada"), "", "hi", {}}));
    CHECK(!hub.setOpen(sid("ada"), "123456789"));            // past the text limit
    CHECK(!hub.setOpen(sid("ada"), ""));                     // none to drop
    CHECK(hub.setOpen(sid("ada"), "part"));
    const SessionView* t = hub.state().find(sid("ada"));
    CHECK(t->entries.size() == 3 && !t->entries.back()->closed && t->entries.back()->text == "part");
    CHECK(t->entries.back()->kind == EntryKind::Friend);
    CHECK(store.find(sid("ada"))->transcript.size() == 2);   // in memory only
    CHECK(!hub.setOpen(sid("ada"), "part"));                 // no change
    CHECK(hub.setOpen(sid("ada"), "part two"));
    CHECK(hub.state().find(sid("ada"))->entries.back()->text == "part two");
    CHECK(hub.setOpen(sid("ada"), ""));
    CHECK(hub.state().find(sid("ada"))->entries.size() == 2);
    // a new hub over the same store has none: the open entry is not durable
    CHECK(hub.setOpen(sid("ada"), "lost"));
    Hub again(store, limits);
    CHECK(again.state().find(sid("ada"))->entries.size() == 2);
}

std::vector<FriendInfo> friends2();

// A session's activity and a tool line cross the wire; a change of activity alone is a change of the state; an
// activity this build does not know reads busy.
void sessions_activity_and_tool_lines() {
    std::vector<EntryRef> t = {entry(EntryKind::Person, "q"), entry(EntryKind::Tool, "Bash: ls src")};
    SessionView busy = tailFor("ada", t, {}, Tail{});
    busy.activity = Activity::Busy;
    SessionView down = busy;
    down.activity = Activity::Down;
    const Sessions a({std::make_shared<const SessionView>(busy)});
    const Sessions b({std::make_shared<const SessionView>(down)});
    CHECK(!(a == b));
    Sessions c;
    c.apply_string(a.init_diff());
    CHECK(c == a && c.find(sid("ada"))->activity == Activity::Busy);
    CHECK(c.find(sid("ada"))->entries[1]->kind == EntryKind::Tool);
    const std::string d = b.diff_from(a);
    CHECK(!d.empty());
    c.apply_string(d);
    CHECK(c == b && c.find(sid("ada"))->activity == Activity::Down);
    // The two whole diffs differ in one byte, the activity's: Down is 2. 3 is an activity this build does not know (a
    // later server's): it reads busy, the friend neither idle nor down, and the diff is not refused.
    std::string later = b.init_diff();
    const std::string good = a.init_diff();
    std::size_t at = later.size();
    for (std::size_t i = 0; i < later.size() && i < good.size(); ++i) {
        if (later[i] != good[i]) {
            at = i;
            break;
        }
    }
    CHECK(at < later.size() && later[at] == 2);
    if (at < later.size()) {
        later[at] = 3;
        Sessions e;
        e.apply_string(later);
        CHECK(e.find(sid("ada")) != nullptr && e.find(sid("ada"))->activity == Activity::Busy);
    }
}

// The two states of the format 1 fixtures (fixtures/main_state_fixture.cpp builds the same).
Sessions mainState(const std::string& open) {
    const std::vector<EntryRef> ada = {entry(EntryKind::Person, "Why did the nightly go red?"),
        entry(EntryKind::Friend, "Looking at the log."), entry(EntryKind::Notice, "Stopped.")};
    const std::vector<EntryRef> bo = {entry(EntryKind::Person, "hi")};
    SessionView a = tailOf(1, SessionInfo{"ada", "claude", "work", "opus", "high"}, ada, {"after that, the PR"}, Tail{},
        entry(EntryKind::Friend, open, false));
    SessionView b = tailOf(2, SessionInfo{"bo", "claude", "", "", ""}, bo, {}, Tail{});
    return Sessions({std::make_shared<const SessionView>(a), std::make_shared<const SessionView>(b)});
}

std::string fixture(const std::string& name) {
    std::ifstream in(std::string(NEXUS_SYNC_FIXTURES) + "/" + name, std::ios::binary);
    CHECK(static_cast<bool>(in));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Format 1 is what a nexus from before formats sends and reads, byte for byte: this build reads the bytes main wrote
// (an older server's state, in a newer window), and writes, for a link of format 1, the very bytes main writes (a
// newer server's state, in an older window). A session of format 1 reads idle; a tool line goes to it as the friend's
// text and its activity not at all.
void state_format_1_from_main() {
    const std::string init = fixture("main-state-1.init");
    const std::string grow = fixture("main-state-1.grow");
    Sessions before = mainState("The nightly");
    Sessions after = mainState("The nightly went red in the link walk");
    Sessions read;
    read.setFormat(1);
    read.apply_string(init);
    CHECK(read == before);
    read.apply_string(grow);
    CHECK(read == after);
    CHECK(read.find(1)->activity == Activity::Idle);
    before.setFormat(1);
    after.setFormat(1);
    CHECK(before.init_diff() == init);
    CHECK(after.diff_from(before) == grow);
    // What format 1 cannot carry.
    SessionView v = tailFor("ada", {entry(EntryKind::Tool, "Read: a file")}, {}, Tail{});
    v.activity = Activity::Busy;
    Sessions tool({std::make_shared<const SessionView>(v)});
    tool.setFormat(1);
    Sessions old;
    old.setFormat(1);
    old.apply_string(tool.init_diff());
    CHECK(old.find(sid("ada"))->entries[0]->kind == EntryKind::Friend);
    CHECK(old.find(sid("ada"))->entries[0]->text == "Read: a file");
    CHECK(old.find(sid("ada"))->activity == Activity::Idle);
}

// A reader of format 2 reads a later format: what that format adds after the fields it knows, in an entry's frame, a
// session's frame or after the sessions, is skipped; an entry kind it does not know reads as a notice (a plain dim
// line), and an activity it does not know as busy.
void state_format_2_reads_later_formats() {
    Writer entry1;
    entry1.byte(9);             // a kind of a later format
    entry1.byte(1);             // closed
    entry1.u64(0);
    entry1.str("a line of a kind this build does not know");
    entry1.str("a field a later format adds to an entry");
    Writer session;
    for (const char* s : {"ada", "claude", "", "", ""}) {
        session.str(s);
    }
    session.byte(7);            // an activity of a later format
    session.u64(0);             // first
    session.u64(0);             // from entry 0
    session.u64(1);             // one entry
    session.str(entry1.take());
    session.byte(1);            // the queue
    session.u64(1);
    session.str("queued");
    session.str("studio");      // the instance, appended to format 2 since it began
    session.u64(42);            // a field a later format adds to a session
    session.str("more of it");
    Writer diff;
    diff.u64(1);
    diff.u64(1);                // session 1
    diff.byte(1);
    diff.str(session.take());
    diff.str("a field a later format adds after the sessions");
    Sessions s;
    CHECK(s.format() == kStateFormat);
    s.apply_string(diff.take());
    const SessionView* v = s.find(1);
    CHECK(v != nullptr);
    if (v != nullptr) {
        CHECK(v->info.friendName == "ada" && v->activity == Activity::Busy);
        CHECK(v->entries.size() == 1 && v->entries[0]->kind == EntryKind::Notice && v->entries[0]->closed);
        CHECK(v->entries[0]->text == "a line of a kind this build does not know");
        CHECK(v->queue == std::vector<std::string>{"queued"});
        CHECK(v->info.instance == "studio");
    }
}

// A session of format 2 as main wrote it before instances (its frame ends with the queue), by hand: friend "ada",
// account "work", idle, one closed entry of the person's, "hi", and the queue empty. The instance is appended to the
// frame when given, as this build writes it.
std::string mainSessionFrame(const std::string* instance) {
    Writer entry1;
    entry1.byte(0);             // the person's
    entry1.byte(1);             // closed
    entry1.u64(0);
    entry1.str("hi");
    Writer session;
    for (const char* f : {"ada", "claude", "work", "", ""}) {
        session.str(f);
    }
    session.byte(0);            // idle
    session.u64(0);             // first
    session.u64(0);             // from entry 0
    session.u64(1);             // one entry
    session.str(entry1.take());
    session.byte(1);            // the queue, changed, empty
    session.u64(0);
    if (instance != nullptr) {
        session.str(*instance);
    }
    Writer diff;
    diff.u64(1);
    diff.u64(sid("ada"));
    diff.byte(1);
    diff.str(session.take());
    return diff.take();
}

// The instance crosses the wire as a field appended to format 2's session frame, so no format number changed: a
// diff main wrote before instances (old to new) reads each session on no instance, its friend's default; this build's
// diff is main's with the instance at the frame's end (new to old: a reader from before skips it, as format 2's readers
// skip what a frame adds); a diff that sends one session carries its instance, and a kept one keeps its own; and a
// window of format 1 is sent no instance, byte for byte main's (fixtures/main-state-1.init).
void sessions_instance_appended() {
    auto with = [](const std::string& ada, const std::string& bo) {
        std::vector<SessionRef> v;
        for (const auto& [name, inst] : {std::pair<std::string, std::string>{"ada", ada}, {"bo", bo}}) {
            SessionView t = tailOf(sid(name), SessionInfo{name, "claude", "work", "", "", inst},
                {entry(EntryKind::Person, "hi")}, {}, Tail{});
            v.push_back(std::make_shared<const SessionView>(std::move(t)));
        }
        return Sessions(std::move(v));
    };
    Sessions read;
    read.apply_string(mainSessionFrame(nullptr));
    const SessionView* a = read.find(sid("ada"));
    CHECK(a != nullptr && a->info.account == "work" && a->info.instance.empty());
    CHECK(a != nullptr && a->entries.size() == 1 && a->entries[0]->text == "hi");
    // New to old: this build's bytes are main's, the instance appended inside the frame.
    SessionView one = tailOf(sid("ada"), SessionInfo{"ada", "claude", "work", "", "", "studio"},
        {entry(EntryKind::Person, "hi")}, {}, Tail{});
    const Sessions b({std::make_shared<const SessionView>(one)});
    const std::string studio = "studio";
    CHECK(b.init_diff() == mainSessionFrame(&studio));
    Sessions c;
    c.apply_string(b.init_diff());
    CHECK(c == b && c.find(sid("ada"))->info.instance == "studio");
    // One session sent, one kept.
    const Sessions both = with("studio", "");
    const Sessions moved = with("studio", "laptop");
    Sessions d;
    d.apply_string(both.init_diff());
    CHECK(d == both);
    d.apply_string(moved.diff_from(both));
    CHECK(d == moved && d.find(sid("bo"))->info.instance == "laptop" && d.find(sid("ada"))->info.instance == "studio");
    // Format 1: no instance, main's bytes exactly.
    const Sessions mainOne = mainState("The nightly");
    std::vector<SessionRef> placed;
    for (const SessionRef& r : mainOne.sessions()) {
        SessionView v = *r;
        v.info.instance = "studio";
        placed.push_back(std::make_shared<const SessionView>(std::move(v)));
    }
    Sessions withInstances(std::move(placed));
    withInstances.setFormat(1);
    CHECK(withInstances.init_diff() == fixture("main-state-1.init"));
}

// The link's format is the older of the two stated, and a floor refuses an older end in one plain line; no format is
// refused today.
void link_formats_agree() {
    CHECK(linkFormat(1, kStateFormat) == 1);
    CHECK(linkFormat(kStateFormat, 1) == 1);
    CHECK(linkFormat(kStateFormat + 5, kStateFormat) == kStateFormat);
    CHECK(windowRefusal(1).empty() && serverRefusal(1).empty());
    CHECK(windowRefusal(kStateFormat).empty() && serverRefusal(kStateFormat).empty());
    const std::string window = windowRefusal(1, 2);
    CHECK(window.find("too old") != std::string::npos && window.find('\n') == std::string::npos);
    const std::string server = serverRefusal(1, 2);
    CHECK(server.find("nexus-server is too old") != std::string::npos && server.find("nexus-server stop") !=
        std::string::npos && server.find('\n') == std::string::npos);
}

// A state file says its format and is read in it: this build's round trip; one of format 1 (main's bytes); one of a
// later format read as this build reads a later server's; and refused, in one line, what is not a state file.
void state_files() {
    Sessions busy = mainState("The nightly");
    std::string why;
    Sessions read;
    CHECK(stateFile(busy).rfind("nexus-state 2\n", 0) == 0);
    CHECK(readStateFile(stateFile(busy), &read, &why) && read == busy);
    CHECK(readStateFile("nexus-state 1\n" + fixture("main-state-1.init"), &read, &why) && read == busy);
    CHECK(readStateFile("nexus-state 9\n" + busy.init_diff(), &read, &why) && read == busy);
    for (const std::string& bad : {std::string(), std::string("nexus-state\n"), std::string("nexus-state 0\n"),
                                   std::string("nexus-state 2x\n"), busy.init_diff()}) {
        why.clear();
        CHECK(!readStateFile(bad, &read, &why) && why.find("not a state file") == 0 && why.find('\n') ==
              std::string::npos);
    }
    why.clear();
    CHECK(!readStateFile("nexus-state 2\nnot a diff", &read, &why) && why.find("not a state of format 2") == 0);
}

// The hub's activity of a session: in memory only, Idle until set, refused for a session the store does not hold.
void hub_activity() {
    MemoryStore store;
    Hub hub(store);
    CHECK(!hub.setActivity(sid("ada"), Activity::Busy));
    withSessions(hub, {"ada", "bo"});
    CHECK(hub.state().find(sid("ada"))->activity == Activity::Idle);
    CHECK(!hub.setActivity(sid("ada"), Activity::Idle));     // no change
    const SessionRef bo = hub.state().sessions()[1];
    CHECK(hub.setActivity(sid("ada"), Activity::Busy));
    CHECK(hub.state().find(sid("ada"))->activity == Activity::Busy);
    CHECK(hub.state().sessions()[1] == bo);                  // the other session is shared, untouched
    CHECK(!hub.setActivity(sid("ada"), Activity::Busy));
    CHECK(hub.setActivity(sid("ada"), Activity::Down));
    CHECK(hub.state().find(sid("ada"))->activity == Activity::Down);
    // it stays as the session changes
    CHECK(hub.apply(Action{1, ActionKind::Send, sid("ada"), "", "hi", {}}));
    CHECK(hub.state().find(sid("ada"))->activity == Activity::Down);
    CHECK(hub.setActivity(sid("ada"), Activity::Idle));
    CHECK(hub.state().find(sid("ada"))->activity == Activity::Idle);
    // a new hub over the same store starts Idle: not durable
    CHECK(hub.setActivity(sid("bo"), Activity::Busy));
    Hub again(store);
    CHECK(again.state().find(sid("bo"))->activity == Activity::Idle);
}

// The mirror holds the activity of the session a tab shows from the last state applied that held it.
void mirror_activity() {
    Model m(friends2(), false);
    Mirror mirror;
    const std::size_t id = m.tab(0).id;
    CHECK(mirror.activity(id) == nullptr);
    std::vector<EntryRef> t = {entry(EntryKind::Person, "q")};
    SessionView v = tailFor("ada", t, {}, Tail{});
    v.activity = Activity::Busy;
    CHECK(mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)})));
    CHECK(mirror.activity(id) != nullptr && *mirror.activity(id) == Activity::Busy);
    CHECK(!mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)})));    // the same: no change
    v.activity = Activity::Idle;
    CHECK(mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)})));     // the activity alone changed
    CHECK(*mirror.activity(id) == Activity::Idle);
    CHECK(mirror.activity(m.tab(1).id) == nullptr);          // bo's: no state has held one
    CHECK(mirror.apply(m, Sessions()));                       // a state without the session
    CHECK(mirror.activity(id) == nullptr);
}

// A state without the tab's session takes its open entry with it: no growing text is left to draw beside the grey
// cursor.
void mirror_drops_open_with_the_session() {
    Model m(friends2(), false);
    Mirror mirror;
    const std::size_t id = m.tab(0).id;
    SessionView v = tailFor("ada", {entry(EntryKind::Person, "q")}, {}, Tail{}, entry(EntryKind::Friend, "gro", false));
    v.activity = Activity::Busy;
    CHECK(mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)})));
    CHECK(mirror.open(id) != nullptr && mirror.open(id)->text == "gro");
    CHECK(mirror.apply(m, Sessions()));
    CHECK(mirror.open(id) == nullptr);
    CHECK(mirror.activity(id) == nullptr);
    CHECK(!mirror.apply(m, Sessions()));      // nothing left to drop
}

// The state cursor from the server's state only: grey whenever the link is lost or no state has held the session, so
// never blue on stale state; otherwise the session's activity.
void cursor_from_the_servers_state() {
    Model m(friends2(), false);
    Mirror mirror;
    const std::size_t id = m.tab(0).id;
    CHECK(cursorOf(LinkPhase::Connected, 0, mirror, id) == CursorState::Lost);     // no session yet
    SessionView v = tailFor("ada", {}, {}, Tail{});
    for (Activity a : {Activity::Busy, Activity::Idle, Activity::Down}) {
        v.activity = a;
        mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)}));
        const CursorState want = a == Activity::Busy ? CursorState::Busy
            : a == Activity::Idle ? CursorState::Idle : CursorState::Down;
        CHECK(cursorOf(LinkPhase::Connected, 0, mirror, id) == want);
        CHECK(cursorOf(LinkPhase::Connected, kQuietMs - 1, mirror, id) == want);
        CHECK(cursorOf(LinkPhase::Connected, kQuietMs, mirror, id) == CursorState::Lost);
        CHECK(cursorOf(LinkPhase::Connecting, 0, mirror, id) == CursorState::Lost);
        CHECK(cursorOf(LinkPhase::Failed, 0, mirror, id) == CursorState::Lost);
    }
    CHECK(!linkLost(LinkPhase::Connected, kQuietMs - 1));
    CHECK(linkLost(LinkPhase::Connected, kQuietMs));
}

void sessions_tail_carries_open_apart() {
    std::vector<EntryRef> t;
    for (int i = 0; i < 5; ++i) {
        t.push_back(entry(EntryKind::Person, std::to_string(i)));
    }
    const SessionView v = tailFor("ada", t, {}, Tail{2, 1000}, entry(EntryKind::Friend, "open", false));
    CHECK(v.first == 3 && v.entries.size() == 3 && v.total() == 6);
    CHECK(v.entries[0]->text == "3" && v.entries[1]->text == "4" && !v.entries[2]->closed);
    // an open entry past the byte budget still comes with the newest closed one
    const SessionView big = tailFor("ada", t, {}, Tail{2, 10}, entry(EntryKind::Friend, std::string(100, 'x'), false));
    CHECK(big.first == 3 && big.entries.size() == 3 && big.entries[1]->text == "4");
    const SessionView none = tailFor("ada", {}, {}, Tail{2, 10}, entry(EntryKind::Friend, "o", false));
    CHECK(none.first == 0 && none.entries.size() == 1);
}

// ---- the window's mirror ----

std::vector<FriendInfo> friends2() {
    return {{"ada", "m1"}, {"bo", "m2"}};
}

Sessions stateOf(const std::string& name, const std::vector<EntryRef>& t, std::vector<std::string> queue,
                 Tail tail = {}) {
    return Sessions({std::make_shared<const SessionView>(tailFor(name, t, std::move(queue), tail))});
}

void mirror_appends_in_order_once() {
    Model m(friends2(), true);
    Mirror mirror;
    std::vector<EntryRef> t = {entry(EntryKind::Person, "hi"), entry(EntryKind::Friend, kReceived)};
    CHECK(mirror.apply(m, stateOf("ada", t, {})));
    CHECK(m.tab(1).transcript.size() == 2 && m.tab(1).transcript[1].text == kReceived);
    CHECK(!mirror.apply(m, stateOf("ada", t, {})));     // the same state again: nothing
    CHECK(m.tab(1).transcript.size() == 2);
    t.push_back(entry(EntryKind::Person, "more"));
    mirror.apply(m, stateOf("ada", t, {}));
    CHECK(m.tab(1).transcript.size() == 3 && m.tab(1).transcript[2].text == "more");
    CHECK(m.tab(2).transcript.empty());
    CHECK(mirror.seen(m.tab(1).id).next == 3);
}

void mirror_marks_entries_not_received() {
    Model m(friends2(), false);
    Mirror mirror;
    std::vector<EntryRef> t;
    for (int i = 0; i < 10; ++i)
        t.push_back(entry(EntryKind::Person, std::to_string(i)));
    mirror.apply(m, stateOf("bo", t, {}, Tail{3, 1000}));
    const auto& tr = m.tab(1).transcript;
    CHECK(tr.size() == 4);
    CHECK(tr[0].kind == EntryKind::Notice && tr[0].text == notShownNotice(7));
    CHECK(tr[1].text == "7" && tr[3].text == "9");
    CHECK(notShownNotice(1) == "1 earlier entry not shown");
}

void mirror_waits_for_open_entry() {
    Model m(friends2(), false);
    Mirror mirror;
    std::vector<EntryRef> t = {entry(EntryKind::Person, "q"), entry(EntryKind::Friend, "part", false)};
    mirror.apply(m, stateOf("ada", t, {}));
    CHECK(m.tab(0).transcript.size() == 1);
    t.back() = entry(EntryKind::Friend, "part and the rest", true);
    mirror.apply(m, stateOf("ada", t, {}));
    CHECK(m.tab(0).transcript.size() == 2 && m.tab(0).transcript[1].text == "part and the rest");
}

void mirror_queue_keeps_draft() {
    Model m(friends2(), false);
    Mirror mirror;
    m.set_draft(0, "typing");
    mirror.apply(m, stateOf("ada", {}, {"one", "two"}));
    CHECK(m.tab(0).queue == std::vector<std::string>({"one", "two"}));
    CHECK(m.tab(0).draft == "typing");
    CHECK(m.tab(0).transcript.empty());
    mirror.apply(m, stateOf("ada", {}, {"one", "two", "three"}));
    CHECK(m.tab(0).queue.size() == 3 && m.tab(0).queue[2] == "three");
}

Sessions openState(const std::vector<EntryRef>& t, EntryRef open, Tail tail = {}) {
    return Sessions({std::make_shared<const SessionView>(tailFor("ada", t, {}, tail, std::move(open)))});
}

void mirror_holds_open_entry_apart() {
    Model m(friends2(), false);
    Mirror mirror;
    const std::size_t id = m.tab(0).id;
    std::vector<EntryRef> t = {entry(EntryKind::Person, "q")};
    CHECK(mirror.open(id) == nullptr);
    CHECK(mirror.apply(m, openState(t, entry(EntryKind::Friend, "pa", false))));
    CHECK(m.tab(0).transcript.size() == 1);
    CHECK(mirror.open(id) != nullptr && mirror.open(id)->text == "pa" && !mirror.open(id)->closed);
    CHECK(mirror.seen(id).next == 1);
    CHECK(!mirror.apply(m, openState(t, entry(EntryKind::Friend, "pa", false))));  // the same: no change
    CHECK(mirror.apply(m, openState(t, entry(EntryKind::Friend, "part", false))));  // grown
    CHECK(mirror.open(id)->text == "part" && m.tab(0).transcript.size() == 1);
    // closed: appended once, and no longer held
    t.push_back(entry(EntryKind::Friend, "part one"));
    CHECK(mirror.apply(m, openState(t, nullptr)));
    CHECK(mirror.open(id) == nullptr);
    CHECK(m.tab(0).transcript.size() == 2 && m.tab(0).transcript[1].text == "part one");
    CHECK(!mirror.apply(m, openState(t, nullptr)));
    CHECK(m.tab(0).transcript.size() == 2);
    // a window away for longer than the tail: the notice, the newest closed entry, and the open one held
    for (int i = 0; i < 6; ++i) {
        t.push_back(entry(EntryKind::Person, std::to_string(i)));
    }
    CHECK(mirror.apply(m, openState(t, entry(EntryKind::Friend, "o", false), Tail{1, 1000})));
    const auto& tr = m.tab(0).transcript;
    CHECK(tr.size() == 4 && tr[2].text == notShownNotice(5) && tr[3].text == "5");
    CHECK(mirror.open(id)->text == "o");
}

void mirror_queue_follows_the_servers() {
    Model m(friends2(), false);
    Mirror mirror;
    m.set_draft(0, "typing");
    mirror.apply(m, stateOf("ada", {}, {"one", "two"}));
    // the server took the oldest and queued another
    CHECK(mirror.apply(m, stateOf("ada", {}, {"two", "three"})));
    CHECK(m.tab(0).queue == std::vector<std::string>({"two", "three"}));
    CHECK(m.tab(0).draft == "typing");
    // took both
    CHECK(mirror.apply(m, stateOf("ada", {}, {})));
    CHECK(m.tab(0).queue.empty());
    // equal prompts: the queue still ends as the server's
    mirror.apply(m, stateOf("ada", {}, {"x", "x"}));
    CHECK(mirror.apply(m, stateOf("ada", {}, {"x"})));
    CHECK(m.tab(0).queue == std::vector<std::string>({"x"}));
    CHECK(!mirror.apply(m, stateOf("ada", {}, {"x"})));
    CHECK(m.tab(0).draft == "typing");
}

void mirror_ignores_other_tabs() {
    Model m(friends2(), true);
    Mirror mirror;
    CHECK(!mirror.apply(m, stateOf("zed", {entry(EntryKind::Person, "x")}, {"q"})));
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        CHECK(m.tab(i).transcript.empty() && m.tab(i).queue.empty());
}

// The session a tab shows is the window's own: its friend's first (lowest id) in the first state that holds one, and
// that one from then on, whatever sessions the friend gets later; nothing until then.
void mirror_shows_first_session() {
    Model m(friends2(), false);
    Mirror mirror;
    const std::size_t ada = m.tab(0).id;
    CHECK(mirror.session(ada) == 0 && sessionOf(m, 0, mirror) == 0);
    CHECK(!mirror.apply(m, Sessions()));
    CHECK(mirror.session(ada) == 0);
    auto view = [](SessionId id, const std::string& name, const std::string& text) {
        return std::make_shared<const SessionView>(tailOf(id, infoOf(name), {entry(EntryKind::Person, text)}, {},
                                                          Tail{}));
    };
    CHECK(mirror.apply(m, Sessions({view(2, "bo", "b"), view(4, "ada", "four"), view(6, "ada", "six")})));
    CHECK(mirror.session(ada) == 4 && sessionOf(m, 0, mirror) == 4 && sessionOf(m, 1, mirror) == 2);
    CHECK(m.tab(0).transcript.size() == 1 && m.tab(0).transcript[0].text == "four");
    // a later state with a lower id for the friend (never so on one server, but the choice stays put)
    CHECK(!mirror.apply(m, Sessions({view(1, "ada", "one"), view(2, "bo", "b"), view(4, "ada", "four")})));
    CHECK(mirror.session(ada) == 4 && m.tab(0).transcript.size() == 1);
    CHECK(sessionOf(m, 9, mirror) == 0);
}

// ---- the window's rules ----

void link_status_lines() {
    CHECK(linkStatus(LinkPhase::Connecting, "studio", 0, "") == "Connecting to studio");
    CHECK(linkStatus(LinkPhase::Failed, "studio", 0, "ssh failed") == "studio: ssh failed");
    CHECK(linkStatus(LinkPhase::Connected, "studio", 5999, "").empty());
    CHECK(linkStatus(LinkPhase::Connected, "studio", 12500, "") == "No contact with studio for 12 s");
}

void take_prompt_rules() {
    Model m(friends2(), true);
    std::string text;
    std::string why;
    CHECK(!takePrompt(m, 0, 100, &text, &why));             // the sprint tab
    m.set_draft(1, " \t");
    CHECK(!takePrompt(m, 1, 100, &text, &why));             // blank
    CHECK(m.tab(1).draft == " \t" && why.empty());
    m.set_draft(1, std::string(2049, 'x'));
    CHECK(!takePrompt(m, 1, 2048, &text, &why));            // too long
    CHECK(why == "This prompt is 3 KiB; the limit is 2 KiB.");
    m.set_draft(1, " hi ");
    CHECK(takePrompt(m, 1, 100, &text, &why));
    CHECK(text == " hi " && m.tab(1).draft.empty() && m.tab(1).transcript.empty());
    CHECK(!takePrompt(m, 9, 100, &text, &why));
    CHECK(blankText("") && blankText(" \r\n\t\v\f") && !blankText(" a"));
}

void ensure_each_friend() {
    Model m(friends2(), true);
    m.add_terminal();
    CHECK((friendsToEnsure(m) == std::vector<std::string>{"ada", "bo"}));
    CHECK(friendsToEnsure(Model({}, true)).empty());
}

void stop_target_rules() {
    Model m(friends2(), true);
    Mirror mirror;
    SessionId session = 0;
    CHECK(!stopTarget(m, 1, mirror, &session) && session == 0);     // no session shown yet
    mirror.apply(m, stateOf("ada", {}, {}));
    CHECK(!stopTarget(m, 0, mirror, &session) && session == 0);     // the sprint tab
    CHECK(stopTarget(m, 1, mirror, &session) && session == 1);      // a friend's, working or not
    CHECK(!stopTarget(m, 9, mirror, &session));                     // past the tabs
    CHECK(!stopTarget(m, 2, mirror, &session));                     // a friend with no session yet
    const std::size_t terminal = m.tab_count();
    m.add_terminal();
    session = 0;
    CHECK(!stopTarget(m, terminal, mirror, &session) && session == 0);  // a terminal's
}

// The window never filters a stop: whatever the session shows and whether or not the link is up, Command-Escape on a
// friend's tab that shows a session is an action in the outbox, sent in order with the others once the link carries
// them. The server decides each against its state when it processes it.
void stop_never_filtered_by_the_window() {
    Model m(friends2(), true);
    Mirror mirror;
    mirror.apply(m, stateOf("ada", {}, {}));    // the link was up: the tab shows ada's session
    Actions outbox;                             // the link is down: nothing leaves the window
    SessionId session = 0;
    CHECK(stopTarget(m, 1, mirror, &session) && session == 1);      // the session shows no turn
    outbox.add(act(ActionKind::Stop, session, ""));
    m.set_draft(1, "hi");
    std::string text;
    CHECK(takePrompt(m, 1, 1024, &text, nullptr));
    outbox.add(act(ActionKind::Send, sessionOf(m, 1, mirror), text));
    for (int i = 0; i < 2; ++i) {
        session = 0;
        CHECK(stopTarget(m, 1, mirror, &session) && session == 1);
        outbox.add(act(ActionKind::Stop, session, ""));
    }
    outbox.add(act(ActionKind::Stop, 7, ""));   // a session the server does not hold
    CHECK(outbox.held().size() == 5);
    // the link comes up: the whole outbox reaches the server, in order
    MemoryStore store;
    RecordingResponder r;
    Hub hub(store, Limits{}, &r);
    withSessions(hub, {"ada"});
    Applier applier;
    std::vector<std::string> outcomes;
    for (const Action& a : applier.apply(Actions::parse(outbox.init_diff()))) {
        std::string why;
        outcomes.push_back(hub.apply(a, &why, "w") ? "ok" : why);
    }
    CHECK((outcomes == std::vector<std::string>{"ok", "ok", "ok", "ok", "no session 7"}));
    CHECK((r.stopped == std::vector<SessionId>{1, 1, 1}));
    CHECK(store.applied("w") == 5);
}

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    {"wire_roundtrip", wire_roundtrip},
    {"wire_refuses_truncation", wire_refuses_truncation},
    {"actions_numbered_from_one", actions_numbered_from_one},
    {"actions_diff_apply_random_pairs", actions_diff_apply_random_pairs},
    {"actions_apply_is_idempotent_and_ignores_old", actions_apply_is_idempotent_and_ignores_old},
    {"actions_gap_refused", actions_gap_refused},
    {"actions_subtract", actions_subtract},
    {"actions_diff_from_misuse_throws", actions_diff_from_misuse_throws},
    {"actions_malformed_refused_unchanged", actions_malformed_refused_unchanged},
    {"applier_exactly_once", applier_exactly_once},
    {"actions_through_mosh_bookkeeping", actions_through_mosh_bookkeeping},
    {"sessions_diff_apply_random_pairs", sessions_diff_apply_random_pairs},
    {"sessions_open_entry_sends_only_growth", sessions_open_entry_sends_only_growth},
    {"sessions_diff_bounded_by_tail", sessions_diff_bounded_by_tail},
    {"sessions_copy_is_cheap_and_shared", sessions_copy_is_cheap_and_shared},
    {"sessions_removed_session", sessions_removed_tab},
    {"sessions_instance_appended", sessions_instance_appended},
    {"sessions_malformed_refused_unchanged", sessions_malformed_refused_unchanged},
    {"sessions_through_mosh_bookkeeping", sessions_through_mosh_bookkeeping},
    {"hub_send_and_queue", hub_send_and_queue},
    {"hub_limits", hub_limits},
    {"hub_shares_untouched_sessions", hub_shares_untouched_sessions},
    {"mirror_appends_in_order_once", mirror_appends_in_order_once},
    {"mirror_marks_entries_not_received", mirror_marks_entries_not_received},
    {"mirror_waits_for_open_entry", mirror_waits_for_open_entry},
    {"mirror_queue_keeps_draft", mirror_queue_keeps_draft},
    {"mirror_ignores_other_sessions", mirror_ignores_other_tabs},
    {"mirror_shows_first_session", mirror_shows_first_session},
    {"link_status_lines", link_status_lines},
    {"take_prompt_rules", take_prompt_rules},
    {"actions_stop_kind", actions_stop_kind},
    {"actions_session_kinds", actions_session_kinds},
    {"hub_ensure_makes_one", hub_ensure_makes_one},
    {"hub_create_copies_settings", hub_create_copies_settings},
    {"hub_ids_never_reused", hub_ids_never_reused},
    {"hub_unknown_session_refused", hub_unknown_session_refused},
    {"hub_settings", hub_settings},
    {"hub_stop_reaches_the_responder_once", hub_stop_reaches_the_responder_once},
    {"hub_stop_without_responder_changes_nothing", hub_stop_without_responder_changes_nothing},
    {"hub_queue_goes_to_the_responder", hub_queue_goes_to_the_responder},
    {"memory_store_take_refusals", memory_store_take_refusals},
    {"hub_open_entry", hub_open_entry},
    {"sessions_tail_carries_open_apart", sessions_tail_carries_open_apart},
    {"mirror_holds_open_entry_apart", mirror_holds_open_entry_apart},
    {"mirror_queue_follows_the_servers", mirror_queue_follows_the_servers},
    {"ensure_each_friend", ensure_each_friend},
    {"stop_target_rules", stop_target_rules},
    {"stop_never_filtered_by_the_window", stop_never_filtered_by_the_window},
    {"sessions_activity_and_tool_lines", sessions_activity_and_tool_lines},
    {"hub_activity", hub_activity},
    {"mirror_activity", mirror_activity},
    {"mirror_drops_open_with_the_session", mirror_drops_open_with_the_session},
    {"state_format_1_from_main", state_format_1_from_main},
    {"state_format_2_reads_later_formats", state_format_2_reads_later_formats},
    {"link_formats_agree", link_formats_agree},
    {"state_files", state_files},
    {"cursor_from_the_servers_state", cursor_from_the_servers_state},
};

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: sync_test [case]\n");
        return 2;
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Case& c : kCases)
            std::printf("%s\n", c.name);
        return 0;
    }
    std::size_t ran = 0;
    for (const Case& c : kCases) {
        if (argc == 2 && std::strcmp(argv[1], c.name) != 0)
            continue;
        const int before = failures;
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: threw %s\n", c.name, e.what());
            ++failures;
        }
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", c.name);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "sync_test: no case named %s (--list lists them)\n", argc == 2 ? argv[1] : "");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
