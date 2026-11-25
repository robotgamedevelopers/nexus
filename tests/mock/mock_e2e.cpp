// The mock's functional test: nexus-mock started for real, in a directory of the test's own, and a client connected to
// it over the real link on loopback, by the command the mock prints for clients.
//
//   mock_e2e <nexus-mock> <dir>
//
// With five AIs and the sign-in preset, instant replies: the server's list is the five AIs in order then Ida; only Ida
// has an account, and it needs a sign-in; each of the five has its seeded conversation, ending idle, with a tool line,
// the stopped turn, the error and the turn its driver's exit cut short in the tail a client is sent; a prompt to Ada
// gets the first scripted reply, exactly once, and nothing more comes after it. Then the state with no AIs: an empty
// list. Then `nexus-mock stop` stops the server and removes the directory.

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "command.h"
#include "mock.h"
#include "mock_link.h"
#include "sync/sessions.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr int kStallSeconds = 60;

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool run(const std::string& mock, std::vector<std::string> arguments) {
    const Command c{mock, arguments, "nexus-mock " + arguments.front()};
    const CommandResult r = runCommand(c, 280);
    if (r.end != CommandResult::End::Exited || r.code != 0) {
        std::fprintf(stderr, "mock_e2e: nexus-mock %s failed (%d): %s%s\n", arguments.front().c_str(), r.code,
            r.out.c_str(), r.err.c_str());
        return false;
    }
    return true;
}

void printLog(const std::string& dir) {
    const std::string log = readFile(dir + "/server/server.log");
    if (!log.empty())
        std::printf("--- server log, %s ---\n%s------------------\n", dir.c_str(), log.c_str());
}

// How many of the entries in the session's tail are of `kind` and hold `text`.
std::size_t count(const sync::SessionView& v, EntryKind kind, const std::string& text) {
    std::size_t n = 0;
    for (const sync::EntryRef& e : v.entries)
        n += e->kind == kind && e->text.find(text) != std::string::npos;
    return n;
}

