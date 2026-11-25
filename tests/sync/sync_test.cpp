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
#include <functional>
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
#include "sync/codec.h"
#include "sync/friend_strip.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#include "sync/sessions.h"
#include "sync/wire.h"
#include "crafted_tables.h"

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
    // Only the three bytes it grew by, not the 2000 it held: in format 4 a diff also carries schema's table of the
    // field ids it uses (about 200 bytes), and in format 3 nothing else (under 40).
    const std::string d = b.diff_from(a);
    CHECK(d.size() < 400);
    Sessions a3 = a;
    Sessions b3 = b;
    a3.setFormat(3);
    b3.setFormat(3);
    CHECK(b3.diff_from(a3).size() < 40);
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
    // 16 entries of about 110 bytes, and a table of field ids in format 4 (about 200 bytes once per diff, sessions_
    // open_entry_sends_only_growth)
    const std::size_t bound = 16 * 120 + 400;
    const std::string fromEmpty = b.init_diff();
    CHECK(fromEmpty.size() < bound);
    // far behind: the tail, not the history
    std::vector<EntryRef> old(t.begin(), t.begin() + 10);
    const Sessions a({std::make_shared<const SessionView>(tailFor("ada", old, {}, tail))});
    CHECK(b.diff_from(a).size() < bound);
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
        a.setFormat(3);
        bool threw = false;
        try {
            a.apply_string(w.take());
        } catch (const BadDiff&) {
            threw = true;
        }
        CHECK(threw && a == b);
    }
    // the same in format 4, as schema's table: well formed, and refused for what it says
    {
        const std::string bytes = synctest::craftedOverflowDiff();
        CHECK(!bytes.empty());
        Sessions a = b;
        bool threw = false;
        try {
            a.apply_string(bytes);
        } catch (const BadDiff& x) {
            threw = std::string(x.what()).find("past the last entry number") != std::string::npos;
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
    const auto epoch = t->info.contextEpoch;
    for (unsigned i = 0; i < 300; ++i)
        CHECK(hub.apply(Action{i + 3, ActionKind::Send, sid("ada"), "", std::to_string(i), {}}));
    std::uint64_t offset = 0;
    while (offset < hub.state().find(sid("ada"))->total()) {
        Sessions decoded;
        decoded.apply_string(hub.historyPage(sid("ada"), epoch, offset).init_diff());
        const auto* page = decoded.find(sid("ada"));
        CHECK(page && page->first == offset && !page->entries.empty() && page->entries.size() <= 256);
        if (!page || page->entries.empty()) break;
        for (const auto& entry : page->entries) CHECK(*entry == *store.find(sid("ada"))->transcript[offset++]);
    }
    CHECK(hub.historyPage(sid("ada"), epoch + 1, 0).sessions().empty());
}

void hub_primary_requires_an_installer_binding() {
    MemoryStore store;
    Hub hub(store);
    std::string why;
    const SessionInfo binding{"@primary", "claude", "work", {}, {}, std::string(32, 'a')};
    CHECK(!hub.ensurePrimary(binding, &why));  // no authoritative registry yet

    accounts::Registry registry;
    registry.accounts = {{"work", "claude"}, {"metered", "claude", accounts::Billing::Api}};
    CHECK(hub.useAccounts(registry, nullptr).empty());
    CHECK(!hub.apply(create(1, "@primary", 0), &why, "window"));
    CHECK(!hub.apply(ensure(2, "@primary"), &why, "window"));
    CHECK(store.applied("window") == 2);
    const std::vector<SessionInfo> invalid = {
        {"named", "claude", "work", {}, {}, binding.instance},
        {"@primary", "claude", "missing", {}, {}, binding.instance},
        {"@primary", "codex", "work", {}, {}, binding.instance},
        {"@primary", "claude", "metered", {}, {}, binding.instance},
        {"@primary", "claude", "work", {}, {}, "invalid-key"},
    };
    for (const auto& rejected : invalid) {
        why.clear();
        CHECK(!hub.ensurePrimary(rejected, &why));
        CHECK(!why.empty());
    }
    CHECK(!hub.primarySession() && store.size() == 0);
    CHECK(hub.state().friendsKnown() && hub.state().friends().empty());
}

void hub_primary_session() {
    MemoryStore store;
    accounts::Registry registry;
    registry.accounts = {{"work", "claude"}, {"other", "claude"}};
    const SessionInfo work{"@primary", "claude", "work", {}, {}, std::string(32, 'a')};
    std::string why;
    SessionId original = 0;
    {
        Hub hub(store);
        CHECK(hub.useAccounts(registry, nullptr).empty());
        CHECK(!hub.signedIn().contains("work"));
        CHECK(hub.ensurePrimary(work, &why)); // opening the chat does not require a completed sign-in
        original = hub.primarySession().value_or(0);
        CHECK(original != 0 && hub.isPrimary(original));
        CHECK(hub.ensurePrimary(work, &why));
        CHECK(store.size() == 1 && hub.primarySession() == original);
        CHECK(hub.state().friendsKnown() && hub.state().friends().empty());
        hub.reportSignedIn("work", true);
        CHECK(hub.apply(numbered(1, act(ActionKind::Send, original, "before restart")), &why, "window"));
    }

    Hub restarted(store);
    restarted.useAccounts(registry, nullptr);
    CHECK(!restarted.primarySession() && !restarted.isPrimary(original));
    CHECK(!restarted.state().find(original));
    CHECK(!restarted.apply(numbered(2, act(ActionKind::Send, original, "still dormant")), &why, "window"));
    CHECK(restarted.ensurePrimary(work, &why) && restarted.primarySession() == original);
    const auto* restored = store.find(original);
    CHECK(restored && restored->info.account == "work" && restored->info.instance == work.instance);
    CHECK(restored && !restored->transcript.empty() && restored->transcript.front()->text == "before restart");
    if (!restored) return;
    const auto transcript = restored->transcript;

    auto other = work;
    other.account = "other";
    CHECK(restarted.ensurePrimary(other, &why));
    const auto second = restarted.primarySession().value_or(0);
    CHECK(second != 0 && second != original && restarted.isPrimary(second));
    CHECK(!restarted.isPrimary(original) && !restarted.state().find(original));
    CHECK(!restarted.apply(numbered(3, act(ActionKind::Send, original, "wrong account")), &why, "window"));
    CHECK(store.find(original)->transcript == transcript);
    CHECK(store.find(second) && store.find(second)->transcript.empty());
    CHECK(restarted.state().friends().empty());

    other.instance = std::string(32, 'b');
    CHECK(restarted.ensurePrimary(other, &why));
    const auto third = restarted.primarySession().value_or(0);
    CHECK(third != 0 && third != second && !restarted.state().find(second));
    restarted.clearPrimary();
    CHECK(!restarted.primarySession() && !restarted.isPrimary(third) && !restarted.state().find(third));
    CHECK(!restarted.apply(numbered(4, act(ActionKind::Queue, third, "cleared")), &why, "window"));
    CHECK(store.find(third) && store.find(third)->queue.empty());

    CHECK(restarted.ensurePrimary(work, &why) && restarted.primarySession() == original);
    CHECK(store.size() == 3 && store.find(original)->transcript == transcript);

    std::vector<std::string> report;
    registry.accounts.front().label = "Renamed subscription";
    CHECK(restarted.applyAccounts(registry, "account label changed", &report));
    CHECK(restarted.primarySession() == original && store.find(original)->info.account == "work");
    registry.accounts.erase(registry.accounts.begin());
    CHECK(restarted.applyAccounts(registry, "subscription removed", &report));
    CHECK(!restarted.isPrimary(original) && !restarted.state().find(original));
    CHECK(!restarted.apply(numbered(5, act(ActionKind::Send, original, "removed account")), &why, "window"));
    CHECK(store.find(original)->transcript == transcript);
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

// The hub's states share one list of friends: a session's change copies none of it, an applied registry whose friends
// are the same keeps it, and only a change of the friends sets another (Hub::refresh, stateFriends).
void hub_keeps_one_friends_list() {
    MemoryStore store;
    Hub hub(store);
    accounts::Registry r;
    r.accounts = {{"work", "fake"}};
    r.friends = {{"ada", {"work"}, ""}, {"bo", {"work"}, ""}};
    CHECK(hub.useAccounts(r, nullptr).empty());
    withSessions(hub, {"ada", "bo"});
    const auto list = hub.state().friendsList();
    CHECK(hub.state().friendsKnown() && list->size() == 2);
    hub.apply(Action{1, ActionKind::Send, sid("ada"), "", "a", {}});
    hub.apply(Action{2, ActionKind::Queue, sid("bo"), "", "b", {}});
    CHECK(hub.state().friendsList() == list);
    accounts::Registry more = r;
    more.accounts.push_back({"home", "fake"});
    std::vector<std::string> report;
    CHECK(hub.applyAccounts(more, "an account added", &report));
    CHECK(hub.state().friendsList() == list);
    accounts::Registry fewer = more;
    fewer.friends.pop_back();
    CHECK(hub.applyAccounts(fewer, "bo taken out", &report));
    CHECK(hub.state().friendsList() != list && hub.state().friends().size() == 1);
    const auto after = hub.state().friendsList();
    hub.apply(Action{3, ActionKind::Send, sid("ada"), "", "c", {}});
    CHECK(hub.state().friendsList() == after);
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

void conversation_commands() {
    class CatalogResponder : public RecordingResponder {
        std::string catalog(const SessionInfo&) const override {
            return R"({"default":"m0","models":{"m0":["low"],"m1":["high"]}})";
        }
    } responder;
    MemoryStore store;
    Hub hub(store, Limits{}, &responder);
    withSessions(hub, {"ada", "bea"});
    const auto send = [&](const char* command) { return hub.apply(act(ActionKind::Send, 1, command)); };
    CHECK(send("/model"));
    CHECK(store.find(1)->transcript.back()->text.find("m0 (current)") != std::string::npos);
    CHECK(send("/model m1 low") && store.find(1)->info.model != "m1");
    CHECK(send("/model m1 high") && store.find(1)->info.model == "m1" && store.find(1)->info.effort == "high");
    CHECK(send("/info") && store.find(1)->transcript.back()->text.find("Friend: ada") != std::string::npos);
    CHECK(store.find(1)->transcript.back()->text.find("Pronouns:") == std::string::npos);
    CHECK(send("/model m1") && store.find(1)->info.model == "m1");
    CHECK(send("/effort high") && store.find(1)->info.effort == "high");
    CHECK(send("/effort low") && store.find(1)->info.effort == "high");
    CHECK(send("remember me"));
    CHECK(send("/compact") && store.find(1)->driver.summary);
    CHECK(send("/clear"));
    CHECK(store.find(1)->transcript.empty());
    CHECK(hub.state().find(1)->entries.empty());
    CHECK(store.find(1)->info.contextEpoch == 1 && !store.find(1)->driver.summary);
    CHECK(store.find(1)->info.model == "m1" && store.find(1)->info.effort == "high");
    CHECK(store.find(2)->info.contextEpoch == 0 && store.find(2)->transcript.empty());
}

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

// A pending model-specific limit survives a reconnect. A stale or duplicate choice
// must never charge another subscription or launch the interrupted work twice.
void hub_model_limit_choice() {
    for (const bool changeAccount : {true, false}) {
        MemoryStore store;
        DriverState paused;
        paused.nextTurn = 4;
        paused.modelLimitPending = true;
        paused.continuationPrompt = "continue my work";
        paused.exhaustedAccounts = {"one"};
        CHECK(store.commit(1, {infoChange({"ada", "claude", "one", "fable", ""}),
            SessionChange{nullptr, {}, paused}}, {}, 0, nullptr));

        Hub hub(store);
        accounts::Registry registry;
        registry.accounts = {{"one", "claude"}, {"other", "codex"}, {"two", "claude"}};
        registry.friends = {{"ada", {"one", "other", "two"}, "one"}};
        hub.useAccounts(registry, nullptr);
        hub.reportSignedIn("one", true);
        hub.reportSignedIn("other", true);
        hub.reportSignedIn("two", true);

        Sessions mirror;
        mirror.apply_string(hub.state().init_diff());
        const auto& offer = mirror.find(1)->info;
        CHECK(offer.limitRequest == 4 && offer.limitAccount == "two" && offer.limitModel == "opus");

        auto select = act(ActionKind::ResolveLimit, 1, changeAccount ? "account" : "model",
            changeAccount ? offer.limitAccount : offer.limitModel);
        select.account = offer.account;
        select.request = offer.limitRequest;
        Actions actions(1);
        actions.add(select);
        select = Actions::parse(actions.init_diff()).at(0);
        for (const auto kind : {ActionKind::SetAccount, ActionKind::SetModel}) {
            std::string why;
            CHECK(!hub.apply(act(kind, 1, "other"), &why));
            CHECK(why.find("pending model-limit choice") != std::string::npos);
        }
        auto stale = select;
        stale.request--;
        CHECK(!hub.apply(stale));
        stale = select;
        stale.friendName = "arbitrary";
        CHECK(!hub.apply(stale));
        CHECK(store.find(1)->driver == paused);

        if (changeAccount) {
            stale = select;
            stale.friendName = "other";  // a different provider cannot serve this model
            CHECK(!hub.apply(stale));
            hub.reportSignedIn("one", false);
            hub.reportSignedIn("two", false);
            CHECK(hub.state().find(1)->info.limitRequest == 4);
            CHECK(hub.state().find(1)->info.limitAccount.empty() && hub.state().find(1)->info.limitModel.empty());
            CHECK(!hub.apply(select));
            CHECK(store.find(1)->driver == paused);
            hub.reportSignedIn("one", true);
            hub.reportSignedIn("two", true);
        }

        CHECK(hub.apply(select, nullptr, "w"));
        const auto* result = store.find(1);
        CHECK(result->info.harness == "claude");
        CHECK(result->info.model == (changeAccount ? "fable" : "opus"));
        CHECK(result->info.account == (changeAccount ? "two" : "one"));
        CHECK(result->driver.continuationPrompt == paused.continuationPrompt);
        CHECK(result->driver.exhaustedAccounts.empty() == !changeAccount);
        CHECK(!result->driver.modelLimitPending && !hub.state().find(1)->info.limitRequest);
        CHECK(store.applied("w") == select.num);
        Hub restarted(store);
        CHECK(!restarted.apply(select));
        CHECK(store.find(1)->transcript.size() == 1);
    }
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
    // In format 3 the two whole diffs differ in one byte, the activity's: Down is 2. 3 is an activity this build does
    // not know (a later server's): it reads busy, the friend neither idle nor down, and the diff is not refused. (In
    // format 4 an activity a later version adds reads busy too: wire_unit_versions.)
    Sessions a3 = a;
    Sessions b3 = b;
    a3.setFormat(3);
    b3.setFormat(3);
    std::string later = b3.init_diff();
    const std::string good = a3.init_diff();
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
        e.setFormat(3);
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
    const std::string sessions = diff.take();
    // Format 2: what follows the sessions is a later format's, skipped.
    Sessions s;
    s.setFormat(2);
    Writer later;
    later.str("a field a later format adds after the sessions");
    s.apply_string(sessions + later.take());
    // Format 3: the secret requests follow the sessions, each framed, so a field a later format adds to a request, and
    // one it adds after them, are skipped.
    Writer request;
    request.u64(4);
    request.str("work");
    request.str("oauth-token");
    request.u64(1);
    request.str("ada");
    request.str("the login expired");
    request.str("a field a later format adds to a request");
    Writer requests;
    requests.u64(1);
    requests.str(request.take());
    requests.str("a field a later format adds after the requests");
    Sessions three;
    three.setFormat(kSecretsFormat);
    CHECK(kSecretsFormat == 3 && kTablesFormat == 4);
    three.apply_string(sessions + requests.take());
    const std::vector<SecretRequest> asked{SecretRequest{4, "work", "oauth-token", 1, "ada", "the login expired"}};
    CHECK(three.requests() == asked);
    CHECK(three.offers().empty());      // formats 1 to 3 carry no sign-in (sync/formats.h)
    for (const Sessions* read : {&s, &three}) {
        const SessionView* v = read->find(1);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(v->info.friendName == "ada" && v->activity == Activity::Busy);
            CHECK(v->entries.size() == 1 && v->entries[0]->kind == EntryKind::Notice && v->entries[0]->closed);
            CHECK(v->entries[0]->text == "a line of a kind this build does not know");
            CHECK(v->queue == std::vector<std::string>{"queued"});
            CHECK(v->info.instance == "studio");
        }
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
    // Main's bytes are format 2's: a link agrees its format, so they are read in format 2.
    Sessions read;
    read.setFormat(2);
    read.apply_string(mainSessionFrame(nullptr));
    const SessionView* a = read.find(sid("ada"));
    CHECK(a != nullptr && a->info.account == "work" && a->info.instance.empty());
    CHECK(a != nullptr && a->entries.size() == 1 && a->entries[0]->text == "hi");
    // New to old: this build's bytes are main's, the instance appended inside the frame.
    SessionView one = tailOf(sid("ada"), SessionInfo{"ada", "claude", "work", "", "", "studio"},
        {entry(EntryKind::Person, "hi")}, {}, Tail{});
    Sessions b({std::make_shared<const SessionView>(one)});
    const std::string studio = "studio";
    // In format 3 the secret requests follow, none here.
    Writer none;
    none.u64(0);
    b.setFormat(3);
    CHECK(b.init_diff() == mainSessionFrame(&studio) + none.take());
    b.setFormat(2);
    CHECK(b.init_diff() == mainSessionFrame(&studio));
    b.setFormat(kStateFormat);
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

// Each end's Hello against the other's: refused only when the other's proto is below this end's min_peer, in one plain
// line naming which end is too old; every other pairing works, at the older of the two protos. The Hello as text reads
// back as it was, and text that is not a Hello is refused with why.
void hello_min_peer() {
    const Hello now = kHello;
    CHECK(now.proto == kStateFormat && now.minPeer == kMinPeer);
    // A window and a server of any two protos at or above the other's min_peer: no refusal either way.
    for (std::uint32_t w = 1; w <= 6; ++w) {
        for (std::uint32_t s = 1; s <= 6; ++s) {
            const Hello window{w, 1};
            const Hello server{s, 1};
            CHECK(refusalOfWindow(window, server).empty() && refusalOfServer(server, window).empty());
            CHECK(linkFormat(w, s) == (w < s ? w : s));
        }
    }
    // A server that works only with windows from proto 5: a window of 4 is refused by the server, and told to update
    // nexus; one of 5 or 6 is served.
    const Hello strictServer{6, 5};
    const std::string old = refusalOfWindow(Hello{4, 1}, strictServer);
    CHECK(old.find("This window is too old") == 0 && old.find("update nexus.") != std::string::npos);
    CHECK(old.find('\n') == std::string::npos);
    CHECK(refusalOfWindow(Hello{5, 1}, strictServer).empty() && refusalOfWindow(Hello{6, 1}, strictServer).empty());
    // A window that works only with servers from proto 5: a server of 4 is refused, by the window and, when the
    // server hears the window's Hello, by the server too, with the same line, which says to update nexus-server.
    const Hello strictWindow{6, 5};
    const std::string fromWindow = refusalOfServer(Hello{4, 1}, strictWindow);
    const std::string fromServer = refusalOfWindow(strictWindow, Hello{4, 1});
    CHECK(fromWindow.find("nexus-server is too old") == 0 && fromWindow == fromServer);
    CHECK(refusalOfServer(Hello{5, 1}, strictWindow).empty());
    // An end from before Hello: it speaks the format it stated, and works with every older one.
    CHECK(statedFormat(0) == (Hello{1, 1}) && statedFormat(3) == (Hello{3, 1}));
    CHECK(refusalOfWindow(statedFormat(0)).empty() && refusalOfServer(statedFormat(3)).empty());
    // The text form.
    Hello read;
    std::string why;
    CHECK(readHello(helloText(Hello{7, 2}), &read, &why) && read == (Hello{7, 2}));
    for (const std::string& bad : {std::string(), std::string("0"), std::string("zz"), std::string("ABCD"),
                                   std::string("00"), helloText(Hello{0, 1}), std::string(2000, 'a')}) {
        why.clear();
        CHECK(!readHello(bad, &read, &why) && !why.empty() && why.find('\n') == std::string::npos);
    }
}

// Format 4: an entry whose kind this build does not know reads as a notice, never failing the read; here one whose
// writer set no kind (None, as a later version's kind reads; wire_unit_versions sends a real later kind).
void sessions_unknown_kind_is_a_notice() {
    const std::string bytes = synctest::craftedUnsetKindDiff();
    CHECK(!bytes.empty());
    Sessions read;
    read.apply_string(bytes);
    const SessionView* v = read.find(1);
    CHECK(v != nullptr && v->entries.size() == 1);
    if (v != nullptr && v->entries.size() == 1) {
        CHECK(v->entries[0]->kind == EntryKind::Notice && v->entries[0]->text == "a later kind");
        CHECK(v->activity == Activity::Idle);   // absent: its declared default
    }
}

// Format 4 carries text as schema's text, well-formed UTF-8 with no zero byte. Text that is not reaches the window with
// each such byte as U+FFFD, the diff never refused; an open entry of such text grows by whole sends (never an offset
// into bytes the window holds otherwise), and the window's entry keeps up.
void sessions_text_not_utf8() {
    const std::string bad = std::string("ok \xff\x00 and \xc3", 11);
    const std::string shown = "ok \xEF\xBF\xBD\xEF\xBF\xBD and \xEF\xBF\xBD";
    auto state = [](const std::string& text, bool closed, bool queued = true) {
        SessionView v = tailFor("ada", {entry(EntryKind::Friend, text, closed)},
            queued ? std::vector<std::string>{text} : std::vector<std::string>{}, Tail{});
        return Sessions({std::make_shared<const SessionView>(std::move(v))});
    };
    const Sessions a = state(bad, false);
    const Sessions b = state(bad + " more", false);
    const Sessions c = state(bad + " more, done", true);
    Sessions window;
    window.apply_string(a.init_diff());
    window.apply_string(b.diff_from(a));
    window.apply_string(c.diff_from(b));
    const SessionView* v = window.find(sid("ada"));
    CHECK(v != nullptr && v->entries.size() == 1);
    if (v != nullptr && v->entries.size() == 1) {
        CHECK(v->entries[0]->text == shown + " more, done" && v->entries[0]->closed);
        CHECK(v->queue == std::vector<std::string>{shown + " more, done"});
    }
    // Good text still grows by its tail alone.
    const Sessions g1 = state(std::string(3000, 'g'), false, false);
    const Sessions g2 = state(std::string(3000, 'g') + "h", false, false);
    CHECK(g2.diff_from(g1).size() < 400);
    Sessions k = g1;
    k.apply_string(g2.diff_from(g1));
    CHECK(k == g2);
}

// An open entry whose text ends inside a UTF-8 character, then grows to complete it. The window holds the cut text with
// U+FFFD (three bytes where the server's has one), so the growth goes whole; before, it went as a growth from the
// server's length, which the window refused on every diff from that state, and the window froze.
void sessions_growth_across_a_split_character() {
    const auto state = [](const std::string& text, bool closed) {
        SessionView v = tailFor("ada", {entry(EntryKind::Friend, text, closed)}, {}, Tail{});
        return Sessions({std::make_shared<const SessionView>(std::move(v))});
    };
    const Sessions s0;
    const Sessions s1 = state("ab\xC3", false);
    const Sessions s2 = state("ab\xC3\xA9", false);
    const Sessions s3 = state("ab\xC3\xA9 and more", true);
    Sessions window = s0;
    window.apply_string(s1.diff_from(s0));
    const SessionView* held = window.find(sid("ada"));
    CHECK(held != nullptr && held->entries.size() == 1 && held->entries[0]->text == "ab\xEF\xBF\xBD");
    // mosh diffs each newer state from the one the window acknowledged, s1, until the window acknowledges another.
    for (const Sessions* next : {&s2, &s3}) {
        Sessions w = window;
        bool refused = false;
        try {
            w.apply_string(next->diff_from(s1));
        } catch (const BadDiff&) {
            refused = true;
        }
        CHECK(!refused);
        CHECK(!refused && w == *next);
    }
    // And from s2, whose text is whole, the growth goes as its tail alone.
    Sessions w = window;
    w.apply_string(s2.diff_from(s1));
    const std::string d = s3.diff_from(s2);
    w.apply_string(d);
    CHECK(w == s3);
}

bool refusedAsBadDiff(const std::function<void()>& f, const std::string& saying) {
    try {
        f();
    } catch (const BadDiff& e) {
        return std::string(e.what()).find(saying) != std::string::npos;
    }
    return false;
}

// The caps on a format 4 read (sync/codec.h): a diff of up to kMaxDiffBytes reads, one byte more is refused before it
// is measured; a region of up to kMaxReadBytes is allocated, one byte more is refused before it is; and bytes crafted to
// load into far more than they are (many tiny elements) are refused whole, the state left as it was.
void format4_read_caps() {
    const std::string longer = "longer than the link carries";
    const std::string past = "one read allocates";
    CHECK(!refusedAsBadDiff([] { codec::checkDiffSize(codec::kMaxDiffBytes, "diff"); }, longer));
    CHECK(refusedAsBadDiff([] { codec::checkDiffSize(codec::kMaxDiffBytes + 1, "diff"); }, longer));
    CHECK(!refusedAsBadDiff([] {
        codec::checkReadSize(1000, static_cast<std::int64_t>(codec::kMaxReadBytes), "diff");
    }, past));
    CHECK(refusedAsBadDiff([] {
        codec::checkReadSize(1000, static_cast<std::int64_t>(codec::kMaxReadBytes) + 1, "diff");
    }, past));
    CHECK(refusedAsBadDiff([] { codec::checkReadSize(1000, -1, "diff"); }, "does not load"));

    // A real diff of exactly kMaxDiffBytes: one open entry, its text sized to make the diff that long.
    const auto withText = [](std::size_t n) {
        SessionView v = tailFor("ada", {entry(EntryKind::Friend, std::string(n, 't'), false)}, {}, Tail{});
        return Sessions({std::make_shared<const SessionView>(std::move(v))});
    };
    std::size_t n = codec::kMaxDiffBytes - 4096;
    std::string full = withText(n).init_diff();
    for (int tries = 0; tries < 16 && full.size() != codec::kMaxDiffBytes; ++tries) {
        n = n + codec::kMaxDiffBytes - full.size();
        full = withText(n).init_diff();
    }
    CHECK(full.size() == codec::kMaxDiffBytes);
    Sessions got;
    got.apply_string(full);
    CHECK(got == withText(n));
    Sessions before = got;
    CHECK(refusedAsBadDiff([&] { got.apply_string(full + "x"); }, longer));
    CHECK(got == before);
    CHECK(refusedAsBadDiff([&] { (void)Actions::parse(std::string(codec::kMaxDiffBytes + 1, 'a')); }, longer));

    // Crafted: 300,000 sessions the window is told it holds unchanged, each an id alone: 3,600,036 bytes, under
    // kMaxDiffBytes, that would load into 38,400,048 (128 bytes for each 12). Refused, and the window keeps its state.
    codec::SessionsChange many;
    many.sessions.resize(300000);
    for (std::size_t i = 0; i < many.sessions.size(); ++i) {
        many.sessions[i].id = i + 1;
    }
    const std::string crafted = codec::writeTables(many, kStateFormat);
    CHECK(crafted.size() <= codec::kMaxDiffBytes);
    int unknown = 0;
    CHECK(refusedAsBadDiff([&] { (void)codec::readTables(crafted, &unknown); }, past));
    Sessions small = withText(10);
    const Sessions kept = small;
    CHECK(refusedAsBadDiff([&] { small.apply_string(crafted); }, past));
    CHECK(small == kept);

    // The sign-in's field (format 5) under the same caps. A flow that is an id alone is 12 bytes and loads into 48, four
    // times (measured: 300,000 of them, 3,600,036 bytes, load into 14,400,064), so within kMaxDiffBytes the flows
    // stay within kMaxReadBytes: 300,000 read, as any diff under both caps does. 350,000 (4,200,036 bytes) are longer
    // than the link carries: refused before they are measured, and the window keeps its state.
    const auto flowsOf = [](std::size_t count) {
        codec::SessionsChange flows;
        flows.offers.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            flows.offers[i].id = i + 1;
        }
        return codec::writeTables(flows, kSignInFormat);
    };
    const std::string within = flowsOf(300000);
    CHECK(within.size() <= codec::kMaxDiffBytes);
    CHECK(codec::readTables(within, &unknown).offers.size() == 300000);
    const std::string longerFlows = flowsOf(350000);
    CHECK(longerFlows.size() > codec::kMaxDiffBytes);
    CHECK(refusedAsBadDiff([&] { (void)codec::readTables(longerFlows, &unknown); }, longer));
    CHECK(refusedAsBadDiff([&] { small.apply_string(longerFlows); }, longer));
    CHECK(small == kept);
}

// The format 4 read path under damage: fixed seeds, a few thousand mutated copies of real sessions diffs and actions
// diffs (bits flipped, bytes changed, cut short, bytes added and removed, lengths overwritten). Every read either takes
// the bytes or throws BadDiff and nothing else; a refused read leaves the state as it was; and a state or actions it
// takes write back out and read again as the same.
std::string mutated(const std::string& m, Rng& rng) {
    std::string out = m;
    const std::size_t rounds = 1 + rng.below(3);
    for (std::size_t r = 0; r < rounds; ++r) {
        if (out.empty()) {
            out.push_back(static_cast<char>(rng.below(256)));
            continue;
        }
        const std::size_t at = rng.below(out.size());
        switch (rng.below(7)) {
            case 0:
                out[at] = static_cast<char>(out[at] ^ static_cast<char>(1u << rng.below(8)));
                break;
            case 1:
                out[at] = static_cast<char>(rng.below(256));
                break;
            case 2:
                out.resize(at);
                break;
            case 3:
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), static_cast<char>(rng.below(256)));
                break;
            case 4:
                out.erase(at, 1);
                break;
            case 5:
                for (std::size_t k = at; k < out.size() && k < at + 8; ++k) {
                    out[k] = static_cast<char>(0xFF);
                }
                break;
            default:
                out.insert(at, out.substr(rng.below(out.size()), 1 + rng.below(16)));
                break;
        }
    }
    return out;
}

void format4_read_fuzz() {
    int accepted = 0;
    int refused = 0;
    int other = 0;
    // Sessions: pairs of states from random histories, the diff of the later from the earlier, with secret requests in
    // some, and the server's list of friends in every form: none known, known and empty, a list arriving, and a list
    // that changes (a name that is not ASCII, and a long one, among them).
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        Rng rng{seed};
        History h;
        std::vector<Sessions> states{Sessions()};
        for (int i = 0; i < 30; ++i) {
            h.step(rng);
            states.push_back(h.state());
        }
        Sessions to = states.back();
        if (seed % 2 == 0) {
            SecretRequest q;
            q.id = seed;
            q.account = "acct";
            q.what = "a token";
            q.asker = 1;
            q.askerName = "ada";
            q.why = "to log in";
            to.setRequests({q});
        }
        Sessions from = states[states.size() / 2];
        const std::vector<ServerFriend> before = {{"ada"}, {"bo"}};
        const std::vector<ServerFriend> after = {{"bo"}, {"zhi y\xc7\x8e w\xc3\xa9n"}, {std::string(200, 'c')}};
        switch (seed % 4) {
            case 0:
                break;                      // no list on either side, as from a server before it
            case 1:
                to.setFriends(after);       // a list arriving
                break;
            case 2:
                from.setFriends(before);    // a list that changes
                to.setFriends(after);
                break;
            default:
                from.setFriends(before);    // known, then known and empty
                to.setFriends({});
                break;
        }
        // The sign-in flows (format 5): in half the seeds, one seen by every window and one held by this window, its
        // URL as bytes; so the fuzz reaches SessionsDiff.signins, its caps and its bytes field.
        if (seed % 3 != 0) {
            SignInOffer everyone;
            everyone.id = seed;
            everyone.account = "acct";
            everyone.provider = "claude";
            everyone.opener = "*";
            everyone.host = "studio";
            SignInOffer held = everyone;
            held.id = seed + 1;
            held.opener = "claim-test";
            held.url = SecretValue("https://sign-in.test/authorize?state=test-state");
            to.setOffers({everyone, held});
        }
        const std::string diff = to.diff_from(from);
        Sessions check = from;
        check.apply_string(diff);
        CHECK(check == to);
        for (int k = 0; k < 500; ++k) {
            const std::string m = mutated(diff, rng);
            Sessions w = from;
            try {
                w.apply_string(m);
                ++accepted;
                Sessions again;
                again.apply_string(w.init_diff());
                CHECK(again == w);
                Sessions step = from;
                step.apply_string(w.diff_from(from));
                CHECK(step == w);
            } catch (const BadDiff&) {
                ++refused;
                CHECK(w == from);
            } catch (const std::exception& e) {
                ++other;
                std::fprintf(stderr, "format4_read_fuzz: sessions seed %llu case %d threw %s\n",
                    static_cast<unsigned long long>(seed), k, e.what());
            }
        }
    }
    // Actions: held actions of every kind, a secret's answer and a sign-in callback's query among them, as the window
    // sends them.
    for (std::uint64_t seed = 11; seed <= 14; ++seed) {
        Rng rng{seed};
        Actions held(1);
        for (int i = 0; i < 12; ++i) {
            Action a = act(static_cast<ActionKind>(1 + rng.below(static_cast<std::uint64_t>(kLastActionKind))),
                1 + rng.below(3), randomText(rng), kTabNames[rng.below(3)]);
            a.account = rng.chance(2) ? "acct" : "";
            a.request = rng.below(4);
            if (a.kind == ActionKind::SecretAnswer) {
                a.secret = SecretValue("hunter2");
            }
            if (a.kind == ActionKind::SignInCallback) {
                a.secret = SecretValue("code=test-code&state=test-state");
            }
            held.add(std::move(a));
        }
        const std::string diff = held.diff_from(Actions(1));
        for (int k = 0; k < 500; ++k) {
            const std::string m = mutated(diff, rng);
            Actions w(1);
            try {
                const std::vector<Action> got = Actions::parse(m);
                CHECK(!got.empty() || m.empty());   // no bytes is no actions; any bytes hold at least one
                if (!got.empty()) {
                    int unknown = 0;
                    const std::vector<Action> again =
                        codec::readActionsTables(codec::writeActionsTables(got.front().num, got, 0, kStateFormat),
                            kStateFormat, &unknown);
                    CHECK(again == got);
                }
                w.apply_string(m);
                ++accepted;
            } catch (const BadDiff&) {
                ++refused;
                CHECK(w == Actions(1));
            } catch (const std::exception& e) {
                ++other;
                std::fprintf(stderr, "format4_read_fuzz: actions seed %llu case %d threw %s\n",
                    static_cast<unsigned long long>(seed), k, e.what());
            }
        }
    }
    CHECK(other == 0);
    CHECK(accepted > 0 && refused > 0);
}

