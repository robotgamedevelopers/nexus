// Functional test of a terminal tab: the window's view over a Prax UI context with no window and no GPU, with the real
// terminal (terminals.h) running /bin/sh on a pseudoterminal in a directory under the test's working directory. It
// opens a tab with Command-T, types a command that prints a marker, waits for the marker on the screen, types exit,
// and waits for the tab to close by itself. Then a relaunch's round trip: a terminal saved with its shell's directory,
// to a file under the test's working directory, comes back there. Then a tab in ssh, with a stand-in ssh (fake_ssh.cpp)
// first on the PATH that connects nowhere: the tab names the fleet machine while it runs, whatever title it sets, and
// has its usual label back after it exits. Real processes, files and time (bounded waits): a functional test.

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "pr/file.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/layout_tree.h"
#include "pr/raw_string.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "model.h"
#include "tab_order.h"
#include "style.h"
#include "terminals.h"
#include "view.h"

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

constexpr auto kDeadline = std::chrono::seconds(20);

struct Harness {
    Model model{std::vector<FriendInfo>{FriendInfo{"alex", "m1"}}};
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    PraxTerminals* terminals = nullptr;
    Look look;
    View view;
    double time = 1000;

    Harness() {
        prFontSetSetSystem(fonts, system);
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        PrKitTheme theme = windowTheme(fonts, false);
        kit = prKitCreate(ui, &theme);
        terminals = new PraxTerminals(kit, fonts, nullptr, nullptr);
        view.model = &model;
        view.look = &look;
        view.terminals = terminals;
        view.extent = PrVec2{1000, 780};
        frame();
    }

    ~Harness() {
        delete terminals;
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    // As the app's frame: each terminal's foreground process is read at the frame's start (main.cpp beginFrame).
    void frame() {
        terminals->observe(time);
        terminals->deliver(ui);
        beginWindowFrame(view, kit, fonts);
        time += 1.0 / 60;
        PrUiFrameDesc desc{.time = time, .delta = 1.0 / 60, .extent = PrVec2{1000, 780}, .maxRounds = 8,
            .display = prAppDisplayDefault(), .preferences = {}};
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Harness& h = *static_cast<Harness*>(context);
            buildWindow(h.view, h.kit, kPrLayoutNodeNone);
        }, this);
    }

    void push(PrAppEvent e) {
        e.time = time;
        const auto& active = model.tab(model.active());
        if (active.kind != TabKind::Terminal || !terminals->hold(active.id, e))
            prUiPushEvent(ui, kPrUiViewportMain, &e);
    }

    void key(PrAppKey k, PrAppKeyCode code, PrAppModifierFlags modifiers = 0) {
        const std::initializer_list<PrAppEventKind> kinds{PrAppEventKind_KeyDown, PrAppEventKind_KeyUp};
        for (PrAppEventKind kind : kinds) {
            PrAppEvent e{};
            e.kind = kind;
            e.modifiers = modifiers;
            e.keyboard.key = k;
            e.keyboard.code = code;
            push(e);
        }
        frame();
    }

    // Types `line` as text, then Enter.
    void typeLine(const std::string& line) {
        PrAppEvent e{};
        e.kind = PrAppEventKind_Text;
        e.input.text = PrStringView{line.data(), line.size()};
        push(e);
        frame();
        key(PrAppKey_Enter, PrAppKeyCode_Enter);
    }

    // Runs frames until `done` or the deadline. Returns whether `done` became true.
    template <class F>
    bool until(F done) {
        const auto end = std::chrono::steady_clock::now() + kDeadline;
        while (!done()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }
};

