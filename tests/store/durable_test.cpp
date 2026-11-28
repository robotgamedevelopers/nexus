// Unit tests of nexus-server's sessions on the store (server/durable.h): the Hub over a DurableStore on MemFs, in
// memory. No time, no sockets, no network, no files. What a restart must keep: every session (its id, what it is, its
// transcript and queue), the next id, and every client's applied counter, so that a reconnecting window's resent
// actions are skipped and the rest applied once, in order, and no id is ever minted twice.
//
//   durable_test            runs every group
//   durable_test <group>    runs one group from kGroups below

#include "server/durable.h"
#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "sync/actions.h"
#include "sync/hub.h"
#include "sync/mirror.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace nexus;
using namespace nexus::sync;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool openOn(store::MemFs& fs, server::DurableStore& ds, store::StoreOptions options = {}) {
    std::string why;
    if (!ds.open(fs, "store", &why, options)) {
        std::fprintf(stderr, "open: %s\n", why.c_str());
        return false;
    }
    return true;
}

Action act(std::uint64_t num, ActionKind kind, SessionId session, std::string text = {}) {
    Action a;
    a.num = num;
    a.kind = kind;
    a.session = session;
    a.text = std::move(text);
    return a;
}

Action ensure(std::uint64_t num, std::string aiName) {
    Action a;
    a.num = num;
    a.kind = ActionKind::Ensure;
    a.aiName = std::move(aiName);
    return a;
}

Action create(std::uint64_t num, std::string aiName, SessionId from) {
    Action a = ensure(num, std::move(aiName));
    a.kind = ActionKind::Create;
    a.session = from;
    return a;
}

// A session as text: its AI, the index of its first entry shown, each entry's kind and text, then the queue.
std::string text(const Sessions& sessions, SessionId id) {
    const SessionView* v = sessions.find(id);
    if (v == nullptr)
        return "-";
    std::string out = v->info.aiName + " " + std::to_string(v->first) + ":";
    for (const EntryRef& e : v->entries)
        out += "[" + std::to_string(int(e->kind)) + e->text + "]";
    out += "|";
    for (const std::string& q : v->queue)
        out += "[" + q + "]";
    return out;
}

std::string all(const Sessions& sessions) {
    std::string out;
    for (const SessionRef& s : sessions.sessions()) {
        out += std::to_string(s->id) + "=" + text(sessions, s->id) + " ";
    }
    return out;
}

// A server's restart: its links, Hub and memory go; the store is opened again on what is durable.
void test_restart() {
    store::MemFs fs;
    std::string before;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        CHECK(ds.report().fresh);
        Hub hub(ds);
        Applier ap(ds.applied("w1"));
        std::vector<Action> acts = {ensure(1, "ada"), ensure(2, "bo"), act(3, ActionKind::Send, 1, "hello"),
                                    act(4, ActionKind::Queue, 1, "later"), act(5, ActionKind::Send, 2, "hi"),
                                    act(6, ActionKind::Send, 1, "  ")};
        std::string why;
        for (const Action& a : ap.apply(acts))
            hub.apply(a, &why, "w1");
        CHECK(why.find("empty prompt") != std::string::npos);
        CHECK(ds.applied("w1") == 6);  // the refused one counts too
        CHECK(hub.apply(ensure(1, "cy"), nullptr, "w2"));
        CHECK(hub.apply(act(2, ActionKind::Send, 3, "x"), nullptr, "w2"));
        before = all(hub.state());
    }
    fs.crash(fs.plan_all(false));   // a kill at the worst moment: nothing volatile survives
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    CHECK(!ds.report().fresh);
    Hub hub(ds);
    CHECK(all(hub.state()) == before);
    CHECK(text(hub.state(), 1) == "ada 0:[0hello][1(received)][2An empty prompt was not sent.]|[later]");
    CHECK(ds.applied("w1") == 6 && ds.applied("w2") == 2 && ds.applied("w3") == 0);
    // the ids go on from where they were: an ensure for an AI with a session makes none, a create the next id
    CHECK(ds.nextId() == 4);
    CHECK(hub.apply(ensure(3, "cy"), nullptr, "w2"));
    CHECK(ds.size() == 3);
    CHECK(hub.apply(create(4, "cy", 3), nullptr, "w2"));
    CHECK(ds.size() == 4 && ds.find(4) != nullptr && ds.find(4)->info.aiName == "cy" && ds.nextId() == 5);
}