// A state file says its format and is read in it: this build's round trip; one of format 1 (main's bytes); one of a
// later format read as this build reads a later server's; and refused, in one line, what is not a state file.
void state_files() {
    Sessions busy = mainState("The nightly");
    std::string why;
    Sessions read;
    CHECK(stateFile(busy).rfind("nexus-state " + std::to_string(kStateFormat) + "\n", 0) == 0);
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
    CHECK(mirror.apply(m, stateOf("bo", t, {})));
    CHECK(!mirror.missingHistory(m.tab(1).id) && tr.size() == 10);
    for (unsigned i = 0; i < 10; ++i) CHECK(tr[i].text == std::to_string(i));
}

void mirror_bus_grants_reconcile_cached_history() {
    Model m(friends2());
    Mirror mirror;
    const auto id = m.tab(0).id;
    const auto hidden = entry(EntryKind::Bus, "");
    const auto old = entry(EntryKind::Bus, "old\noutside the newest tail");
    const auto first = entry(EntryKind::Bus, "peer\nfirst");
    const auto second = entry(EntryKind::Bus, "peer\nsecond");
    const auto tool = entry(EntryKind::Tool, "work");
    const auto open = entry(EntryKind::Friend, "stream", false);
    SessionView v = tailFor("ada", {}, {}, {});
    auto apply = [&] { return mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)})); };
    auto texts = [&] {
        std::vector<std::string> result;
        for (const auto& row : m.tab(0).transcript) result.push_back(row.text);
        return result;
    };
    v.first = 2;
    v.entries = {old, entry(EntryKind::Person, "old\nlookalike"), entry(EntryKind::Friend, "reply"), first, second};
    CHECK(apply());  // legacy server, no roster
    m.append(0, Entry{EntryKind::Notice, "local"});
    m.set_draft(0, "held");
    m.hold_draft(0);
    m.set_draft(0, "typing");
    CHECK(mirror.apply(m, Sessions{}));  // disconnect

    v.first = 5;
    v.entries = {hidden, hidden, tool, open};
    v.busSenders = std::vector<std::string>{};
    v.queue = {"queued"};
    CHECK(apply());
    const std::vector<std::string> ordinary{notShownNotice(2), "old\nlookalike", "reply", "local", "work"};
    CHECK(texts() == ordinary);
    CHECK(m.tab(0).transcript.size() == ordinary.size() && m.tab(0).transcript[1].kind == EntryKind::Person &&
          m.tab(0).transcript.back().kind == EntryKind::Tool);
    CHECK(m.tab(0).held == "held" && m.tab(0).draft == "typing" && m.tab(0).queue == v.queue);
    CHECK(mirror.seen(id).next == 8 && mirror.open(id) == open.get());
    const auto generation = m.tab(0).transcriptGeneration;
    CHECK(!apply() && m.tab(0).transcriptGeneration == generation);

    v.busSenders = std::vector<std::string>{"old", "peer"};
    v.entries = {first, second, tool, open};
    CHECK(apply());  // a restored roster also recovers the previously displayed row outside this tail
    const std::vector<std::string> restored{notShownNotice(2), old->text, "old\nlookalike", "reply", first->text,
        second->text, "local", "work"};
    CHECK(texts() == restored);
    CHECK(m.tab(0).transcriptGeneration == generation + 1);
    CHECK(mirror.seen(id).next == 8 && mirror.open(id) == open.get());
    CHECK(!apply());
    v.first = 2;
    v.entries = {old, entry(EntryKind::Person, "old\nlookalike"), entry(EntryKind::Friend, "reply"),
        first, second, tool, open};
    CHECK(!apply());  // receiving the older entry again does not duplicate it
    CHECK(texts() == restored);
    CHECK(mirror.seen(id).next == 8 && mirror.open(id) == open.get());
}

