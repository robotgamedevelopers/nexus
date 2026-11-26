// Writes the state format 1 fixtures, main-state-1.init and main-state-1.grow, with the sessions code of a nexus from
// before formats (main at 50565c6), so that sync_test checks this build against bytes that build wrote, not bytes this
// one claims it would. Not part of the build: it compiles against that tree's sources only.
//
//   git worktree add <tree> 50565c6
//   c++ -std=c++20 -I<tree>/src tests/sync/fixtures/main_state_fixture.cpp <tree>/src/sync/sessions.cpp \
//       <tree>/src/sync/wire.cpp -o main_state_fixture
//   ./main_state_fixture tests/sync/fixtures
//
// The states (sync_test's state_format_1_from_main builds the same ones): ada's session 1, of every setting, with a
// prompt, a reply, a notice, an open reply and a queued prompt; bo's session 2 with one prompt. .init is the first
// diff, from the empty state; .grow is the diff after ada's open reply grew.

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "sync/sessions.h"

using namespace nexus;
using namespace nexus::sync;

namespace {

EntryRef entry(EntryKind kind, std::string text, bool closed = true) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, std::move(text), closed});
}

Sessions state(const std::string& open) {
    const std::vector<EntryRef> ada = {entry(EntryKind::Person, "Why did the nightly go red?"),
        entry(EntryKind::AI, "Looking at the log."), entry(EntryKind::Notice, "Stopped.")};
    const std::vector<EntryRef> bo = {entry(EntryKind::Person, "hi")};
    SessionView a = tailOf(1, SessionInfo{"ada", "claude", "work", "opus", "high"}, ada, {"after that, the PR"}, Tail{},
        entry(EntryKind::AI, open, false));
    SessionView b = tailOf(2, SessionInfo{"bo", "claude", "", "", ""}, bo, {}, Tail{});
    return Sessions({std::make_shared<const SessionView>(a), std::make_shared<const SessionView>(b)});
}

bool write(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    return static_cast<bool>(out.flush());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: main_state_fixture <dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const Sessions before = state("The nightly");
    const Sessions after = state("The nightly went red in the link walk");
    if (!write(dir + "/main-state-1.init", before.init_diff()) ||
            !write(dir + "/main-state-1.grow", after.diff_from(before))) {
        std::fprintf(stderr, "main_state_fixture: cannot write to %s\n", dir.c_str());
        return 1;
    }
    return 0;
}
