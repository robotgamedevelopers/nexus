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
#include "sync/tabs.h"
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
        a.add(rng.chance(3) ? ActionKind::Queue : ActionKind::Send, kTabNames[rng.below(3)], randomText(rng));
    return a;
}

void actions_numbered_from_one() {
    Actions a;
    CHECK(a.next() == 1);
    CHECK(a.add(ActionKind::Send, "ada", "hi") == 1);
    CHECK(a.add(ActionKind::Queue, "bo", "later") == 2);
    CHECK(a.next() == 3);
    CHECK(a.held().size() == 2);
    CHECK(a.held()[1].num == 2 && a.held()[1].kind == ActionKind::Queue && a.held()[1].tab == "bo");
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
            b.add(rng.chance(3) ? ActionKind::Queue : ActionKind::Send, kTabNames[rng.below(3)], randomText(rng));
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
    older.add(ActionKind::Send, "ada", "x");
    const std::string first = Actions().diff_from(Actions());
    CHECK(first.empty());
    // a diff of actions 1..2 on a state that holds 1..5: nothing changes
    Actions two;
    two.add(ActionKind::Send, "ada", "a");
    two.add(ActionKind::Send, "ada", "b");
    Actions copy = a;
    copy.apply_string(two.init_diff());
    CHECK(copy == a);
}

void actions_gap_refused() {
    Actions b;
    for (int i = 0; i < 4; ++i)
        b.add(ActionKind::Send, "ada", std::to_string(i));
    Actions two;
    two.add(ActionKind::Send, "ada", "0");
    two.add(ActionKind::Send, "ada", "1");
    const std::string tail = b.diff_from(two);   // actions 3 and 4
    Actions one;
    one.add(ActionKind::Send, "ada", "0");
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
        a.add(ActionKind::Send, "ada", std::to_string(i));
    Actions p;
    p.add(ActionKind::Send, "ada", "0");
    p.add(ActionKind::Send, "ada", "1");
    a.subtract(&p);
    CHECK(a.first() == 3 && a.next() == 6 && a.held().size() == 3 && a.held()[0].num == 3);
    a.subtract(&p);   // again: nothing more
    CHECK(a.first() == 3 && a.held().size() == 3);
    a.subtract(&a);   // itself: empty, numbers kept
    CHECK(a.first() == 6 && a.next() == 6 && a.held().empty());
    CHECK(a.add(ActionKind::Queue, "bo", "x") == 6);
}

void actions_diff_from_misuse_throws() {
    Actions a;
    for (int i = 0; i < 3; ++i)
        a.add(ActionKind::Send, "ada", "x");
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
        a.add(ActionKind::Send, "ada", "keep");
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
    w.str("ada");
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
            v.push_back(Action{n, ActionKind::Send, "ada", std::to_string(n)});
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
                    const std::uint64_t n = client.current.add(ActionKind::Send, kTabNames[rng.below(3)], randomText(rng));
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

// ---- down: Tabs ----

EntryRef entry(EntryKind kind, std::string text, bool closed = true) {
    return std::make_shared<const TabEntry>(TabEntry{kind, std::move(text), closed});
}

// A random server history: each step appends, grows the open entry, closes it, queues or makes a tab.
struct History {
    std::map<std::string, std::vector<EntryRef>> transcripts;
    std::map<std::string, std::vector<std::string>> queues;
    Tail tail{3, 400};
    Tabs state() const {
        std::vector<TabRef> tabs;
        for (const auto& [name, t] : transcripts)
            tabs.push_back(std::make_shared<const TabView>(tailOf(name, t, queues.at(name), tail)));
        return Tabs(std::move(tabs));
    }
    void step(Rng& rng) {
        const std::string name = kTabNames[rng.below(3)];
        auto& t = transcripts[name];
        queues[name];
        const std::size_t what = rng.below(5);
        if (what == 0 && !t.empty() && !t.back()->closed) {
            t.back() = entry(t.back()->kind, t.back()->text + randomText(rng), false);
        } else if (what == 1 && !t.empty() && !t.back()->closed) {
            t.back() = entry(t.back()->kind, t.back()->text, true);
        } else if (what == 2) {
            queues[name].push_back(randomText(rng));
        } else {
            if (!t.empty() && !t.back()->closed)
                t.back() = entry(t.back()->kind, t.back()->text, true);
            t.push_back(entry(static_cast<EntryKind>(rng.below(3)), randomText(rng), rng.chance(3) == false));
        }
    }
};

void tabs_diff_apply_random_pairs() {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        Rng rng{seed};
        History h;
        std::vector<Tabs> states{Tabs()};
        const std::size_t steps = rng.below(40);
        for (std::size_t i = 0; i < steps; ++i) {
            h.step(rng);
            states.push_back(h.state());
        }
        // every pair, in both orders: mosh only diffs newer from older, but the diff holds for any two
        for (int k = 0; k < 20; ++k) {
            const Tabs& a = states[rng.below(states.size())];
            const Tabs& b = states[rng.below(states.size())];
            Tabs c = a;
            const std::string d = b.diff_from(a);
            c.apply_string(d);
            CHECK(c == b);
            CHECK(d.empty() == (a == b));
            CHECK(!b.compare(c));
            Tabs e;
            e.apply_string(b.init_diff());
            CHECK(e == b);
        }
    }
}