void mirror_skips_masked_bus_slots_and_respects_clear() {
    Model m(friends2());
    Mirror mirror;
    const auto id = m.tab(0).id;
    SessionView v = tailFor("ada", {}, {}, {});
    auto apply = [&] { return mirror.apply(m, Sessions({std::make_shared<const SessionView>(v)})); };
    const auto peer = entry(EntryKind::Bus, "peer\nbody");
    const auto closed = entry(EntryKind::Friend, "closed");
    v.busSenders = std::vector<std::string>{};
    v.entries = {entry(EntryKind::Bus, ""), closed};
    CHECK(apply());
    CHECK(m.tab(0).transcript.size() == 1 && m.tab(0).transcript[0].text == closed->text);
    CHECK(mirror.seen(id).next == 2);
    v.first = 1;
    v.entries = {closed};
    v.busSenders = std::vector<std::string>{"peer"};
    CHECK(!apply());  // no body was ever received for the masked slot
    CHECK(m.tab(0).transcript.size() == 1);
    v.first = 0;
    v.entries = {peer, closed};
    CHECK(apply());  // an expanded tail now supplies its body
    CHECK(m.tab(0).transcript.size() == 2 && m.tab(0).transcript[0].text == peer->text);
    m.clear_transcript(0);
    mirror.clearOpen(id);
    v.busSenders.reset();  // another roster transition after the local clear
    v.entries = {peer, closed, peer};
    CHECK(apply());
    CHECK(m.tab(0).transcript.size() == 1 && m.tab(0).transcript[0].text == peer->text);
    CHECK(mirror.seen(id).next == 3);
    v.busSenders = std::vector<std::string>{};
    CHECK(apply());  // the roster governs even a nonempty bus row
    CHECK(m.tab(0).transcript.empty() && mirror.seen(id).next == 3);
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

// ---- the friends: the server's list over the wire, and the window's tabs from it ----

std::string tabNames(const Model& m) {
    std::string out;
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        const Tab& t = m.tab(i);
        out += (i != 0 ? " " : "") + (t.kind == TabKind::Sprint ? std::string("[sprint]")
            : t.kind == TabKind::Terminal ? std::string("[term]") : t.name);
    }
    return out;
}