// A window reconnecting after a restart sends again what it has no acknowledgement for, under the same numbers: the
// server's link starts its Applier from the stored counter, so those applied before are skipped.
void test_resend() {
    store::MemFs fs;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds);
        Applier ap(ds.applied("w"));
        for (const Action& a : ap.apply({ensure(1, "ada"), act(2, ActionKind::Send, 1, "one"),
                                         act(3, ActionKind::Send, 1, "two")}))
            hub.apply(a, nullptr, "w");
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    Hub hub(ds);
    // The window never saw the acknowledgement of 1 to 3: it starts its new link at 1 and sends 1 to 4.
    Actions window(1);
    window.add(ensure(0, "ada"));
    window.add(act(0, ActionKind::Send, 1, "one"));
    window.add(act(0, ActionKind::Send, 1, "two"));
    window.add(act(0, ActionKind::Send, 1, "three"));
    Applier ap(ds.applied("w"));
    std::vector<Action> fresh = ap.apply(Actions::parse(window.diff_from(Actions(1))));
    CHECK(fresh.size() == 1 && fresh[0].num == 4);
    for (const Action& a : fresh)
        CHECK(hub.apply(a, nullptr, "w"));
    CHECK(text(hub.state(), 1) == "ada 0:[0one][1(received)][0two][1(received)][0three][1(received)]|");
    CHECK(ds.applied("w") == 4 && ds.size() == 1);
    // A link that starts past the counter would skip actions: the Applier refuses it whole.
    Applier late(ds.applied("w"));
    std::string refused;
    CHECK(late.apply({act(6, ActionKind::Send, 1, "six")}, &refused).empty());
    CHECK(!refused.empty());
    // A store that is asked to apply an action twice refuses, whatever the caller does.
    std::string why;
    CHECK(!hub.apply(act(4, ActionKind::Send, 1, "three"), &why, "w"));
    CHECK(why.find("its next action is 5, not 4") != std::string::npos);
    CHECK(text(hub.state(), 1) == "ada 0:[0one][1(received)][0two][1(received)][0three][1(received)]|");
    // Numbers that go on from where a window was: both ends start at the same number.
    Actions resumed(5);
    resumed.add(act(0, ActionKind::Queue, 1, "five"));
    std::vector<Action> five = Applier(ds.applied("w")).apply(Actions::parse(resumed.diff_from(Actions(5))));
    CHECK(five.size() == 1 && five[0].num == 5);
    CHECK(resumed.init_diff() == resumed.diff_from(Actions(5)));
}

// A crash at every write and sync of a run of actions from two windows: the reopened state is the state after some
// prefix of the actions, at least those committed, with each window's counter matching it.
void test_crash() {
    std::vector<std::string> states;
    std::size_t acked = 0;
    bool reference = true;
    store::StoreOptions options;
    options.compact_bytes = 300;
    auto state = [](const Hub& hub, const server::DurableStore& ds) {
        return all(hub.state()) + "next " + std::to_string(ds.nextId()) + " " + std::to_string(ds.applied("a")) + "/" +
            std::to_string(ds.applied("b"));
    };
    auto run = [&](store::MemFs& fs) {
        acked = 0;
        server::DurableStore ds;
        std::string why;
        if (!ds.open(fs, "store", &why, options))
            return;
        Hub hub(ds);
        if (reference)
            states.push_back(state(hub, ds));
        std::uint64_t num[2] = {0, 0};
        for (int i = 0; i < 14; ++i) {
            const int w = i % 3 == 2 ? 1 : 0;
            Action a;
            if (i < 2) {
                a = ensure(++num[w], i == 0 ? "ada" : "bo");
            } else {
                a = act(++num[w], i % 4 == 3 ? ActionKind::Queue : ActionKind::Send, i % 2 ? 2 : 1,
                        "p" + std::to_string(i));
            }
            hub.apply(a, nullptr, w ? "b" : "a");
            if (hub.failed())
                return;
            if (reference)
                states.push_back(state(hub, ds));
            ++acked;
        }
    };
    {
        store::MemFs ref;
        run(ref);
        reference = false;
        CHECK(acked == 14 && states.size() == 15);
    }
    std::uint64_t taken = 0;
    for (std::uint64_t point = 1;; ++point) {
        bool reached = false;
        for (std::uint64_t plan = 0; plan < 6; ++plan) {
            store::MemFs fs;
            fs.fail_at(point);
            run(fs);
            if (!fs.crashed())
                break;
            reached = true;
            store::Rng rng(point * 7 + plan);
            fs.crash(plan == 0 ? fs.plan_all(false) : plan == 1 ? fs.plan_all(true) : fs.plan(rng));
            server::DurableStore ds;
            CHECK(openOn(fs, ds, options));
            Hub hub(ds);
            const std::string got = state(hub, ds);
            bool found = false;
            for (std::size_t i = acked; i < states.size(); ++i)
                found = found || states[i] == got;
            CHECK(found);
            ++taken;
        }
        if (!reached)
            break;
    }
    CHECK(taken > 100);
}