void fiveAndSignIn(const std::string& mock, const std::string& dir) {
    const bool started = run(mock, {"start", "--dir", dir, "--ais", "5", "--sign-in", "--instant"});
    CHECK(started);
    if (!started)
        return;
    std::string command = readFile(dir + "/server-command");
    while (!command.empty() && command.back() == '\n')
        command.pop_back();
    CHECK(command.find(" new --dir " + dir + "/server") != std::string::npos);
    CHECK(command.find("HOME=" + dir + "/home ") != std::string::npos);

    mock::Client c;
    std::string why;
    if (!c.connect(command, "mock-e2e", &why)) {
        std::fprintf(stderr, "mock_e2e: %s\n", why.c_str());
        ++failures;
        return;
    }
    const std::vector<std::string> five = {"Ada", "Grace", "Linus", "Margaret", "Alan"};
    // A window ensures a session for each AI it shows; the mock's seeder already has, so these change nothing.
    for (const std::string& name : five) {
        sync::Action a;
        a.kind = sync::ActionKind::Ensure;
        a.friendName = name;
        c.session->add(a);
    }
    const bool arrived = c.until([&] {
        if (!c.session->state().friendsKnown() || !c.session->allAcknowledged())
            return false;
        for (const std::string& name : five)
            if (c.session->state().first(name) == nullptr)
                return false;
        return true;
    }, kStallSeconds, &why);
    CHECK(arrived);
    if (!arrived) {
        std::fprintf(stderr, "mock_e2e: %s\n", why.c_str());
        return;
    }

    // The AIs, in order, and only Ida with an account, which needs a sign-in.
    const sync::Sessions& state = c.session->state();
    std::vector<std::string> listed;
    for (const sync::ServerFriend& f : state.friends()) {
        listed.push_back(f.name);
        if (f.name == mock::kSignInName) {
            CHECK(f.accounts.size() == 1 && f.accounts[0].name == mock::kSignInAccount && f.accounts[0].needsSignIn);
        } else {
            CHECK(f.accounts.empty());
        }
    }
    CHECK((listed == std::vector<std::string>{"Ada", "Grace", "Linus", "Margaret", "Alan", "Ida"}));

    // Each AI's seeded conversation, as a client is sent it (the newest entries: sync::Tail): idle, ending with the last
    // seeded reply, each of its last four prompts in once, and the turns its order ends with.
    for (std::size_t i = 0; i < five.size(); ++i) {
        const sync::SessionView* v = state.first(five[i]);
        CHECK(v != nullptr);
        if (v == nullptr)
            continue;
        const std::vector<mock::SeedTurn> seed = mock::seedFor(five[i], i);
        CHECK(v->activity == sync::Activity::Idle && v->queue.empty());
        CHECK(!v->entries.empty() && v->entries.back()->kind == EntryKind::Friend &&
            v->entries.back()->text == seed.back().reply);
        for (std::size_t t = seed.size() - 4; t < seed.size(); ++t)
            CHECK(count(*v, EntryKind::Person, seed[t].prompt) == 1);
        if (i % 2 == 0) {
            CHECK(count(*v, EntryKind::Friend, "```cpp") == 1);
            CHECK(count(*v, EntryKind::Friend, "## Release checklist") == 1);
            CHECK(count(*v, EntryKind::Friend, "This week is about making the server boring") == 1);
            CHECK(count(*v, EntryKind::Tool, "ctest --test-dir build") == 1);
            CHECK(count(*v, EntryKind::Friend, "One test fails") == 1);
        } else {
            CHECK(count(*v, EntryKind::Friend, "# Design: the journal's second generation") == 1);
            CHECK(count(*v, EntryKind::Friend, "Deploying to the bench now.") == 1);
            CHECK(count(*v, EntryKind::Notice, "a scripted error") == 1);
            CHECK(count(*v, EntryKind::Friend, "three restarts of the server") == 1);
            CHECK(count(*v, EntryKind::Notice, "cut short") == 1);
        }
        CHECK(v->total() >= 2 * seed.size());
        std::printf("mock_e2e: %s holds %llu entries, %zu of them sent\n", five[i].c_str(),
            static_cast<unsigned long long>(v->total()), v->entries.size());
    }

    // A new prompt to Ada: the first scripted reply, exactly once.
    const sync::SessionView* ada = state.first("Ada");
    CHECK(ada != nullptr);
    if (ada == nullptr)
        return;
    const sync::SessionId adaId = ada->id;
    const std::uint64_t before = ada->total();
    sync::Action send;
    send.kind = sync::ActionKind::Send;
    send.session = adaId;
    send.text = "A prompt from the mock's test";
    c.session->add(send);
    const auto answered = [&] {
        const sync::SessionView* v = c.session->state().find(adaId);
        return v != nullptr && c.session->allAcknowledged() && v->activity == sync::Activity::Idle &&
            v->total() >= before + 2 && v->entries.back()->closed;
    };
    CHECK(c.until(answered, kStallSeconds, &why));
    // Then a second's worth of rounds more, in which nothing else may arrive.
    const auto quiet = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < quiet && c.step(&why)) {
    }
    const sync::SessionView* v = c.session->state().find(adaId);
    CHECK(v != nullptr);
    if (v != nullptr) {
        CHECK(v->total() == before + 2);
        CHECK(count(*v, EntryKind::Person, "A prompt from the mock's test") == 1);
        CHECK(count(*v, EntryKind::Friend, mock::firstReplyText()) == 1);
        CHECK(v->entries.back()->kind == EntryKind::Friend && v->entries.back()->text == mock::firstReplyText());
    }
    c.close();
}

void none(const std::string& mock, const std::string& dir) {
    const bool started = run(mock, {"start", "--dir", dir, "--ais", "0", "--instant"});
    CHECK(started);
    if (!started)
        return;
    std::string command = readFile(dir + "/server-command");
    while (!command.empty() && command.back() == '\n')
        command.pop_back();
    mock::Client c;
    std::string why;
    if (!c.connect(command, "mock-e2e-none", &why)) {
        std::fprintf(stderr, "mock_e2e: %s\n", why.c_str());
        ++failures;
        return;
    }
    CHECK(c.until([&] { return c.session->state().friendsKnown(); }, kStallSeconds, &why));
    CHECK(c.session->state().friends().empty() && c.session->state().sessions().empty());
    c.close();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mock_e2e <nexus-mock> <dir>\n");
        return 2;
    }
    const std::string mock = argv[1];
    const std::string dir = argv[2];
    // A directory an earlier run left (a crash, a timeout) is the mock's own, and start removes it.
    fiveAndSignIn(mock, dir);
    if (failures != 0)
        printLog(dir);
    const int before = failures;
    none(mock, dir);
    if (failures != before)
        printLog(dir);
    CHECK(run(mock, {"stop", "--dir", dir}));
    CHECK(!std::filesystem::exists(dir));
    if (failures != 0) {
        std::fprintf(stderr, "mock_e2e: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mock_e2e: ok\n");
    return 0;
}