std::vector<FriendInfo> listOf(std::initializer_list<const char*> names) {
    std::vector<FriendInfo> out;
    for (const char* n : names) {
        out.push_back(FriendInfo{n, {}});
    }
    return out;
}

// The server's list crosses format 4 whole in each diff that changes it: set, changed, emptied (known, none) and
// unset (a server that holds no friends file). A diff carries it only when the state changed.
void friends_cross_the_wire() {
    Sessions empty;
    Sessions two({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{}))});
    const std::vector<ServerFriend> choices = {
        {"ada", {{"work", "Work", true, "dev+work@example.test"}, {"home"}}}, {"bo"}};
    // The generated format-5 writer at ef5e86c carried the name, label and sign-in state.
    Sessions previous;
    previous.apply_string(fixture("account-choice-before-email.init"));
    const std::vector<ServerFriend> previousChoices = {{"ada", {{"work", "Work", true}}}};
    CHECK(previous.friends() == previousChoices && previous.unknown() == 0);
    two.setFriends(choices);
    Sessions read;
    read.apply_string(two.diff_from(empty));
    CHECK(read == two && read.friendsKnown() && read.unknown() == 0);
    CHECK(read.friends() == choices);
    Sessions one = two;
    one.setFriends({ServerFriend{"bo"}});
    CHECK(!one.diff_from(two).empty());
    read.apply_string(one.diff_from(two));
    CHECK(read == one && (read.friends() == std::vector<ServerFriend>{{"bo"}}));
    Sessions none = one;
    none.setFriends({});
    read.apply_string(none.diff_from(one));
    CHECK(read == none && read.friendsKnown() && read.friends().empty());
    Sessions unset = none;
    unset.clearFriends();
    read.apply_string(unset.diff_from(none));
    CHECK(read == unset && !read.friendsKnown());
    CHECK(two.diff_from(two).empty());
}