// Group commit (DurableStore::beginBatch, flush), as the server's loop does it: the same run of actions, in batches of
// one to four, each batch made durable by one flush. A crash at every write and sync: the reopened state is the state
// after some prefix of the actions, at least every one whose batch's flush returned (only those are acknowledged).
void test_batch() {
    std::vector<std::string> states;
    std::size_t acked = 0;
    bool reference = true;
    store::StoreOptions options;
    options.compact_bytes = 300;
    auto state = [](const Hub& hub, const server::DurableStore& ds) {
        return all(hub.state()) + "next " + std::to_string(ds.nextId()) + " " + std::to_string(ds.applied("a")) + "/" +
            std::to_string(ds.applied("b"));
    };
    const int sizes[] = {1, 3, 4, 2, 4};    // 14 actions
    std::uint64_t flushes = 0;
    auto run = [&](store::MemFs& fs) {
        acked = 0;
        flushes = 0;
        server::DurableStore ds;
        std::string why;
        if (!ds.open(fs, "store", &why, options))
            return;
        Hub hub(ds);
        if (reference)
            states.push_back(state(hub, ds));
        std::uint64_t num[2] = {0, 0};
        int i = 0;
        for (int size : sizes) {
            ds.beginBatch();
            for (int k = 0; k < size; ++k, ++i) {
                const int w = i % 3 == 2 ? 1 : 0;
                Action a;
                if (i < 2) {
                    a = ensure(++num[w], i == 0 ? "ada" : "bo");
                } else {
                    a = act(++num[w], i % 4 == 3 ? ActionKind::Queue : ActionKind::Send, i % 2 ? 2 : 1,
                            "p" + std::to_string(i));
                }
                hub.apply(a, nullptr, w ? "b" : "a");
                if (hub.failed())
                    return;
                if (reference)
                    states.push_back(state(hub, ds));
            }
            if (!ds.flush(&why))
                return;
            ++flushes;
            acked = static_cast<std::size_t>(i);
        }
    };
    {
        store::MemFs ref;
        run(ref);
        reference = false;
        CHECK(acked == 14 && states.size() == 15 && flushes == 5);
    }
    std::uint64_t taken = 0;
    for (std::uint64_t point = 1;; ++point) {
        bool reached = false;
        for (std::uint64_t plan = 0; plan < 6; ++plan) {
            store::MemFs fs;
            fs.fail_at(point);
            run(fs);
            if (!fs.crashed())
                break;
            reached = true;
            store::Rng rng(point * 7 + plan);
            fs.crash(plan == 0 ? fs.plan_all(false) : plan == 1 ? fs.plan_all(true) : fs.plan(rng));
            server::DurableStore ds;
            CHECK(openOn(fs, ds, options));
            Hub hub(ds);
            const std::string got = state(hub, ds);
            bool found = false;
            for (std::size_t i = acked; i < states.size(); ++i)
                found = found || states[i] == got;
            CHECK(found);
            ++taken;
        }
        if (!reached)
            break;
    }
    CHECK(taken > 30);
}

// A batch that is never flushed is lost whole by a crash that keeps nothing volatile, and one flush makes all of it
// durable: no step of a batch is durable through its own commit.
void test_batch_flush() {
    store::MemFs fs;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds);
        ds.beginBatch();
        hub.apply(ensure(1, "ada"), nullptr, "a");
        hub.apply(act(2, ActionKind::Send, 1, "one"), nullptr, "a");
        CHECK(ds.applied("a") == 2);    // applied here at once
    }
    fs.crash(fs.plan_all(false));
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        CHECK(ds.applied("a") == 0 && ds.size() == 0);
        Hub hub(ds);
        ds.beginBatch();
        hub.apply(ensure(1, "ada"), nullptr, "a");
        hub.apply(act(2, ActionKind::Send, 1, "one"), nullptr, "a");
        std::string why;
        CHECK(ds.flush(&why));
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    CHECK(ds.applied("a") == 2 && ds.size() == 1);
}

