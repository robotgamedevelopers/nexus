// Functional test of a terminal tab: the window's view over a Prax UI context with no window and no GPU, with the real
// terminal (terminals.h) running /bin/sh on a pseudoterminal in a directory under the test's working directory. It
// opens a tab with Command-T, types a command that prints a marker, waits for the marker on the screen, types exit,
// and waits for the tab to close by itself. Then a relaunch's round trip: a terminal saved with its shell's directory,
// to a file under the test's working directory, comes back there. Real processes, files and time (bounded waits): a
// functional test.

#include <chrono>
#include <cstdio>
#include <filesystem>
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
        terminals = new PraxTerminals(kit, fonts, nullptr, nullptr, false);
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

    void frame() {
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
        prUiPushEvent(ui, kPrUiViewportMain, &e);
    }

    void key(PrAppKey k, PrAppKeyCode code, PrAppModifierFlags modifiers = 0) {
        for (PrAppEventKind kind : {PrAppEventKind_KeyDown, PrAppEventKind_KeyUp}) {
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
    h.key(PrAppKey('T'), PrAppKeyCode_KeyT, PrAppModifierFlag_Super);
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
        CHECK(h.until([&] { return h.terminals->text(id).find("moved-42") != std::string::npos; }));
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
    CHECK(h.until([&] { return h.terminals->text(second).find('$') != std::string::npos; }));
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

} // namespace

int main() {
    aShellRunsAndItsTabClosesWhenItExits();
    aTerminalComesBackInItsDirectory();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("terminal functional test passed\n");
    return 0;
}