// A diff carries the list only when it changed from the state diffed from: a state whose sessions changed and whose
// list did not (the same list, or an equal one set apart) sends no friends, and the reader keeps the list it held.
void friends_sent_only_when_changed() {
    std::vector<ServerFriend> many;
    for (int i = 0; i < 2000; ++i) {
        many.push_back(ServerFriend{"friend-" + std::to_string(i)});
    }
    Sessions before({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{}))});
    before.setFriends(many);
    Sessions read;
    read.apply_string(before.diff_from(Sessions()));
    CHECK(read == before && read.friends().size() == 2000);
    Sessions plain({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{})),
                    std::make_shared<const SessionView>(tailOf(2, infoOf("bo"), {}, {}, Tail{}))});
    plain.setFriends(many);
    Sessions after = plain;              // shares the list with plain
    Sessions apart = plain;
    apart.setFriends(many);              // an equal list, set apart
    Sessions noList({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{}))});
    Sessions noListAfter({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{})),
                          std::make_shared<const SessionView>(tailOf(2, infoOf("bo"), {}, {}, Tail{}))});
    const std::size_t bare = noListAfter.diff_from(noList).size();
    CHECK(after.diff_from(before).size() == bare);
    CHECK(apart.diff_from(before).size() == bare);
    read.apply_string(after.diff_from(before));
    CHECK(read == after && read.friends().size() == 2000 && read.friendsKnown());
}

// The list of friends under the read caps (sync/codec.h): 10,000 friends of 64-byte names, far more than a person sets
// up, make a diff well inside kMaxDiffBytes that reads back whole; and a diff that claims a list too long for the
// bytes that carry it is refused, the window keeping the list it had.
void friends_within_the_caps() {
    std::vector<ServerFriend> many;
    for (int i = 0; i < 10000; ++i) {
        std::string name = "friend-" + std::to_string(i);
        name.resize(64, 'x');
        many.push_back(ServerFriend{name});
    }
    Sessions big;
    big.setFriends(many);
    const std::string diff = big.diff_from(Sessions());
    CHECK(diff.size() < codec::kMaxDiffBytes / 4);
    Sessions read;
    read.apply_string(diff);
    CHECK(read == big && read.friends().size() == 10000);
    // The same bytes cut short: refused, and the state is the one before.
    Sessions small;
    small.setFriends({ServerFriend{"ada"}});
    Sessions held;
    held.apply_string(small.diff_from(Sessions()));
    const Sessions kept = held;
    CHECK(refusedAsBadDiff([&] { held.apply_string(diff.substr(0, diff.size() / 2)); }, ""));
    CHECK(held == kept);
}