EntryRef entry(EntryKind kind, const std::string& text) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, text, true});
}

// A queued prompt delivered at a turn's end, and the AI's text still growing, across a restart: the take is
// durable (the prompt left the queue once, and is in the transcript once); the open entry is not, and a closed entry
// is kept once.
void test_take() {
    store::MemFs fs;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds);
        CHECK(hub.apply(ensure(1, "ada"), nullptr, "w1"));
        CHECK(hub.apply(act(2, ActionKind::Queue, 1, "one"), nullptr, "w1"));
        CHECK(hub.apply(act(3, ActionKind::Queue, 1, "two"), nullptr, "w1"));
        std::string why;
        CHECK(hub.commit(1, {takeQueued(), SessionChange{entry(EntryKind::Person, "one"), {}}}, &why));
        CHECK(hub.setOpen(1, "the reply so"));
        CHECK(hub.commit(1, {SessionChange{entry(EntryKind::AI, "the reply so far"), {}}}, &why));
        CHECK(hub.setOpen(1, "and then"));
        // a take of more than the queue holds is refused whole
        CHECK(!hub.commit(1, {takeQueued(), takeQueued()}, &why));
        CHECK(why.find("a dequeue from a queue with nothing left in it") != std::string::npos);
        CHECK(text(hub.state(), 1) == "ada 0:[0one][1the reply so far][1and then]|[two]");
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    Hub hub(ds);
    CHECK(text(hub.state(), 1) == "ada 0:[0one][1the reply so far]|[two]");
    CHECK(ds.applied("w1") == 3);
}

// A store an older server left, keyed by AI tab: opened through the DurableStore, each tab is a session the hub
// shows, with its AI and the harness the server runs, transcripts and queues intact, counters kept; a resent
// action is still skipped, and an ensure for a migrated AI makes nothing new.
void test_migrate() {
    store::MemFs fs;
    {
        // a fresh store, then its journal replaced by an AI-tab snapshot (format 2)
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
    }
    {
        store::Writer w;
        w.u8(2);
        w.u32(2);
        for (const char* name : {"ada", "bo"}) {
            w.str(name);
            w.u32(1);
            w.u8(0);
            w.u8(1);
            w.str(std::string("to ") + name);
            w.u32(name[0] == 'a' ? 1 : 0);
            if (name[0] == 'a') {
                w.str("waiting");
            }
            w.u64(1);
            w.u64(0);
            w.str("");
            w.u32(0);
        }
        w.u32(1);
        w.str("w1");
        w.u64(5);
        store::Journal j(fs, "store");
        store::Journal::Recovery rec;
        CHECK(j.open(rec).is_ok());
        CHECK(j.append(store::Bytes{0}).is_ok());       // a record for the snapshot to stand for
        CHECK(j.compact(w.out()).is_ok());
    }
    server::DurableStore ds;
    store::StoreOptions options;
    options.legacy_harness = "claude";
    CHECK(openOn(fs, ds, options));
    CHECK(ds.report().migrated == 2);
    Hub hub(ds);
    CHECK(all(hub.state()) == "1=ada 0:[0to ada]|[waiting] 2=bo 0:[0to bo]| ");
    CHECK(hub.state().find(1)->info.harness == "claude");
    CHECK(ds.applied("w1") == 5 && ds.nextId() == 3);
    // the window resends 5 (skipped) and goes on with 6, an ensure for ada: nothing new
    std::vector<Action> fresh = Applier(ds.applied("w1")).apply({act(5, ActionKind::Send, 1, "again"),
                                                                 ensure(6, "ada")});
    CHECK(fresh.size() == 1 && fresh[0].num == 6);
    CHECK(hub.apply(fresh[0], nullptr, "w1"));
    CHECK(ds.size() == 2 && ds.nextId() == 3 && ds.applied("w1") == 6);
}