void tabs_open_entry_sends_only_growth() {
    std::vector<EntryRef> t = {entry(EntryKind::Person, "hi"), entry(EntryKind::Friend, std::string(2000, 'a'), false)};
    const Tabs a({std::make_shared<const TabView>(tailOf("ada", t, {}, Tail{}))});
    t.back() = entry(EntryKind::Friend, std::string(2000, 'a') + "bcd", false);
    const Tabs b({std::make_shared<const TabView>(tailOf("ada", t, {}, Tail{}))});
    const std::string d = b.diff_from(a);
    CHECK(d.size() < 40);
    Tabs c = a;
    c.apply_string(d);
    CHECK(c == b);
    // and a diff that claims growth from an entry this state does not hold open is refused
    Tabs closed({std::make_shared<const TabView>(tailOf("ada",
        {entry(EntryKind::Person, "hi"), entry(EntryKind::Friend, std::string(2000, 'a'), true)}, {}, Tail{}))});
    const Tabs before = closed;
    bool threw = false;
    try {
        closed.apply_string(d);
    } catch (const BadDiff&) {
        threw = true;
    }
    CHECK(threw && closed == before);
}

void tabs_diff_bounded_by_tail() {
    std::vector<EntryRef> t;
    for (int i = 0; i < 10000; ++i)
        t.push_back(entry(i % 2 ? EntryKind::Friend : EntryKind::Person, "entry " + std::to_string(i) + " " +
            std::string(100, 'x')));
    const Tail tail;   // 16 entries, 16 KiB
    const Tabs b({std::make_shared<const TabView>(tailOf("ada", t, {}, tail))});
    CHECK(b.find("ada")->first == 10000 - 16);
    CHECK(b.find("ada")->total() == 10000);
    const std::string fromEmpty = b.init_diff();
    CHECK(fromEmpty.size() < 16 * 120 + 100);
    // far behind: the tail, not the history
    std::vector<EntryRef> old(t.begin(), t.begin() + 10);
    const Tabs a({std::make_shared<const TabView>(tailOf("ada", old, {}, tail))});
    CHECK(b.diff_from(a).size() < 16 * 120 + 100);
    // the byte budget: entries of 6000 bytes, at most 16 KiB, so two; the newest whole even when larger
    std::vector<EntryRef> big = {entry(EntryKind::Friend, std::string(6000, 'a')),
        entry(EntryKind::Friend, std::string(6000, 'b')), entry(EntryKind::Friend, std::string(6000, 'c'))};
    CHECK(tailOf("ada", big, {}, tail).entries.size() == 2);
    std::vector<EntryRef> huge = {entry(EntryKind::Friend, "x"), entry(EntryKind::Friend, std::string(40000, 'h'))};
    CHECK(tailOf("ada", huge, {}, tail).entries.size() == 1);
    CHECK(tailOf("ada", {}, {}, tail).entries.empty());
    CHECK(tailOf("ada", {}, {}, tail).first == 0);
}