// The formats before 4 (a window of an older build) carry no list: the server's diff to such a window is byte for
// byte the one without it, and a state read from it holds none.
void friends_not_in_older_formats() {
    for (std::uint32_t format : {std::uint32_t(1), std::uint32_t(2), kSecretsFormat}) {
        Sessions plain({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{}))});
        plain.setFormat(format);
        Sessions with = plain;
        with.setFriends({ServerFriend{"ada"}});
        Sessions empty;
        empty.setFormat(format);
        CHECK(with.diff_from(empty) == plain.diff_from(empty));
        Sessions read;
        read.setFormat(format);
        read.apply_string(with.diff_from(empty));
        CHECK(!read.friendsKnown() && read.friends().empty() && read.sessions().size() == 1);
    }
}

// The friends a window shows: the server's list when the state has one, in its order, each once; otherwise the friends
// of the state's sessions, in session order, each once, with the model the session names.
void friends_of_a_state() {
    auto view = [](SessionId id, const std::string& name) {
        SessionInfo info = infoOf(name);
        info.model = "m-" + name;
        return std::make_shared<const SessionView>(tailOf(id, info, {}, {}, Tail{}));
    };
    Sessions state({view(1, "bo"), view(2, "ada"), view(3, "bo")});
    std::vector<FriendInfo> f = friendsOf(state);
    CHECK(f.size() == 2 && f[0].name == "bo" && f[0].model == "m-bo" && f[1].name == "ada");
    state.setFriends({ServerFriend{"cy"}, ServerFriend{"ada"}, ServerFriend{"cy"}, ServerFriend{""}});
    f = friendsOf(state);
    CHECK(f.size() == 2 && f[0].name == "cy" && f[1].name == "ada" && f[0].model.empty());
    state.setFriends({});
    CHECK(friendsOf(state).empty());
}

// The strip follows the server's list: each friend gets a tab at the end of the friends' zone, in the saved order then
// the list's; a friend that leaves the list loses its tab, with what it held; the tabs standing keep their order.
void apply_friends_follows_the_list() {
    Model m({}, true);
    m.add_terminal();
    FriendChanges c = applyFriends(m, listOf({"ada", "bo", "cy"}), true, {"cy", "gone", "ada"});
    CHECK(tabNames(m) == "[sprint] cy ada bo [term]");
    CHECK((c.added == std::vector<std::string>{"cy", "ada", "bo"}) && c.removed.empty());
    CHECK(m.tab(m.active()).kind == TabKind::Terminal);     // the active tab stays active
    // The same list again changes nothing.
    c = applyFriends(m, listOf({"ada", "bo", "cy"}), true, {});
    CHECK(c.added.empty() && c.removed.empty() && tabNames(m) == "[sprint] cy ada bo [term]");
    // The person's order is kept; a new friend goes at the end of the friends.
    CHECK(m.move_tab(3, 1));
    c = applyFriends(m, listOf({"ada", "bo", "cy", "di"}), true, {"di"});
    CHECK(tabNames(m) == "[sprint] bo cy ada di [term]" && (c.added == std::vector<std::string>{"di"}));
    // ada leaves: her tab goes, with its transcript and draft, and its id is reported.
    const std::size_t adaId = m.tab(3).id;
    CHECK(m.append(3, Entry{EntryKind::Friend, "said"}) && m.set_draft(3, "unsent"));
    CHECK(m.select_tab(4) && m.select_tab(3));
    c = applyFriends(m, listOf({"bo", "cy", "di"}), true, {});
    CHECK(tabNames(m) == "[sprint] bo cy di [term]");
    CHECK(c.added.empty() && (c.removed == std::vector<std::size_t>{adaId}));
    CHECK(m.tab(m.active()).name == "di");     // she was active: the tab selected before her shows
    // She comes back: a new tab, empty, with a new id.
    c = applyFriends(m, listOf({"bo", "cy", "di", "ada"}), true, {});
    CHECK(tabNames(m) == "[sprint] bo cy di ada [term]" && m.tab(4).id != adaId);
    CHECK(m.tab(4).transcript.empty() && m.tab(4).draft.empty());
    // An empty list empties the friends' zone.
    c = applyFriends(m, {}, true, {});
    CHECK(tabNames(m) == "[sprint] [term]" && c.removed.size() == 4);
}

// From an older server (no list), the friends of its sessions are added and none is removed: a friend whose sessions
// are gone keeps its tab, as the window always did.
void apply_friends_without_a_list_only_adds() {
    Model m({});
    FriendChanges c = applyFriends(m, listOf({"ada"}), false, {});
    CHECK(tabNames(m) == "ada" && m.active() == 0 && c.added.size() == 1);
    c = applyFriends(m, listOf({"bo"}), false, {});
    CHECK(tabNames(m) == "ada bo" && c.removed.empty());
    c = applyFriends(m, {}, false, {});
    CHECK(tabNames(m) == "ada bo" && c.added.empty() && c.removed.empty());
}

// A state holding the server's list `names`.
Sessions listState(std::initializer_list<const char*> names) {
    Sessions s;
    std::vector<ServerFriend> list;
    for (const char* n : names) {
        list.push_back(ServerFriend{n});
    }
    s.setFriends(std::move(list));
    return s;
}

// Model::add_friends and Model::remove_friends are the loops of add_friend and remove_friend (last first) in one pass:
// over random strips, selections and histories, the same tabs, ids, active tab and history.
void batch_friends_are_the_loops() {
    Rng rng{20261115};
    for (int round = 0; round < 3000; ++round) {
        Model a({}, rng.chance(2));
        const int steps = static_cast<int>(rng.below(12));
        for (int s = 0; s < steps; ++s) {
            switch (rng.below(4)) {
                case 0: a.add_friend(FriendInfo{"f" + std::to_string(rng.below(8)), {}}); break;
                case 1: a.add_terminal(); break;
                case 2: if (a.tab_count() != 0) { a.select_tab(rng.below(a.tab_count())); } break;
                default: if (a.tab_count() != 0) { a.remove_friend(rng.below(a.tab_count())); } break;
            }
        }
        Model b = a;
        const auto same = [](const Model& x, const Model& y) {
            if (x.tab_count() != y.tab_count() || x.active() != y.active() ||
                    x.selection_history() != y.selection_history()) {
                return false;
            }
            for (std::size_t i = 0; i < x.tab_count(); ++i) {
                if (x.tab(i).id != y.tab(i).id || x.tab(i).name != y.tab(i).name) {
                    return false;
                }
            }
            return true;
        };
        // Remove a random set: the loop, last first, against the one pass.
        std::vector<char> gone(64, 0);
        for (char& g : gone) {
            g = rng.chance(2) ? 1 : 0;
        }
        std::vector<std::size_t> loopRemoved;
        for (std::size_t i = a.tab_count(); i-- > 0;) {
            if (a.tab(i).kind == TabKind::Friend && gone[a.tab(i).id % gone.size()] != 0) {
                loopRemoved.insert(loopRemoved.begin(), a.tab(i).id);
                a.remove_friend(i);
            }
        }
        const std::vector<std::size_t> passRemoved = b.remove_friends(
            [&gone](const Tab& t) { return gone[t.id % gone.size()] != 0; });
        CHECK(same(a, b) && loopRemoved == passRemoved);
        // Add a random list, names repeated and empty among them.
        std::vector<FriendInfo> more;
        const std::size_t count = rng.below(6);
        for (std::size_t k = 0; k < count; ++k) {
            more.push_back(FriendInfo{rng.chance(6) ? std::string() : "f" + std::to_string(rng.below(10)), {}});
        }
        std::vector<std::string> loopAdded;
        for (const FriendInfo& f : more) {
            if (a.add_friend(f)) {
                loopAdded.push_back(f.name);
            }
        }
        const std::vector<std::string> passAdded = b.add_friends(more);
        CHECK(same(a, b) && loopAdded == passAdded);
    }
}

