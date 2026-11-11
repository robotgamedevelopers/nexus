// Unit tests of nexus-server's tabs on the store (server/durable.h): the Hub over a DurableStore on MemFs, in memory.
// No time, no sockets, no network, no files. What a restart must keep: every tab, every queue and every client's
// applied counter, so that a reconnecting window's resent actions are skipped and the rest applied once, in order.
//
//   durable_test            runs every group
//   durable_test <group>    runs one: restart resend crash take

#include "server/durable.h"
#include "store/fs.h"
#include "sync/actions.h"
#include "sync/hub.h"

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

// A tab as text: each entry's kind and text, then the queue.
std::string text(const Tabs& tabs, const std::string& name) {
    const TabView* v = tabs.find(name);
    if (v == nullptr)
        return "-";
    std::string out = std::to_string(v->first) + ":";
    for (const EntryRef& e : v->entries)
        out += "[" + std::to_string(int(e->kind)) + e->text + "]";
    out += "|";
    for (const std::string& q : v->queue)
        out += "[" + q + "]";
    return out;
}

std::string all(const Tabs& tabs) {
    std::string out;
    for (const TabRef& t : tabs.tabs())
        out += t->name + "=" + text(tabs, t->name) + " ";
    return out;
}

// A server's restart: its sessions, Hub and memory go; the store is opened again on what is durable.
void test_restart() {
    store::MemFs fs;
    std::string before;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        CHECK(ds.report().fresh);
        Hub hub(ds);
        Applier ap(ds.applied("w1"));
        std::vector<Action> acts = {{1, ActionKind::Send, "ada", "hello"}, {2, ActionKind::Queue, "ada", "later"},
                                    {3, ActionKind::Send, "bo", "hi"}, {4, ActionKind::Send, "ada", "  "}};
        std::string why;
        for (const Action& a : ap.apply(acts))
            hub.apply(a, &why, "w1");
        CHECK(why.find("empty prompt") != std::string::npos);
        CHECK(ds.applied("w1") == 4);  // the refused one counts too
        CHECK(hub.apply(Action{1, ActionKind::Send, "cy", "x"}, nullptr, "w2"));
        before = all(hub.state());
    }
    fs.crash(fs.plan_all(false));   // a kill at the worst moment: nothing volatile survives
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    CHECK(!ds.report().fresh);
    Hub hub(ds);
    CHECK(all(hub.state()) == before);
    CHECK(text(hub.state(), "ada") == "0:[0hello][1(received)][2An empty prompt was not sent.]|[later]");
    CHECK(ds.applied("w1") == 4 && ds.applied("w2") == 1 && ds.applied("w3") == 0);
}

// A window reconnecting after a restart sends again what it has no acknowledgement for, under the same numbers: the
// server's session starts its Applier from the stored counter, so those applied before are skipped.
void test_resend() {
    store::MemFs fs;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds);
        Applier ap(ds.applied("w"));
        for (const Action& a : ap.apply({{1, ActionKind::Send, "ada", "one"}, {2, ActionKind::Send, "ada", "two"}}))
            hub.apply(a, nullptr, "w");
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    Hub hub(ds);
    // The window never saw the acknowledgement of 1 and 2: it starts its new session at 1 and sends 1, 2 and 3.
    Actions window(1);
    window.add(ActionKind::Send, "ada", "one");
    window.add(ActionKind::Send, "ada", "two");
    window.add(ActionKind::Send, "ada", "three");
    Actions serverSide(1);
    serverSide.apply_string(window.diff_from(Actions(1)));
    Applier ap(ds.applied("w"));
    std::vector<Action> fresh = ap.apply(Actions::parse(window.diff_from(Actions(1))));
    CHECK(fresh.size() == 1 && fresh[0].num == 3);
    for (const Action& a : fresh)
        CHECK(hub.apply(a, nullptr, "w"));
    CHECK(text(hub.state(), "ada") == "0:[0one][1(received)][0two][1(received)][0three][1(received)]|");
    CHECK(ds.applied("w") == 3);
    // A session that starts past the counter would skip actions: the Applier refuses it whole.
    Applier late(ds.applied("w"));
    std::string refused;
    CHECK(late.apply({{5, ActionKind::Send, "ada", "five"}}, &refused).empty());
    CHECK(!refused.empty());
    // A store that is asked to apply an action twice refuses, whatever the caller does.
    std::string why;
    CHECK(!hub.apply(Action{3, ActionKind::Send, "ada", "three"}, &why, "w"));
    CHECK(why.find("its next action is 4, not 3") != std::string::npos);
    CHECK(text(hub.state(), "ada") == "0:[0one][1(received)][0two][1(received)][0three][1(received)]|");
    // Numbers that go on from where a window was: both ends start at the same number.
    Actions resumed(4);
    resumed.add(ActionKind::Queue, "ada", "four");
    std::vector<Action> four = Applier(ds.applied("w")).apply(Actions::parse(resumed.diff_from(Actions(4))));
    CHECK(four.size() == 1 && four[0].num == 4);
    CHECK(resumed.init_diff() == resumed.diff_from(Actions(4)));
}

