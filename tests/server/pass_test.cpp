// The end of a pass of the server's loop (src/server/pass.h): the flush, then the release and the state, never before
// it, and neither after a flush that failed. Logic only.

#include "server/pass.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

// What the pass did, in order, with the flush succeeding or not.
std::vector<std::string> run(bool flushes, bool changed, bool* ended) {
    std::vector<std::string> done;
    const nexus::server::PassEnd end{
        [&](std::string* why) {
            done.push_back("flush");
            if (!flushes) {
                *why = "the disk is gone";
            }
            return flushes;
        },
        [&] { done.push_back("release"); },
        [&] { done.push_back("publish"); }};
    std::string why;
    *ended = nexus::server::endPass(end, changed, &why);
    if (!flushes) {
        CHECK(why == "the disk is gone");
    }
    return done;
}

} // namespace

int main() {
    bool ended = false;
    CHECK((run(true, true, &ended) == std::vector<std::string>{"flush", "release", "publish"}));
    CHECK(ended);
    // Nothing changed: the drivers are still released (a driver's own step may be in the batch), no state is set.
    CHECK((run(true, false, &ended) == std::vector<std::string>{"flush", "release"}));
    CHECK(ended);
    // A failed flush: nothing of the batch goes out.
    CHECK((run(false, true, &ended) == std::vector<std::string>{"flush"}));
    CHECK(!ended);
    if (failures != 0) {
        std::fprintf(stderr, "pass_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pass_test: ok\n");
    return 0;
}