// A friend's name the window shows is the server's rule for one: a name that breaks it gets no tab and is reported
// once, the rest of the list kept in order.
void friend_names_are_checked() {
    CHECK(friendNameShown("ada") && friendNameShown("a.b-c_9") && friendNameShown(std::string(64, 'x')));
    CHECK(!friendNameShown("") && !friendNameShown(std::string(65, 'x')) && !friendNameShown("a\nb"));
    CHECK(!friendNameShown("tab\there") && !friendNameShown(".hidden") && !friendNameShown("-dash") &&
          !friendNameShown("two words") && !friendNameShown("ünï"));
    Sessions s = listState({"ada", "bad\nname", "bo", "bad\nname", "", "cy"});
    std::vector<std::string> dropped;
    const std::vector<FriendInfo> f = friendsOf(s, &dropped);
    CHECK(f.size() == 3 && f[0].name == "ada" && f[1].name == "bo" && f[2].name == "cy");
    CHECK(dropped.size() == 2 && dropped[0] == "bad\nname" && dropped[1].empty());
    Sessions old({std::make_shared<const SessionView>(tailOf(1, infoOf("ok"), {}, {}, Tail{})),
                  std::make_shared<const SessionView>(tailOf(2, infoOf("no\x01"), {}, {}, Tail{}))});
    dropped.clear();
    CHECK(friendsOf(old, &dropped).size() == 1 && dropped.size() == 1);
    // The strip takes the same rule: no tab for the bad name, and the take reports it.
    Model m({});
    FriendStrip strip;
    const StripTake t = strip.take(m, s);
    CHECK(tabNames(m) == "ada bo cy" && t.dropped.size() == 2 && t.past == 0);
    // Past kMaxFriendTabs (the most a friends.json holds) the rest have no tab, and are counted; a repeat counts once.
    Sessions many;
    std::vector<ServerFriend> list;
    for (std::size_t i = 0; i < kMaxFriendTabs + 44; ++i) {
        list.push_back(ServerFriend{"f" + std::to_string(i)});
    }
    list.push_back(ServerFriend{"f299"});
    many.setFriends(list);
    std::size_t past = 0;
    const std::vector<FriendInfo> shown = friendsOf(many, nullptr, &past);
    CHECK(shown.size() == kMaxFriendTabs && past == 44 && shown.back().name == "f255");
    Model big({});
    FriendStrip bigStrip;
    CHECK(bigStrip.take(big, many).past == 44 && big.tab_count() == kMaxFriendTabs);
}

// What the person typed to a friend the server's list leaves out is kept by name and given back with a new tab for a
// friend of that name: one friend removed and back; every friend gone (an empty list) and the list back; an empty
// draft is not kept. Nothing kept is lost: a kept draft for a friend that never returns stays.
void drafts_outlive_their_tabs() {
    Model m({});
    FriendStrip strip;
    strip.take(m, listState({"ada", "bo", "cy"}));
    CHECK(m.set_draft(0, "to ada") && m.set_draft(1, "to bo"));
    StripTake t = strip.take(m, listState({"ada", "cy"}));
    CHECK(tabNames(m) == "ada cy" && t.keptChanged && strip.kept().size() == 1 && strip.kept().at("bo") == "to bo");
    t = strip.take(m, listState({"ada", "bo", "cy"}));
    CHECK(tabNames(m) == "ada cy bo" && t.keptChanged && strip.kept().empty());
    CHECK(m.tab(2).draft == "to bo" && m.tab(0).draft == "to ada");
    // Every friend gone, then back: every draft back, the empty one not kept.
    t = strip.take(m, listState({}));
    CHECK(tabNames(m).empty() && t.keptChanged && strip.kept().size() == 2);
    t = strip.take(m, listState({"cy", "bo", "ada"}));
    CHECK(tabNames(m) == "cy bo ada" && strip.kept().empty());
    CHECK(m.tab(1).draft == "to bo" && m.tab(2).draft == "to ada" && m.tab(0).draft.empty());
    // A friend that never comes back keeps its draft, whatever else comes and goes.
    m.set_draft(0, "to cy");
    strip.take(m, listState({"bo", "ada"}));
    strip.take(m, listState({"bo"}));
    strip.take(m, listState({"bo", "dee"}));
    CHECK(strip.kept().size() == 2 && strip.kept().at("cy") == "to cy" && strip.kept().at("ada") == "to ada");
    // Drafts kept from before (a launch) are given back as their friends arrive.
    Model again({});
    FriendStrip relaunched({}, std::string(), strip.kept());
    relaunched.take(again, listState({"cy"}));
    CHECK(again.tab(0).draft == "to cy" && relaunched.kept().size() == 1);
}

// A state whose friends are the ones last taken costs nothing: the same list (by pointer) or, from an older server,
// the same sessions' friends, takes no step. A new list with the same names runs and changes nothing.
void strip_takes_only_a_change() {
    Model m({});
    FriendStrip strip;
    Sessions s = listState({"ada", "bo"});
    CHECK(strip.take(m, s).ran);
    Sessions copy = s;
    CHECK(!strip.take(m, s).ran && !strip.take(m, copy).ran);
    Sessions equal = listState({"ada", "bo"});
    const StripTake t = strip.take(m, equal);
    CHECK(t.ran && t.added.empty() && t.removed.empty() && tabNames(m) == "ada bo");
    Sessions unknown;
    CHECK(strip.take(m, unknown).ran && !strip.take(m, unknown).ran);
    Sessions older({std::make_shared<const SessionView>(tailOf(1, infoOf("cy"), {}, {}, Tail{}))});
    CHECK(strip.take(m, older).ran && tabNames(m) == "ada bo cy");
    Sessions grown({std::make_shared<const SessionView>(tailOf(1, infoOf("cy"), {}, {}, Tail{}))});
    CHECK(!strip.take(m, grown).ran);
}

void primary_chat_follows_its_session() {
    Model model({});
    FriendStrip strip;
    Mirror mirror;
    std::vector<Action> sent;
    Sessions received;
    const auto deliver = [&](const Sessions& server) {
        received.apply_string(server.diff_from(received));
        takeFriends(strip, model, mirror, received, [&](Action a) { sent.push_back(std::move(a)); });
        mirror.apply(model, received);
    };
    const Sessions empty = listState({});
    deliver(empty);
    const auto roster = received.friendsList();
    CHECK(model.tab_count() == 0 && sent.empty());

    const std::string primary(kPrimaryChat);
    const auto hostSession = std::make_shared<const SessionView>(
        tailOf(7, infoOf(primary), {entry(EntryKind::Person, "Hello")}, {}, Tail{}));
    Sessions bound({hostSession});
    bound.shareFriends(empty);
    deliver(bound);
    CHECK(received.friendsKnown() && received.friends().empty() && received.friendsList() == roster);
    CHECK(sent.empty() && friendsToEnsure(model).empty());
    CHECK(model.tab_count() == 1);
    if (model.tab_count() != 1) return;
    const auto tabId = model.tab(0).id;
    CHECK(model.tab(0).name == primary && model.tab(0).displayName() == "Chat");
    CHECK(sessionOf(model, 0, mirror) == 7 && model.tab(0).transcript.size() == 1 &&
          model.tab(0).transcript.front().text == "Hello");

    const auto namedSession = std::make_shared<const SessionView>(
        tailOf(8, infoOf("fern"), {entry(EntryKind::Friend, "Named reply")}, {}, Tail{}));
    Sessions both({hostSession, namedSession});
    both.setFriends({{"fern"}});
    deliver(both);
    CHECK(model.tab_count() == 2);
    if (model.tab_count() != 2) return;
    const std::size_t chat = model.tab(0).name == primary ? 0 : 1;
    const std::size_t named = 1 - chat;
    CHECK(model.tab(chat).id == tabId && sessionOf(model, chat, mirror) == 7);
    CHECK(model.tab(named).name == "fern" && sessionOf(model, named, mirror) == 8);
    CHECK(model.tab(named).transcript.size() == 1 && model.tab(named).transcript.front().text == "Named reply");
    CHECK(sent.size() == 1 && sent.front().kind == ActionKind::Ensure && sent.front().friendName == "fern");

    Model restored({}, true);
    FriendStrip saved({}, "fern", {});
    saved.take(restored, received);
    CHECK(restored.tab(restored.active()).name == "fern" && !saved.pending());

    Sessions unbound({namedSession});
    unbound.shareFriends(both);
    deliver(unbound);
    CHECK(model.tab_count() == 1 && model.tab(0).name == "fern" && sessionOf(model, 0, mirror) == 8);
    CHECK(mirror.session(tabId) == 0 && sent.size() == 1);

    Model fresh({}, true);
    FriendStrip defaultStrip;
    defaultStrip.take(fresh, unbound);
    defaultStrip.take(fresh, both);
    CHECK(fresh.tab(fresh.active()).name == primary);

    Model chosen({}, true);
    FriendStrip explicitStrip;
    explicitStrip.take(chosen, unbound);
    CHECK(chosen.tab_count() == 2);
    if (chosen.tab_count() != 2) return;
    CHECK(chosen.select_tab(chosen.tab(0).name == "fern" ? 0 : 1));
    explicitStrip.personSelected();
    explicitStrip.take(chosen, both);
    CHECK(chosen.tab(chosen.active()).name == "fern");
}

