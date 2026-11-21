// Functional test of the tab order's, the selected tab's, the text scale's and the terminals' file stores
// (src/tab_order.h), and of the window's home and config (src/client_home.h): real files, inside this test's working
// directory, with HOME set to a directory there.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "pr/file.h"

#include "client_home.h"
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

#if !defined(_WIN32)
// The mode bits of `path`, or -1.
int modeOf(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 ? int(st.st_mode & 0777) : -1;
}

// The writer's thread makes a wrapped store's writes: the newest of those waiting, every one made by flush and by the
// writer's end, and a failure logged rather than reported to the caller, who has moved on.
void writerWritesTheNewest() {
    const std::string path = "writer-test/nexus/order.txt";
    {
        StoreWriter writer;
        TabOrderStore store = writer.wrap(fileTabOrderStore(path));
        std::string error;
        CHECK(store.write(formatTabOrder({"a"}), &error));
        CHECK(store.write(formatTabOrder({"b"}), &error));
        writer.flush();
        std::string text;
        CHECK(store.read(&text, &error));
        std::vector<std::string> names;
        CHECK(parseTabOrder(text, &names, &error) && names == std::vector<std::string>{"b"});
        CHECK(store.write(formatTabOrder({"c"}), &error));
    }
    std::string text;
    std::string error;
    CHECK(fileTabOrderStore(path).read(&text, &error));
    std::vector<std::string> names;
    CHECK(parseTabOrder(text, &names, &error) && names == std::vector<std::string>{"c"});
    StoreWriter writer;
    TabOrderStore broken = writer.wrap(fileTabOrderStore("/dev/null/nexus/order.txt"));
    CHECK(broken.write(formatTabOrder({"a"}), &error));     // handed over; the failure is logged on the writer's thread
    writer.flush();
}

// The window's home under a HOME inside this test's working directory: the stores' files are in ~/.nexus/client, which
// the first write makes, mode 0700 (and ~/.nexus above it); the config is read from there, and no config is the
// defaults.
void clientHomeFiles() {
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof cwd) != nullptr);
    const std::string home = std::string(cwd) + "/home-test";
    const std::string top = home + "/.nexus";
    const std::string dir = top + "/client";
    for (const char* f : {"tab-order.txt", "selected-tab.txt", "text-scale.txt", "terminals.txt", "config"}) {
        const std::string path = dir + "/" + f;
        prFileRemove(PrStringView{path.data(), path.size()});
    }
    rmdir(dir.c_str());
    rmdir(top.c_str());
    CHECK(mkdir(home.c_str(), 0755) == 0 || modeOf(home) != -1);
    CHECK(setenv("HOME", home.c_str(), 1) == 0);
    std::string got;
    std::string why;
    CHECK(clientHome(&got, &why) && got == dir);
    CHECK(fileTabOrderStore().where == dir + "/tab-order.txt");
    CHECK(fileSelectedTabStore().where == dir + "/selected-tab.txt");
    CHECK(fileTextScaleStore().where == dir + "/text-scale.txt");
    CHECK(fileTerminalsStore().where == dir + "/terminals.txt");
    CHECK(modeOf(top) == -1);
    std::string error;
    CHECK(saveTextScale(fileTextScaleStore(), 3, &error));
    CHECK(modeOf(top) == 0700 && modeOf(dir) == 0700);
    std::string warning;
    CHECK(loadTextScale(fileTextScaleStore(), -5, 10, &warning) == 3 && warning.empty());
    // A home made by someone else with a looser mode is set to 0700.
    CHECK(chmod(top.c_str(), 0755) == 0 && makeClientHome(dir, &why) && modeOf(top) == 0700);
    // No config: the defaults. A config: its settings, and its problems named by file and line.
    ClientConfig c = readClientConfig(dir);
    CHECK(c.path == dir + "/config" && c.novaConfig.empty() && c.sprintUrl.empty() && c.problems.empty());
    const std::string text = "nova-config /opt/nova-config\nsprint-url http://h:1/\nwho knows\n";
    const std::string path = dir + "/config";
    CHECK(prFileWrite(PrStringView{path.data(), path.size()},
        PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
    c = readClientConfig(dir);
    CHECK(c.novaConfig == "/opt/nova-config" && c.sprintUrl == "http://h:1/");
    CHECK(c.problems.size() == 1 && c.problems[0].find(path + " line 3:") != std::string::npos);
    // No HOME: the stores say so and write nothing.
    CHECK(unsetenv("HOME") == 0);
    TabOrderStore none = fileTabOrderStore();
    CHECK(none.where.empty() && !none.write("x", &error) && error.find("HOME is not set") != std::string::npos);
    CHECK(setenv("HOME", home.c_str(), 1) == 0);
}
#endif

} // namespace