void aShellRunsAndItsTabClosesWhenItExits() {
    const std::filesystem::path dir = std::filesystem::absolute("terminal-run");
    std::filesystem::create_directories(dir);
    Harness h;
    h.terminals->program = "/bin/sh";
    h.terminals->directory = dir.string();
    h.key(PrAppKey('T'), PrAppKeyCode_KeyT, style::kCommand);
    CHECK(h.model.tab_count() == 2 && h.model.active() == 1);
    if (h.model.tab_count() != 2)
        return;
    const std::size_t id = h.model.tab(1).id;
    CHECK(h.model.tab(1).kind == TabKind::Terminal);
    // The shell's first prompt, then a command whose output differs from its echo.
    CHECK(h.until([&] { return h.terminals->text(id).find('$') != std::string::npos; }));
    h.typeLine("echo mark-$((40+2))-$(basename \"$(pwd)\")");
    const bool printed = h.until([&] { return h.terminals->text(id).find("mark-42-terminal-run") != std::string::npos; });
    if (!printed)
        std::fprintf(stderr, "  screen:\n%s\n", h.terminals->text(id).c_str());
    CHECK(printed);
    // Escape goes to the shell, which starts a key sequence with it: Control-C then drops the line.
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    h.key(PrAppKey('C'), PrAppKeyCode_KeyC, PrAppModifierFlag_Control);
    CHECK(h.model.tab_count() == 2);
    h.typeLine("exit");
    const bool closed = h.until([&] { return h.model.tab_count() == 1; });
    if (!closed)
        std::fprintf(stderr, "  screen:\n%s\n", h.terminals->text(id).c_str());
    CHECK(closed);
    CHECK(h.model.active() == 0 && h.model.tab(0).kind == TabKind::Friend);
    CHECK(h.terminals->text(id).empty());   // destroyed
}

// A relaunch's round trip: a shell changes directory, its terminal is saved with the directory it is in (to a real file
// under the test's working directory), and a new terminal opened from that file starts there. A saved directory that
// is gone starts the shell in the home directory.
void aTerminalComesBackInItsDirectory() {
    const std::filesystem::path dir = std::filesystem::absolute("terminal-keep");
    std::filesystem::create_directories(dir / "sub");
    std::filesystem::remove_all(dir / "gone");
    const std::string file = (dir / "terminals.txt").string();
    std::filesystem::remove(file);
    {
        Harness h;
        h.terminals->program = "/bin/sh";
        const std::size_t id = h.model.add_terminal();
        CHECK(h.terminals->openIn(id, dir.string()));
        CHECK(h.until([&] { return h.terminals->text(id).find('$') != std::string::npos; }));
        h.typeLine("cd sub && echo moved-$((40+2))");
        const bool moved = h.until([&] { return h.terminals->text(id).find("moved-42") != std::string::npos; });
        if (!moved) std::fprintf(stderr, "  screen after cd: %s\n", h.terminals->text(id).c_str());
        CHECK(moved);
        const std::string sub = std::filesystem::canonical(dir / "sub").string();
        const bool there = h.until([&] {
            const std::string now = h.terminals->workingDirectory(id);
            return !now.empty() && std::filesystem::exists(now) && std::filesystem::canonical(now).string() == sub;
        });
        if (!there)
            std::fprintf(stderr, "  working directory: %s\n", h.terminals->workingDirectory(id).c_str());
        CHECK(there);
        std::string error;
        CHECK(saveTerminals(fileTabOrderStore(file), terminalDirectories(h.model,
            [&](std::size_t t) { return h.terminals->workingDirectory(t); }), &error));
    }
    std::string warning;
    std::vector<std::string> saved = loadTerminals(fileTabOrderStore(file), &warning);
    CHECK(saved.size() == 1 && warning.empty());
    if (saved.size() != 1)
        return;
    saved.push_back((dir / "gone").string());
    Harness h;
    h.terminals->program = "/bin/sh";
    const std::size_t first = h.model.add_terminal();
    const std::size_t second = h.model.add_terminal();
    CHECK(h.terminals->openIn(first, saved[0]) && h.terminals->openIn(second, saved[1]));
    CHECK(h.model.select_tab(0));
    CHECK(h.until([&] {
        return !h.terminals->title(first).empty() && !h.terminals->title(second).empty() &&
            h.terminals->text(first).find('$') != std::string::npos &&
            h.terminals->text(second).find('$') != std::string::npos;
    }));
    CHECK(h.model.select_tab(2));
    PrRawString home = prFileUserDirectory(PrFileUserDirectory_Home);
    PrStringView hv = prRawStringView(&home);
    const std::string homeName = std::filesystem::path(std::string(hv.data, hv.size)).filename().string();
    prRawStringReset(&home);
    h.typeLine("echo at-$(basename \"$(pwd)\")");
    const bool inHome = h.until([&] { return h.terminals->text(second).find("at-" + homeName) != std::string::npos; });
    if (!inHome)
        std::fprintf(stderr, "  screen:\n%s\n", h.terminals->text(second).c_str());
    CHECK(inHome);
    CHECK(h.model.select_tab(1) && h.model.tab(1).id == first);
    CHECK(h.until([&] { return h.terminals->text(first).find('$') != std::string::npos; }));
    h.typeLine("echo at-$(basename \"$(pwd)\")");
    const bool inSub = h.until([&] { return h.terminals->text(first).find("at-sub") != std::string::npos; });
    if (!inSub)
        std::fprintf(stderr, "  screen:\n%s\n", h.terminals->text(first).c_str());
    CHECK(inSub);
}