// An entry of a kind a later server wrote, which this build does not know, is shown as a notice (a plain dim line);
// the clear boundary still consumes kind 4, and Tool and Bus survive with their original kinds.
void test_kinds() {
    store::MemFs fs;
    {
        std::unique_ptr<store::Store> s;
        CHECK(store::Store::open(fs, "store", store::StoreOptions{}, s).is_ok());
        store::Change made;
        made.kind = store::Change::Kind::SetInfo;
        made.info = store::Info{"ada", "claude", "", "", ""};
        std::vector<store::Change> step = {made};
        for (std::uint8_t kind : {0, 4, 3, 5, 7}) {
            store::Change c;
            c.kind = store::Change::Kind::Append;
            c.entry = store::Entry{kind, kind == 5 ? "bo\nhello" : "kind " + std::to_string(kind), true};
            step.push_back(c);
        }
        CHECK(s != nullptr && s->commit(1, step).is_ok());
    }
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    const StoredSession* t = ds.find(1);
    CHECK(t != nullptr && t->info.contextEpoch == 1 && t->transcript.size() == 3);
    if (t != nullptr && t->transcript.size() == 3) {
        CHECK(t->transcript[0]->kind == EntryKind::Tool && t->transcript[0]->text == "kind 3");
        CHECK(t->transcript[1]->kind == EntryKind::Bus && t->transcript[1]->text == "bo\nhello");
        CHECK(t->transcript[2]->kind == EntryKind::Notice && t->transcript[2]->text == "kind 7");
    }
}

// Reconnect uses the saved destination, including the extent of its address, in both log and snapshot recovery.
void test_switch_notice_reconnect() {
    const std::string address = "rotation@example.test";
    const std::string switched = std::string(kAccountSwitchPrefix) + address + " (work)";
    const auto end = static_cast<std::uint32_t>(kAccountSwitchPrefix.size() + address.size());
    for (bool compact : {false, true}) {
        store::MemFs fs;
        {
            server::DurableStore ds;
            store::StoreOptions options;
            if (compact) options.compact_bytes = 0;
            CHECK(openOn(fs, ds, options));
            Hub hub(ds);
            CHECK(hub.apply(ensure(1, "ada"), nullptr, "w"));
            auto notice = std::make_shared<const SessionEntry>(SessionEntry{EntryKind::Notice, switched, true, end});
            CHECK(hub.commit(1, {SessionChange{notice, {}}}, nullptr));
        }
        fs.crash(fs.plan_all(false));
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds);
        Sessions window;
        window.apply_string(hub.state().init_diff());
        const auto* session = window.find(1);
        CHECK(session && session->entries.size() == 1);
        if (session && session->entries.size() == 1) {
            CHECK(session->entries[0]->text == switched && session->entries[0]->accountEmailEnd == end);
        }
    }
}

// A new window can reconstruct the sender and body after journal recovery or compaction. An
// identical human-authored payload remains a human entry, even when it resembles a bus record.
void test_bus_reconnect() {
    const std::string payload = "bo\nfirst line\nsecond line";
    const std::string driverPrompt = "Message from bo on the message bus (id 123-1)\nText attachment: incoming/bus-stored/message.txt";
    for (bool compact : {false, true}) {
        store::MemFs fs;
        {
            server::DurableStore ds;
            store::StoreOptions options;
            if (compact) options.compact_bytes = 0;
            CHECK(openOn(fs, ds, options));
            Hub hub(ds);
            CHECK(hub.apply(ensure(1, "ada"), nullptr, "w"));
            CHECK(hub.commit(1, {{entry(EntryKind::Person, payload), {}},
                                 {entry(EntryKind::Bus, payload), driverPrompt}}, nullptr));
        }
        fs.crash(fs.plan_all(false));
        server::DurableStore recovered;
        CHECK(openOn(fs, recovered));
        const auto* stored = recovered.find(1);
        CHECK(stored && stored->busPrompts.size() == 1 && stored->busPrompts.contains(1));
        if (stored && stored->busPrompts.contains(1)) CHECK(stored->busPrompts.at(1) == driverPrompt);
        Hub server(recovered);
        server.setBusSenders({{"ada", {"bo"}}});
        Sessions window;
        const auto firstDiff = server.state().init_diff();
        CHECK(firstDiff.find("incoming/bus-stored/message.txt") == std::string::npos);
        window.apply_string(firstDiff);
        Model model({{"ada", ""}});
        Mirror mirror;
        CHECK(mirror.apply(model, window));
        CHECK(!mirror.apply(model, window));
        const auto& transcript = model.tab(0).transcript;
        CHECK(transcript.size() == 2);
        if (transcript.size() == 2) {
            CHECK(transcript[0].kind == EntryKind::Person && transcript[0].text == payload);
            CHECK(transcript[1].kind == EntryKind::Bus && transcript[1].text == payload);
        }
    }
}

