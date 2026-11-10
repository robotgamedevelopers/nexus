// Functional test of a terminal tab: the window's view over a Prax UI context with no window and no GPU, with the real
// terminal (terminals.h) running /bin/sh on a pseudoterminal in a directory under the test's working directory. It
// opens a tab with Command-T, types a command that prints a marker, waits for the marker on the screen, types exit,
// and waits for the tab to close by itself. Real processes and real time (bounded waits): a functional test.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "model.h"
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
        terminals = new PraxTerminals(kit, fonts, nullptr, false);
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
        if (!takeEvent(ui, e))
            prUiPushEvent(ui, &e);
    }

    void key(PrAppKey k, PrAppKeyCode code, PrAppModifierFlags modifiers = 0) {
        push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .key = k, .code = code});
        push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .key = k, .code = code});
        frame();
    }

    // Types `line` as text, then Enter.
    void typeLine(const std::string& line) {
        push(PrAppEvent{.kind = PrAppEventKind_Text, .text = PrStringView{line.data(), line.size()}});
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

} // namespace

int main() {
    aShellRunsAndItsTabClosesWhenItExits();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("terminal functional test passed\n");
    return 0;
}
