// Functional test of the tab order's and the selected tab's file stores (src/tab_order.h): real files, inside this
// test's working directory.

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
    // The selected tab, in a file beside it: saved by name, and found again after the order changed.
    const std::string selectedPath = "tab-order-test/nexus/selected-tab.txt";
    prFileRemove(PrStringView{selectedPath.data(), selectedPath.size()});
    TabOrderStore selected = fileTabOrderStore(selectedPath);
    Model sprint(loaded, true);
    CHECK(loadSelectedTab(selected, sprint, &warning) == 0 && warning.empty());        // no file yet: the sprint tab
    CHECK(sprint.select_tab(1) && sprint.move_tab(1, 3));                              // "a" selected, then last
    CHECK(saveSelectedTab(selected, sprint, &error));
    Model relaunched(loadTabOrder(store, loaded, &warning), true);
    CHECK(relaunched.tab(3).name == "a" && loadSelectedTab(selected, relaunched, &warning) == 3 && warning.empty());
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("tab order file functional test passed\n");
    return 0;
}