// Shows the tab's label and the screen when a wait for the label failed.
void showLabel(Harness& h, std::size_t id) {
    std::fprintf(stderr, "  label %s, title %s, screen:\n%s\n", h.terminals->title(id).c_str(),
        h.terminals->programTitle(id).c_str(), h.terminals->text(id).c_str());
}

// The owner's case: Command-T, `ssh space`, and the tab reads "space" for the whole session, while the remote side sets
// titles of its own (OSC 0 and 2); after exit it has its usual label back, though the remote's last title is still set.
void aTabInSshNamesTheMachine() {
    Harness h;
    h.terminals->program = "/bin/sh";
    h.terminals->machines = {Machine{"space", "100.64.0.9"}, Machine{"hetzner2", ""}};
    h.key(PrAppKey('T'), PrAppKeyCode_KeyT, style::kCommand);
    CHECK(h.model.tab_count() == 2);
    if (h.model.tab_count() != 2)
        return;
    const std::size_t id = h.model.tab(1).id;
    CHECK(h.until([&] { return h.terminals->text(id).find('$') != std::string::npos; }));
    const std::string usual = h.terminals->title(id);
    CHECK(!usual.empty() && usual != "space");
    h.typeLine("ssh space");
    const bool named = h.until([&] {
        return h.terminals->text(id).find("fake-ssh-connected space") != std::string::npos &&
            h.terminals->title(id) == "space";
    });
    if (!named)
        showLabel(h, id);
    CHECK(named);
    // While working in space: each line sets the remote title, and the tab still reads "space".
    for (const char* line : {"vim notes.txt", "htop"}) {
        h.typeLine(line);
        const std::string ran = std::string("ran-") + line;
        CHECK(h.until([&] {
            return h.terminals->text(id).find(ran) != std::string::npos && h.terminals->programTitle(id) == line;
        }));
        h.frame();
        CHECK(h.terminals->title(id) == "space");
    }
    h.typeLine("exit");
    // The text, the label and the program's title are read at different moments, and the title may come in a later
    // frame than the text: all three are waited for together.
    const bool back = h.until([&] {
        return h.terminals->text(id).find("Connection to remote closed.") != std::string::npos &&
            h.terminals->title(id) == usual && h.terminals->programTitle(id) == "nova@remote: left behind";
    });
    if (!back) {
        showLabel(h, id);
    }
    CHECK(back);
    // A fleet machine by its tailnet name, with options; then a host that is not one, shown as given. Each time the
    // usual label comes back.
    const std::pair<const char*, const char*> sessions[] = {
        {"ssh -p 22 nova@hetzner2.tailbd3740.ts.net", "hetzner2"},
        {"ssh -o BatchMode=yes glenn@unknown.example", "unknown.example"},
        {"ssh -o BatchMode=yes nova@fast.example", "fast.example"},
    };
    for (const auto& [command, label] : sessions) {
        h.typeLine(command);
        const bool in = h.until([&] { return h.terminals->title(id) == label; });
        if (!in)
            showLabel(h, id);
        CHECK(in);
        h.typeLine("exit");
        const bool out = h.until([&] {
            return h.terminals->title(id) == usual && h.terminals->programTitle(id) == "nova@remote: left behind";
        });
        if (!out)
            showLabel(h, id);
        CHECK(out);
    }
}

} // namespace

int main(int argc, char** argv) {
    // The stand-in ssh's directory, first on the PATH that the terminals' shells inherit.
    if (argc != 2) {
        std::fprintf(stderr, "usage: nexus_terminal_tests <directory of the stand-in ssh>\n");
        return 2;
    }
    const char* path = std::getenv("PATH");
#if defined(_WIN32)
    _putenv_s("PATH", (std::string(argv[1]) + ";" + (path != nullptr ? path : "")).c_str());
#else
    setenv("PATH", (std::string(argv[1]) + ":" + (path != nullptr ? path : "/usr/bin:/bin")).c_str(), 1);
#endif
    aShellRunsAndItsTabClosesWhenItExits();
    aTerminalComesBackInItsDirectory();
    aTabInSshNamesTheMachine();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("terminal functional test passed\n");
    return 0;
}
