// Functional test of the tab order's, the selected tab's, the text scale's and the terminals' file stores
// (src/tab_order.h): real files, inside this test's working directory.

#include <cstdint>
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
    // The text scale, in a file beside them: nothing saved is the default; a saved scale comes back; a file that is not
    // a text scale warns and gives the default.
    const std::string scalePath = "tab-order-test/nexus/text-scale.txt";
    prFileRemove(PrStringView{scalePath.data(), scalePath.size()});
    TabOrderStore scale = fileTabOrderStore(scalePath);
    CHECK(loadTextScale(scale, -5, 10, &warning) == 0 && warning.empty());
    CHECK(saveTextScale(scale, 4, &error) && saveTextScale(scale, -2, &error));
    CHECK(loadTextScale(scale, -5, 10, &warning) == -2 && warning.empty());
    const std::string garbage = "not a scale\n";
    CHECK(prFileWrite(PrStringView{scalePath.data(), scalePath.size()},
        PrByteView{reinterpret_cast<const uint8_t*>(garbage.data()), garbage.size()}));
    CHECK(loadTextScale(scale, -5, 10, &warning) == 0 && warning.find(scalePath) != std::string::npos);
    // The terminal tabs and a selected terminal, in files beside them: nothing saved reopens none; saved terminals come
    // back in order, and the selected one by its place among them.
    const std::string terminalsPath = "tab-order-test/nexus/terminals.txt";
    prFileRemove(PrStringView{terminalsPath.data(), terminalsPath.size()});
    TabOrderStore terminals = fileTabOrderStore(terminalsPath);
    CHECK(loadTerminals(terminals, &warning).empty() && warning.empty());
    Model open(loaded, true);
    open.add_terminal();
    open.add_terminal();
    CHECK(saveTerminals(terminals, {"/tmp", ""}, &error) && saveSelectedTab(selected, open, &error));
    const std::vector<std::string> reopened = loadTerminals(terminals, &warning);
    CHECK(reopened == (std::vector<std::string>{"/tmp", ""}) && warning.empty());
    Model back(loaded, true);
    for (std::size_t k = 0; k < reopened.size(); ++k)
        back.add_terminal();
    CHECK(back.tab_count() == 6);
    CHECK(loadSelectedTab(selected, back, &warning) == 5 && warning.empty());
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("tab order file functional test passed\n");
    return 0;
}