void replacement_primary_starts_with_its_own_transcript() {
    const std::string primary(kPrimaryChat);
    const auto first = std::make_shared<const SessionView>(tailOf(14, infoOf(primary),
        {entry(EntryKind::Person, "Old question"), entry(EntryKind::Friend, "Old reply")},
        {"Old queue"}, Tail{}, entry(EntryKind::Friend, "Old stream", false)));
    Sessions before({first});
    before.setFriends({});
    Model model({});
    FriendStrip strip;
    Mirror mirror;
    takeFriends(strip, model, mirror, before, {});
    mirror.apply(model, before);
    CHECK(model.tab_count() == 1);
    if (model.tab_count() != 1) return;
    const auto original = model.tab(0).id;
    CHECK(model.set_draft(0, "For the old session"));
    CHECK(mirror.seen(original).next == 2 && mirror.open(original) != nullptr);
    CHECK(model.tab(0).queue.size() == 1);

    const auto second = std::make_shared<const SessionView>(tailOf(15, infoOf(primary),
        {entry(EntryKind::Friend, "New reply")}, {}, Tail{}));
    Sessions after({second});
    after.shareFriends(before);
    before.apply_string(after.diff_from(before));
    takeFriends(strip, model, mirror, before, {});
    mirror.apply(model, before);
    CHECK(model.tab_count() == 1);
    if (model.tab_count() != 1) return;
    const auto& chat = model.tab(0);
    CHECK(chat.displayName() == "Chat" && mirror.session(chat.id) == 15);
    CHECK(chat.transcript.size() == 1 && chat.transcript.front().text == "New reply");
    CHECK(chat.draft.empty() && chat.queue.empty());
    CHECK(mirror.seen(chat.id).next == 1 && mirror.open(chat.id) == nullptr);
}

// The tab saved as selected waits for its friend: selected when the friend's tab arrives, unless the person chose a
// tab first; a list from the server that does not name it ends the wait, the selection as it was.
void saved_tab_waits_for_its_friend() {
    Model m({}, true);
    FriendStrip strip({"bo", "ada"}, "ada", {});
    CHECK(strip.pending());
    strip.take(m, Sessions());                                  // a first state with nothing in it
    CHECK(strip.pending() && m.active() == 0);
    strip.take(m, listState({"ada", "bo"}));
    CHECK(!strip.pending() && tabNames(m) == "[sprint] bo ada" && m.active() == 2);
    // The person chose first: the saved tab is not selected.
    Model n({}, true);
    FriendStrip chose({}, "ada", {});
    chose.personSelected();
    chose.take(n, listState({"ada"}));
    CHECK(n.active() == 0 && !chose.pending());
    // The server's list without the saved friend: the wait ends, the selection stays.
    Model o({}, true);
    FriendStrip absent({}, "zed", {});
    absent.take(o, listState({"ada"}));
    CHECK(!absent.pending() && o.active() == 0 && absent.taken());
    // An older server's sessions without it: the wait goes on, since its friend may yet have a session.
    Model p({}, true);
    FriendStrip waiting({}, "zed", {});
    waiting.take(p, Sessions({std::make_shared<const SessionView>(tailOf(1, infoOf("ada"), {}, {}, Tail{}))}));
    CHECK(waiting.pending() && p.active() == 0);
}

// `nexus --server H --tab N` past the tabs at launch waits for the server's friends, bounded, before it judges N.
void tab_given_waits_for_the_friends() {
    CHECK(tabWaitsForFriends(true, true, 3, 1));        // tab 3 of the sprint tab alone: wait
    CHECK(!tabWaitsForFriends(true, true, 1, 1));       // there already
    CHECK(!tabWaitsForFriends(true, false, 3, 1));      // no server: no friends will come, judged at once
    CHECK(!tabWaitsForFriends(false, true, 3, 1));      // no --tab
    Model m({}, true);
    FriendStrip strip;
    CHECK(tabWaitGoesOn(strip, false, 1000, 21000));    // nothing heard yet
    strip.take(m, Sessions());                          // a state with no list and no session: still waiting
    CHECK(tabWaitGoesOn(strip, false, 1500, 21000));
    CHECK(!tabWaitGoesOn(strip, true, 1500, 21000));    // the link failed: judged now
    CHECK(!tabWaitGoesOn(strip, false, 21000, 21000));  // out of time: judged now
    strip.take(m, listState({"ada", "bo"}));            // the friends came: tab 3 is bo's
    CHECK(!tabWaitGoesOn(strip, false, 2000, 21000));
    CHECK(m.select_tab(2) && m.tab(2).name == "bo");
}

// A removed friend's tab is forgotten by the mirror: nothing of its session is held for its id.
void mirror_forgets_a_removed_tab() {
    Model m(friends2(), false);
    Mirror mirror;
    const std::size_t ada = m.tab(0).id;
    CHECK(mirror.apply(m, stateOf("ada", {entry(EntryKind::Person, "x")}, {})));
    CHECK(mirror.session(ada) != 0 && mirror.seen(ada).next == 1);
    mirror.forget(ada);
    CHECK(mirror.session(ada) == 0 && mirror.seen(ada).next == 0 && mirror.activity(ada) == nullptr);
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

// Once ServerSession::shutdown closes its remote Actions state, every copy mosh holds refuses new client diffs so
// mosh's shutdown loop never advances ack_num past the last store flush.
void actions_close_refuses_new_diffs() {
    Actions initial;
    initial.trackClose();
    Actions copy = initial;
    Actions sent;
    sent.add(act(ActionKind::Send, sid("ada"), "flushed"));
    copy.apply_string(sent.init_diff());
    CHECK(copy.next() == 2);

    const Actions before = copy;
    sent.add(act(ActionKind::Send, sid("ada"), "after-stop"));
    const std::string diff = sent.diff_from(before);
    initial.close();
    Actions next = copy;
    bool threw = false;
    try {
        next.apply_string(diff);
    } catch (const BadDiff& e) {
        threw = std::string(e.what()).find("shutting down") != std::string::npos;
    }
    CHECK(threw);
    CHECK(next == before);
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
    {"mirror_bus_grants_reconcile_cached_history", mirror_bus_grants_reconcile_cached_history},
    {"mirror_skips_masked_bus_slots_and_respects_clear", mirror_skips_masked_bus_slots_and_respects_clear},
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
    {"conversation_commands", conversation_commands},
    {"hub_model_limit_choice", hub_model_limit_choice},
    {"hub_stop_reaches_the_responder_once", hub_stop_reaches_the_responder_once},
    {"hub_stop_without_responder_changes_nothing", hub_stop_without_responder_changes_nothing},
    {"hub_queue_goes_to_the_responder", hub_queue_goes_to_the_responder},
    {"memory_store_take_refusals", memory_store_take_refusals},
    {"hub_open_entry", hub_open_entry},
    {"sessions_tail_carries_open_apart", sessions_tail_carries_open_apart},
    {"mirror_holds_open_entry_apart", mirror_holds_open_entry_apart},
    {"mirror_queue_follows_the_servers", mirror_queue_follows_the_servers},
    {"ensure_each_friend", ensure_each_friend},
    {"friends_cross_the_wire", friends_cross_the_wire},
    {"friends_sent_only_when_changed", friends_sent_only_when_changed},
    {"hub_keeps_one_friends_list", hub_keeps_one_friends_list},
    {"hub_primary_requires_an_installer_binding", hub_primary_requires_an_installer_binding},
    {"hub_primary_session", hub_primary_session},
    {"friends_within_the_caps", friends_within_the_caps},
    {"friends_not_in_older_formats", friends_not_in_older_formats},
    {"friends_of_a_state", friends_of_a_state},
    {"apply_friends_follows_the_list", apply_friends_follows_the_list},
    {"apply_friends_without_a_list_only_adds", apply_friends_without_a_list_only_adds},
    {"batch_friends_are_the_loops", batch_friends_are_the_loops},
    {"friend_names_are_checked", friend_names_are_checked},
    {"drafts_outlive_their_tabs", drafts_outlive_their_tabs},
    {"strip_takes_only_a_change", strip_takes_only_a_change},
    {"primary_chat_follows_its_session", primary_chat_follows_its_session},
    {"replacement_primary_starts_with_its_own_transcript", replacement_primary_starts_with_its_own_transcript},
    {"saved_tab_waits_for_its_friend", saved_tab_waits_for_its_friend},
    {"tab_given_waits_for_the_friends", tab_given_waits_for_the_friends},
    {"mirror_forgets_a_removed_tab", mirror_forgets_a_removed_tab},
    {"stop_target_rules", stop_target_rules},
    {"stop_never_filtered_by_the_window", stop_never_filtered_by_the_window},
    {"sessions_activity_and_tool_lines", sessions_activity_and_tool_lines},
    {"hub_activity", hub_activity},
    {"mirror_activity", mirror_activity},
    {"mirror_drops_open_with_the_session", mirror_drops_open_with_the_session},
    {"state_format_1_from_main", state_format_1_from_main},
    {"state_format_2_reads_later_formats", state_format_2_reads_later_formats},
    {"link_formats_agree", link_formats_agree},
    {"hello_min_peer", hello_min_peer},
    {"sessions_unknown_kind_is_a_notice", sessions_unknown_kind_is_a_notice},
    {"sessions_text_not_utf8", sessions_text_not_utf8},
    {"sessions_growth_across_a_split_character", sessions_growth_across_a_split_character},
    {"format4_read_caps", format4_read_caps},
    {"format4_read_fuzz", format4_read_fuzz},
    {"state_files", state_files},
    {"cursor_from_the_servers_state", cursor_from_the_servers_state},
    {"actions_close_refuses_new_diffs", actions_close_refuses_new_diffs},
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
