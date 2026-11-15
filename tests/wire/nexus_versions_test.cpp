// Unit tests of nexus itself across versions: this build's real wire code (sync::Sessions, sync::Actions, the Hello of
// sync/formats.h) against a later version's generated code (tests/wire/next/NexusNext.schema, wire_next.cpp), each
// writing what the other reads, both ways. In memory only: no time, no sockets, no files.
//
//   nexus_versions_test           runs every group
//   nexus_versions_test <group>   runs one:
//     sessions   a later server's state diff applied by this build's window: the added kind shows as a notice, the
//                added activity as busy, the added field is skipped, each counted (Sessions::unknown), and nothing is
//                refused; this build's server's diffs (a first one and one that grows an open entry) read whole by a
//                later window, nothing unknown
//     actions    a later window's actions read by this build's server: the added kind reads as unknown (which the
//                server refuses one by one, never the link), the added field is skipped, both counted, the rest as
//                written, the secret's bytes included; this build's window's actions read whole by a later server
//     hello      each version's Hello read by the other: the added field is skipped and counted; the decision
//                (sync::refusalOfServer, sync::refusalOfWindow) refuses only when the other's proto is below this
//                end's min_peer

#include "versions.h"

#include "sync/actions.h"
#include "sync/formats.h"
#include "sync/sessions.h"
#include "sync/wire.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace nexus::sync;
using wiretest::Bytes;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

std::string text(const Bytes& b) {
    return std::string(b.begin(), b.end());
}

Bytes bytes(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return c - 'a' + 10;
}

std::string hex(const Bytes& b) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t c : b) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

Bytes unhex(const std::string& s) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(hexDigit(s[i]) * 16 + hexDigit(s[i + 1])));
    }
    return out;
}

EntryRef entry(nexus::EntryKind kind, const std::string& t, bool closed = true) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, t, closed});
}

SessionRef session(SessionId id, const std::string& friendName, Activity activity, std::vector<EntryRef> entries,
                   std::vector<std::string> queue = {}) {
    auto v = std::make_shared<SessionView>();
    v->id = id;
    v->info.friendName = friendName;
    v->info.harness = "claude";
    v->info.instance = "studio";
    v->activity = activity;
    v->entries = std::move(entries);
    v->queue = std::move(queue);
    return v;
}

void test_sessions() {
    using Names = std::vector<std::string>;

    // A later server's state, applied by this build's window.
    Sessions window;
    bool threw = false;
    try {
        window.apply_string(text(wiretest::nextSessions()));
    } catch (const BadDiff& e) {
        std::fprintf(stderr, "a later server's diff was refused: %s\n", e.what());
        threw = true;
    }
    CHECK(!threw);
    CHECK(window.unknown() == 3);       // the title, the Image kind, the Waiting activity
    CHECK(window.sessions().size() == 2);
    if (window.sessions().size() == 2) {
        const SessionView& a = *window.sessions()[0];
        CHECK(a.id == 7 && a.info.friendName == "rowan" && a.info.instance == "studio");
        CHECK(a.activity == Activity::Busy);
        CHECK(a.entries.size() == 3);
        if (a.entries.size() == 3) {
            CHECK(a.entries[0]->kind == nexus::EntryKind::Person && a.entries[0]->text == "hi");
            CHECK(a.entries[1]->kind == nexus::EntryKind::Notice && a.entries[1]->text == "a picture");
            CHECK(a.entries[2]->kind == nexus::EntryKind::Tool && a.entries[2]->text == "ls");
        }
        CHECK((a.queue == Names{"later"}));
        const SessionView& b = *window.sessions()[1];
        CHECK(b.id == 8 && b.info.friendName == "stella");
        CHECK(b.activity == Activity::Busy);    // Waiting, which this build does not know, shows as busy
        CHECK(b.entries.size() == 1);
    }

    // This build's server's diffs, read by a later window: a first diff, then one that grows an open entry.
    const Sessions empty;
    const Sessions first({session(7, "rowan", Activity::Busy,
                                  {entry(nexus::EntryKind::Person, "hi"), entry(nexus::EntryKind::Friend, "hel", false)},
                                  {"later"})});
    const std::string firstDiff = first.diff_from(empty);
    const wiretest::Read r1 = wiretest::nextReadSessions(bytes(firstDiff));
    CHECK(r1.loaded && !r1.damaged && !r1.refused && r1.unknown == 0);
    CHECK(r1.friendName == "rowan" && r1.instance == "studio" && !r1.titlePresent);
    CHECK((r1.kinds == Names{"Person", "Friend"}));
    CHECK((r1.queue == Names{"later"}));
    const Sessions grown({session(7, "rowan", Activity::Idle,
                                  {entry(nexus::EntryKind::Person, "hi"), entry(nexus::EntryKind::Friend, "hello")})});
    const wiretest::Read r2 = wiretest::nextReadSessions(bytes(grown.diff_from(first)));
    CHECK(r2.loaded && !r2.damaged && !r2.refused && r2.unknown == 0);
    CHECK((r2.kinds == Names{"Friend"}));       // only the entry that grew
    // And this build reads its own the same, nothing unknown.
    Sessions own;
    own.apply_string(firstDiff);
    CHECK(own == first && own.unknown() == 0);
}