// Two instances of one extension, terminals in the same directory, and late roster delivery all retain their
// identity. The same snapshot respects a fork which disallows the prior cross-group move.
static void mixedLayout() {
    const SavedLayout wanted{"extension:8", {
        {"sprint", "dashboards", ""}, {"extension:4", "friends", "notes"}, {"friend:ada", "friends", ""},
        {"terminal:2", "friends", "/same"}, {"terminal:3", "terminals", "/same"}, {"extension:8", "writing", "notes"}}};
    SavedLayout read;
    CHECK(parseLayout(formatLayout(wanted), &read) && read == wanted);
    const auto path = fileTabOrderStore("mixed-layout-test/tab-layout.json");
    std::string error, text;
    CHECK(path.write(formatLayout(wanted), &error) && path.read(&text, &error) && parseLayout(text, &read));
    CHECK(!parseLayout(R"({"version":1,"selected":"","tabs":[["sprint","dashboards",""],["sprint","friends",""]]})", &read));
    CHECK(!parseLayout(R"({"version":1,"selected":"extension:7","tabs":[]})", &read));
    const auto extension = [](const std::string& type, std::string* group, std::string* title) {
        if (type != "notes") return false;
        *group = "writing";
        *title = "Notes";
        return true;
    };
    for (bool crossing : {true, false}) {
        Model model({}, true);
        model.set_tab_groups({{"dashboards", {"sprint"}}, {"friends", {"friend"}, false,
            crossing ? std::vector<std::string>{"terminals", "writing"} : std::vector<std::string>{}},
            {"terminals", {"terminal"}, true}, {"writing", {"notes"}, true}});
        TabLayout layout(read);
        const auto shells = layout.reopenTerminals(model);
        CHECK(shells.size() == 2 && shells[0].second == "/same" && shells[1].second == "/same");
        layout.restore(model, extension, false);
        const auto focused = model.tab(model.active()).id;
        CHECK(model.tab(model.active()).kind == TabKind::Extension);
        layout.capture(model, {"/same", "/same"}, false);
        CHECK(layout.saved().selected == "extension:8");
        model.add_friend({"ada", {}});
        layout.restore(model, extension, true);
        CHECK(model.tab(model.active()).id == focused);
        layout.capture(model, {"/same", "/same"}, true);
        if (crossing) {
            CHECK(layout.saved() == wanted);
        } else {
            CHECK(model.tab(1).name == "ada" && model.tab(2).kind == TabKind::Terminal);
            CHECK(model.tab(2).group == "terminals" && model.tab(4).group == "writing");
        }
        CHECK(model.close_tab(model.active()));
        layout.restore(model, extension, true);
        CHECK(model.tab_count() == 5);
        layout.capture(model, {"/same", "/same"}, true);
        CHECK(layout.saved().tabs.size() == 5);
    }
    // A plugin may become available after launch. Respect a focus choice made while waiting for it.
    TabLayout delayed(wanted);
    Model partial({}, true);
    delayed.reopenTerminals(partial);
    delayed.restore(partial, [](const auto&, auto*, auto*) { return false; }, false);
    delayed.capture(partial, {"/same", "/same"}, false);
    CHECK(delayed.saved().selected == "extension:8");
    CHECK(partial.select_tab(0));
    delayed.personSelected();
    delayed.restore(partial, extension, true);
    CHECK(partial.tab(partial.active()).kind == TabKind::Sprint);
    delayed.capture(partial, {"/same", "/same"}, true);
    CHECK(delayed.saved().selected == "sprint" && delayed.saved().tabs.size() == 5);
    SavedLayout friendFocus = wanted;
    friendFocus.selected = "friend:ada";
    TabLayout pending(friendFocus);
    Model waiting({}, true);
    pending.reopenTerminals(waiting);
    pending.restore(waiting, extension, false);
    pending.capture(waiting, {"/same", "/same"}, false);
    CHECK(pending.saved().selected == "friend:ada");
    waiting.add_friend({"ada", {}});
    pending.restore(waiting, extension, true);
    CHECK(waiting.tab(waiting.active()).name == "ada");
}

int main() {
    mixedLayout();
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
#if !defined(_WIN32)
    writerWritesTheNewest();
    clientHomeFiles();
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("tab order file functional test passed\n");
    return 0;
}