// A crash at every write and sync of a run of actions from two windows: the reopened state is the state after some
// prefix of the actions, at least those committed, with each window's counter matching it.
void test_crash() {
    std::vector<std::string> states;
    std::size_t acked = 0;
    bool reference = true;
    store::StoreOptions options;
    options.compact_bytes = 300;
    auto run = [&](store::MemFs& fs) {
        acked = 0;
        server::DurableStore ds;
        std::string why;
        if (!ds.open(fs, "store", &why, options))
            return;
        Hub hub(ds);
        if (reference)
            states.push_back(all(hub.state()) + std::to_string(ds.applied("a")) + "/" + std::to_string(ds.applied("b")));
        std::uint64_t num[2] = {0, 0};
        for (int i = 0; i < 12; ++i) {
            const int w = i % 3 == 2 ? 1 : 0;
            Action a{++num[w], i % 4 == 3 ? ActionKind::Queue : ActionKind::Send, i % 2 ? "bo" : "ada",
                     "p" + std::to_string(i)};
            hub.apply(a, nullptr, w ? "b" : "a");
            if (hub.failed())
                return;
            if (reference)
                states.push_back(all(hub.state()) + std::to_string(ds.applied("a")) + "/" +
                                 std::to_string(ds.applied("b")));
            ++acked;
        }
    };
    {
        store::MemFs ref;
        run(ref);
        reference = false;
        CHECK(acked == 12 && states.size() == 13);
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
            const std::string got = all(hub.state()) + std::to_string(ds.applied("a")) + "/" +
                std::to_string(ds.applied("b"));
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

// A queued prompt delivered at a turn's end, and the friend's text still growing, across a restart: the take is
// durable (the prompt left the queue once, and is in the transcript once); the open entry is not, and a closed entry
// is kept once.
void test_take() {
    store::MemFs fs;
    std::string before;
    {
        server::DurableStore ds;
        CHECK(openOn(fs, ds));
        Hub hub(ds);
        CHECK(hub.apply(Action{1, ActionKind::Queue, "ada", "one"}, nullptr, "w1"));
        CHECK(hub.apply(Action{2, ActionKind::Queue, "ada", "two"}, nullptr, "w1"));
        std::string why;
        CHECK(hub.commit("ada", {takeQueued(), TabChange{std::make_shared<const TabEntry>(TabEntry{EntryKind::Person,
            "one", true}), {}}}, &why));
        CHECK(hub.setOpen("ada", "the reply so"));
        CHECK(hub.commit("ada", {TabChange{std::make_shared<const TabEntry>(TabEntry{EntryKind::Friend,
            "the reply so far", true}), {}}}, &why));
        CHECK(hub.setOpen("ada", "and then"));
        // a take of more than the queue holds is refused whole
        CHECK(!hub.commit("ada", {takeQueued(), takeQueued()}, &why));
        CHECK(why.find("a dequeue from a queue with nothing left in it") != std::string::npos);
        CHECK(text(hub.state(), "ada") == "0:[0one][1the reply so far][1and then]|[two]");
        before = text(hub.state(), "ada");
    }
    fs.crash(fs.plan_all(false));
    server::DurableStore ds;
    CHECK(openOn(fs, ds));
    Hub hub(ds);
    CHECK(text(hub.state(), "ada") == "0:[0one][1the reply so far]|[two]");
    CHECK(ds.applied("w1") == 2);
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {{"restart", test_restart}, {"resend", test_resend}, {"crash", test_crash},
                         {"take", test_take}};

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