void test_actions() {
    using Names = std::vector<std::string>;

    // A later window's actions, read by this build's server.
    int unknown = -1;
    std::vector<Action> read;
    bool threw = false;
    try {
        read = Actions::parse(text(wiretest::nextActions()), kStateFormat, &unknown);
    } catch (const BadDiff& e) {
        std::fprintf(stderr, "a later window's actions were refused: %s\n", e.what());
        threw = true;
    }
    CHECK(!threw);
    CHECK(unknown == 2);    // the Rename kind and the note field
    CHECK(read.size() == 4);
    if (read.size() == 4) {
        CHECK(read[0].num == 5 && read[0].kind == ActionKind::Send && read[0].session == 7 && read[0].text == "hi");
        // Unknown, and nothing of it is kept: what its fields mean is the later version's.
        CHECK(read[1].num == 6 && read[1].kind == ActionKind::Unknown && read[1].session == 0 && read[1].text.empty());
        CHECK(read[2].num == 7 && read[2].kind == ActionKind::SecretAnswer && read[2].request == 3);
        CHECK(read[2].secret.bytes() == "test-value");
        CHECK(read[3].num == 8 && read[3].kind == ActionKind::Stop && read[3].session == 7);
    }
    // The server's state takes them as one unbroken run.
    Actions server(5);
    server.apply_string(text(wiretest::nextActions()));
    CHECK(server.next() == 9);

    // This build's window's actions, read by a later server.
    Actions window(3);
    Action send;
    send.kind = ActionKind::Send;
    send.session = 7;
    send.text = "hi";
    window.add(send);
    Action create;
    create.kind = ActionKind::Create;
    create.session = 7;
    create.friendName = "stella";
    window.add(create);
    Action answer;
    answer.kind = ActionKind::SecretAnswer;
    answer.request = 4;
    answer.secret = SecretValue("test-value");
    window.add(answer);
    const wiretest::ReadActions r = wiretest::nextReadActions(bytes(window.diff_from(Actions(3))));
    CHECK(r.loaded && !r.damaged && !r.refused && r.unknown == 0);
    CHECK(r.start == 3);
    CHECK((r.kinds == Names{"Send", "Create", "SecretAnswer"}));
    CHECK((r.texts == Names{"hi", "", ""}));
    CHECK((r.friends == Names{"", "stella", ""}));
    CHECK((r.sessions == std::vector<std::uint64_t>{7, 7, 0}));
    CHECK((r.requests == std::vector<std::uint64_t>{0, 0, 4}));
    CHECK((r.secrets == Names{"", "", "test-value"}));
}

void test_hello() {
    // A later end's Hello, read by this build.
    Hello later;
    std::string why;
    int unknown = -1;
    CHECK(readHello(hex(wiretest::nextHello(6, 2)), &later, &why, &unknown));
    CHECK(later == (Hello{6, 2}));
    CHECK(unknown > 0);     // the build field, skipped and counted (0 when a later Hello leaves it out)
    // It works with this build either way round: this build's proto is not below its min_peer, nor its below ours.
    CHECK(refusalOfServer(later).empty());
    CHECK(refusalOfWindow(later).empty());
    // A later end that no longer works with this build's proto is refused, in plain words, whichever end it is.
    Hello strict;
    CHECK(readHello(hex(wiretest::nextHello(9, kStateFormat + 1)), &strict, &why, &unknown));
    const std::string server = refusalOfServer(strict);
    CHECK(server.find("too old") != std::string::npos);
    const std::string window = refusalOfWindow(strict);
    CHECK(window.find("too old") != std::string::npos);
    CHECK(server != window);

    // This build's Hello, read by a later end: whole, nothing unknown, and no build (this build never writes one).
    const wiretest::ReadHello ours = wiretest::nextReadHello(unhex(helloText(kHello)));
    CHECK(ours.loaded && ours.unknown == 0 && !ours.buildPresent);
    CHECK(ours.proto == kStateFormat && ours.minPeer == kMinPeer);
}

struct Group {
    const char* name;
    void (*run)();
};

const Group groups[] = {
    {"sessions", test_sessions},
    {"actions", test_actions},
    {"hello", test_hello},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : groups) {
        if (argc < 2 || std::strcmp(argv[1], g.name) == 0) {
            g.run();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "nexus_versions_test: no group named %s\n", argv[1]);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "nexus_versions_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("nexus_versions_test: ok\n");
    return 0;
}