void tabs_copy_is_cheap_and_shared() {
    std::vector<EntryRef> t = {entry(EntryKind::Person, "hi")};
    const Tabs a({std::make_shared<const TabView>(tailOf("ada", t, {"q"}, Tail{}))});
    const Tabs b = a;
    CHECK(&a.tabs() == &b.tabs());   // one shared list
    CHECK(a == b);
    CHECK(b.diff_from(a).empty());
}

void tabs_removed_tab() {
    const Tabs a({std::make_shared<const TabView>(tailOf("ada", {entry(EntryKind::Person, "x")}, {}, Tail{})),
        std::make_shared<const TabView>(tailOf("bo", {}, {}, Tail{}))});
    const Tabs b({std::make_shared<const TabView>(tailOf("bo", {}, {}, Tail{}))});
    Tabs c = a;
    c.apply_string(b.diff_from(a));
    CHECK(c == b);
    CHECK(c.find("ada") == nullptr);
}

void tabs_malformed_refused_unchanged() {
    Rng rng{3};
    History h;
    for (int i = 0; i < 20; ++i)
        h.step(rng);
    const Tabs b = h.state();
    const std::string d = b.init_diff();
    for (std::size_t n = 1; n < d.size(); ++n) {
        Tabs a;
        bool threw = false;
        try {
            a.apply_string(d.substr(0, n));
        } catch (const BadDiff&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(a == Tabs());
    }
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng r{seed};
        std::string junk(1 + r.below(60), '\0');
        for (char& c : junk)
            c = static_cast<char>(r.below(256));
        Tabs a = b;
        try {
            a.apply_string(junk);
        } catch (const BadDiff&) {
            CHECK(a == b);
        }
    }
    // a diff applied to a state other than the one it is from: refused, unchanged, when it keeps what that state lacks
    std::size_t refused = 0;
    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
        Rng r{seed};
        History hist;
        for (int i = 0; i < 25; ++i)
            hist.step(r);
        const Tabs mid = hist.state();
        for (int i = 0; i < 5; ++i)
            hist.step(r);
        const Tabs later = hist.state();
        Tabs other;
        try {
            other.apply_string(later.diff_from(mid));
            CHECK(other == later);   // the diff happened to keep nothing
        } catch (const BadDiff&) {
            CHECK(other == Tabs());
            ++refused;
        }
    }
    CHECK(refused > 0);
}

// The down direction through mosh's bookkeeping: the window's state ends equal to the server's, and every state it
// held on the way was one the server had.
void tabs_through_mosh_bookkeeping() {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        Rng rng{seed};
        History h;
        FakeSender<Tabs> server;
        FakeReceiver<Tabs> client;
        std::vector<Tabs> had{Tabs()};
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
                        for (const Tabs& t : had)
                            known = known || t == client.latest();
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
    CHECK(hub.apply(Action{1, ActionKind::Send, "ada", "hello"}));
    CHECK(hub.apply(Action{2, ActionKind::Queue, "ada", "later"}));
    const TabView* t = hub.state().find("ada");
    CHECK(t != nullptr && t->entries.size() == 2 && t->first == 0);
    CHECK(t->entries[0]->kind == EntryKind::Person && t->entries[0]->text == "hello" && t->entries[0]->closed);
    CHECK(t->entries[1]->kind == EntryKind::Friend && t->entries[1]->text == kReceived);
    CHECK(t->queue == std::vector<std::string>{"later"});
    CHECK(hub.state().find("bo") == nullptr);
}

