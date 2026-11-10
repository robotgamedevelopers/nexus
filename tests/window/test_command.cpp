// Functional tests of runCommand (src/friends.h): real child processes and a real clock, so not unit tests.

#include <cstdio>
#include <string>

#include "friends.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

Command sh(const char* line) {
    return Command{"/bin/sh", {"-c", line}, line};
}

void exitStatusAndOutput() {
    CommandResult r = runCommand(sh("printf out; printf err >&2; exit 3"), 5);
    CHECK(r.end == CommandResult::End::Exited);
    CHECK(r.code == 3);
    CHECK(r.out == "out");
    CHECK(r.err == "err");
}

void missingProgram() {
    CommandResult r = runCommand(Command{"/nonexistent/nexus-test-program", {}, "x"}, 5);
    CHECK(r.end == CommandResult::End::NotFound || r.end == CommandResult::End::NotStarted);
}

void timeout() {
    CommandResult r = runCommand(sh("exec sleep 30"), 1);
    CHECK(r.end == CommandResult::End::TimedOut);
}

} // namespace

int main() {
    exitStatusAndOutput();
    missingProgram();
    timeout();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("command functional tests passed\n");
    return 0;
}