// Rows written before sender restrictions have no policy metadata. On reopen, use each
// recipient's current senders for both new windows and windows following a policy change.
void test_bus_history() {
    const std::string retired = "eve\nexpired message for ada";
    const std::string crossProfile = "bo\nmessage from the other profile";
    const std::string allowed = "bo\nallowed in ada";
    const std::string alsoAllowed = "cy\nsecond peer in ada";
    const std::string otherAllowed = "eve\nallowed in di";
    const std::string human = "eve\nquoted by a person";
    store::MemFs fs;
    {
        server::DurableStore store;
        CHECK(openOn(fs, store));
        Hub hub(store);
        CHECK(hub.apply(ensure(1, "ada"), nullptr, "w"));
        CHECK(hub.apply(ensure(2, "di"), nullptr, "w"));
        CHECK(store.commit(1, {{entry(EntryKind::Person, human), {}},
                               {entry(EntryKind::Bus, retired), {}},
                               {entry(EntryKind::AI, "AI reply"), {}},
                               {entry(EntryKind::Bus, alsoAllowed), {}},
                               {entry(EntryKind::Bus, allowed), {}},
                               {entry(EntryKind::Notice, "notice"), {}}}, "nexus:bus:in:ada:123-0", 1, nullptr));
        CHECK(store.commit(2, {{entry(EntryKind::Bus, crossProfile), {}},
                               {entry(EntryKind::Bus, otherAllowed), {}}}, {}, 0, nullptr));
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore reopened;
    CHECK(openOn(fs, reopened));
    Hub hub(reopened);
    const Sessions initial = hub.state();
    CHECK(hub.setBusSenders({{"ada", {"cy", "bo", "unused"}}, {"di", {"eve"}}}));
    const Sessions granted = hub.state();
    CHECK(hub.setBusSenders({{"ada", {}}})); // empty selection for ada, missing selection for di
    const Sessions revoked = hub.state();

    auto checkRows = [&](const Sessions& window, bool grantedNow) {
        const auto* ada = window.find(1);
        const auto* di = window.find(2);
        CHECK(ada && ada->first == 0 && ada->entries.size() == 6);
        CHECK(di && di->first == 0 && di->entries.size() == 2);
        if (!ada || ada->entries.size() != 6 || !di || di->entries.size() != 2) return;
        const auto busKind = window.format() >= kTablesFormat ? EntryKind::Bus : EntryKind::Notice;
        CHECK(*ada->entries[0] == *entry(EntryKind::Person, human));
        CHECK(*ada->entries[1] == *entry(busKind, ""));
        CHECK(*ada->entries[2] == *entry(EntryKind::AI, "AI reply"));
        CHECK(*ada->entries[3] == *entry(busKind, grantedNow ? alsoAllowed : ""));
        CHECK(*ada->entries[4] == *entry(busKind, grantedNow ? allowed : ""));
        CHECK(*ada->entries[5] == *entry(EntryKind::Notice, "notice"));
        CHECK(*di->entries[0] == *entry(busKind, ""));
        CHECK(*di->entries[1] == *entry(busKind, grantedNow ? otherAllowed : ""));
        if (window.format() >= kTablesFormat) {
            std::vector<std::string> adaSenders, diSenders;
            if (grantedNow) {
                adaSenders = {"bo", "cy"};
                diSenders = {"eve"};
            }
            CHECK(ada->busSenders == adaSenders);
            CHECK(di->busSenders == diSenders);
        } else {
            CHECK(!ada->busSenders && !di->busSenders);
        }
    };
    for (std::uint32_t format = kMinPeer; format <= kStateFormat; ++format) {
        Sessions window, previous;
        window.setFormat(format);
        previous.setFormat(format);
        for (const Sessions* projection : {&initial, &granted, &revoked}) {
            Sessions next = *projection;
            next.setFormat(format);
            const bool grantedNow = projection == &granted;
            const auto delta = next.diff_from(previous);
            const auto full = next.init_diff();
            for (const auto* bytes : {&full, &delta}) {
                CHECK(bytes->find("expired message for ada") == std::string::npos);
                CHECK(bytes->find("message from the other profile") == std::string::npos);
                if (!grantedNow) {
                    CHECK(bytes->find("allowed in ada") == std::string::npos);
                    CHECK(bytes->find("second peer in ada") == std::string::npos);
                    CHECK(bytes->find("allowed in di") == std::string::npos);
                }
                CHECK(bytes->find("unused") == std::string::npos);
            }
            window.apply_string(delta);
            checkRows(window, grantedNow);
            Sessions reconnect;
            reconnect.setFormat(format);
            reconnect.apply_string(full);
            checkRows(reconnect, grantedNow);
            previous = std::move(next);
        }
    }
    CHECK(hub.hasBusReceipt("ada", "123-0"));
    CHECK(hub.receiveBus("ada", "123-0", "eve", "duplicate", "duplicate", nullptr));
    const auto* stored = reopened.find(1);
    CHECK(stored && stored->transcript.size() == 6 && stored->transcript[1]->text == retired &&
          stored->transcript[3]->text == alsoAllowed && stored->transcript[4]->text == allowed);
    stored = reopened.find(2);
    CHECK(stored && stored->transcript.size() == 2 && stored->transcript[0]->text == crossProfile &&
          stored->transcript[1]->text == otherAllowed);
}

void test_native_bus() {
    class Driver final : public Responder {
    public:
        SessionInfo defaults(const std::string& name) const override { return {name, "driver", {}, {}, {}}; }
        std::vector<SessionChange> prepareBus(SessionId, const std::string&, std::string&) override { return {}; }
        std::vector<SessionChange> send(SessionId, const std::string&) override { return {}; }
        std::vector<SessionChange> queue(SessionId, const std::string&) override { return {}; }
        std::vector<SessionChange> stop(SessionId) override { return {}; }
        void sent(SessionId, bool) override {}
        void configured(SessionId, const SessionInfo&) override {}
    } driver;
    store::MemFs fs;
    accounts::Registry registry;
    for (const auto* name : {"ada", "bo"}) registry.ais.push_back({name, {}, {}, {}});
    std::string why, receipt;
    const std::string nonce(32, 'a'), replyNonce(32, 'b');
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds, Limits{}, &driver);
        hub.useAccounts(registry, nullptr);
        CHECK(hub.apply(ensure(1, "ada"), &why, "w"));
        CHECK(hub.apply(create(2, "ada", 1), &why, "w"));
        receipt = hub.sendBus(2, nonce, "bo", "Review", "Please check.", {}, why);
        CHECK(!receipt.empty() && why.empty());
        CHECK(hub.sendBus(2, nonce, "bo", "Review", "Please check.", {}, why) == receipt);
        const auto* recipient = ds.find(3);
        CHECK(recipient && recipient->busPrompts.size() == 1);
        CHECK(hub.state().init_diff().find("Please check.") != std::string::npos);
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    Hub hub(ds, Limits{}, &driver);
    hub.useAccounts(registry, nullptr);
    CHECK(hub.state().init_diff().find("Please check.") != std::string::npos);
    CHECK(hub.sendBus(2, nonce, "bo", "Review", "Please check.", {}, why) == receipt);
    CHECK(hub.apply(create(3, "bo", 3), &why, "w"));
    CHECK(hub.sendBus(4, replyNonce, "ada", "Re", "Wrong conversation", receipt, why).empty());
    CHECK(!hub.sendBus(3, replyNonce, "ada", "Re", "Finished.", receipt, why).empty());
    CHECK(ds.find(2) && ds.find(2)->busPrompts.size() == 1);
    CHECK(ds.find(1) && ds.find(1)->busPrompts.empty());
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {{"restart", test_restart}, {"resend", test_resend}, {"crash", test_crash},
                         {"batch", test_batch}, {"batch_flush", test_batch_flush}, {"take", test_take},
                         {"migrate", test_migrate}, {"kinds", test_kinds}, {"switch_notice_reconnect", test_switch_notice_reconnect},
                         {"bus_reconnect", test_bus_reconnect}, {"bus_history", test_bus_history}, {"native_bus", test_native_bus}};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0)
            continue;
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "durable_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "durable_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("durable_test: ok\n");
    return 0;
}