void hub_limits() {
    MemoryStore store;
    Limits limits;
    limits.text = 10;
    limits.queue = 2;
    limits.queueBytes = 12;
    limits.tabs = 2;
    Hub hub(store, limits);
    std::string why;
    CHECK(!hub.apply(Action{1, ActionKind::Send, "ada", std::string(11, 'x')}, &why));
    CHECK(why.find("limit") != std::string::npos);
    CHECK(hub.state().find("ada")->entries.back()->kind == EntryKind::Notice);
    CHECK(!hub.apply(Action{2, ActionKind::Send, "ada", " \t\n"}, &why));
    CHECK(hub.apply(Action{3, ActionKind::Queue, "ada", "123456"}));
    CHECK(!hub.apply(Action{4, ActionKind::Queue, "ada", "1234567"}, &why));   // 13 bytes queued: over
    CHECK(hub.apply(Action{5, ActionKind::Queue, "ada", "12345"}));
    CHECK(!hub.apply(Action{6, ActionKind::Queue, "ada", "1"}, &why));         // a third prompt: over
    CHECK(hub.state().find("ada")->queue.size() == 2);
    CHECK(hub.apply(Action{7, ActionKind::Send, "bo", "hi"}));
    CHECK(!hub.apply(Action{8, ActionKind::Send, "cy", "hi"}, &why));           // a third tab
    CHECK(hub.state().find("cy") == nullptr);
    CHECK(why.find("limit") != std::string::npos);
}

void hub_shares_untouched_tabs() {
    MemoryStore store;
    Hub hub(store);
    hub.apply(Action{1, ActionKind::Send, "ada", "a"});
    hub.apply(Action{2, ActionKind::Send, "bo", "b"});
    const Tabs before = hub.state();
    hub.apply(Action{3, ActionKind::Send, "bo", "c"});
    CHECK(before.find("ada") == hub.state().find("ada"));
    CHECK(before.find("bo") != hub.state().find("bo"));
}

// ---- the window's mirror ----

std::vector<FriendInfo> friends2() {
    return {{"ada", "m1"}, {"bo", "m2"}};
}

Tabs stateOf(const std::string& name, const std::vector<EntryRef>& t, std::vector<std::string> queue, Tail tail = {}) {
    return Tabs({std::make_shared<const TabView>(tailOf(name, t, std::move(queue), tail))});
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

void mirror_ignores_other_tabs() {
    Model m(friends2(), true);
    Mirror mirror;
    CHECK(!mirror.apply(m, stateOf("zed", {entry(EntryKind::Person, "x")}, {"q"})));
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        CHECK(m.tab(i).transcript.empty() && m.tab(i).queue.empty());
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
    {"tabs_diff_apply_random_pairs", tabs_diff_apply_random_pairs},
    {"tabs_open_entry_sends_only_growth", tabs_open_entry_sends_only_growth},
    {"tabs_diff_bounded_by_tail", tabs_diff_bounded_by_tail},
    {"tabs_copy_is_cheap_and_shared", tabs_copy_is_cheap_and_shared},
    {"tabs_removed_tab", tabs_removed_tab},
    {"tabs_malformed_refused_unchanged", tabs_malformed_refused_unchanged},
    {"tabs_through_mosh_bookkeeping", tabs_through_mosh_bookkeeping},
    {"hub_send_and_queue", hub_send_and_queue},
    {"hub_limits", hub_limits},
    {"hub_shares_untouched_tabs", hub_shares_untouched_tabs},
    {"mirror_appends_in_order_once", mirror_appends_in_order_once},
    {"mirror_marks_entries_not_received", mirror_marks_entries_not_received},
    {"mirror_waits_for_open_entry", mirror_waits_for_open_entry},
    {"mirror_queue_keeps_draft", mirror_queue_keeps_draft},
    {"mirror_ignores_other_tabs", mirror_ignores_other_tabs},
    {"link_status_lines", link_status_lines},
    {"take_prompt_rules", take_prompt_rules},
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
