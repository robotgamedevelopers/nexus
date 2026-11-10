// Functional test of the tab order's file store (src/tab_order.h): a real file, inside this test's working directory.

#include <cstdio>
#include <string>
#include <vector>

#include "pr/file.h"

#include "tab_order.h"

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

} // namespace

int main() {
    const std::string path = "tab-order-test/nexus/tab-order.txt";
    prFileRemove(PrStringView{path.data(), path.size()});
    TabOrderStore store = fileTabOrderStore(path);
    const std::vector<FriendInfo> loaded{{"a", "m"}, {"b", "m"}, {"c", "m"}};
    std::string warning;
    CHECK(loadTabOrder(store, loaded, &warning)[0].name == "a" && warning.empty());   // no file yet: no warning
    Model model(loaded);
    model.move_tab(0, 2);
    std::string error;
    CHECK(saveTabOrder(store, model, &error));                                         // the directory is made too
    std::vector<FriendInfo> again = loadTabOrder(store, loaded, &warning);
    CHECK(warning.empty());
    CHECK(again.size() == 3 && again[0].name == "b" && again[1].name == "c" && again[2].name == "a");
    // A write into a path whose directory cannot exist fails with the path and a cause.
    TabOrderStore bad = fileTabOrderStore("/dev/null/nexus/tab-order.txt");
    CHECK(!saveTabOrder(bad, model, &error));
    std::fprintf(stderr, "  expected failure: %s\n", error.c_str());
    CHECK(error.find("/dev/null/nexus/tab-order.txt") != std::string::npos);
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("tab order file functional test passed\n");
    return 0;
}
