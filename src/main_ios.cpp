// nexus on iOS and iPadOS: the window as a UIKit app (Prax's UIKit backend through its Studio app), filling its scene
// on iPhone and iPad. A second client of nexus-server, a thin portal as the desktop window is (AGENTS.md): it sends the
// person's input and shows the server's state. There is no overlay and no hotkey (an app's window is the system's to
// show), and what an iOS app cannot run is left out of this build (platform.h NEXUS_PROCESSES): no terminal tabs and no
// attachment upload. The sprint tab shows the board at the address the server sends (board.h offer).
//
// It reaches nexus-server as the desktop window does, over mosh's transport after `nexus-server new` over ssh
// (link/client.h), with the ssh inside the app (link/inapp_ssh.h): the server is `--server user@host`, the client
// config's `server user@host` (client_home.h), or the one the person gives the window, which asks at first launch
// and keeps it (server_choice.h), with a way to change it under the link's line.
//
// Launched with `--state FILE`, the window shows a server's state file as the desktop's `--screenshot --state` draws
// it (state_window.h), live; `--lost` with it shows it as once the server is lost.
//
// Usage (as the simulator or devicectl passes a launch's arguments): nexus [--server USER@HOST | --state FILE [--lost]]

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>

#include "pr/app_select.h"
#include "pr/app_device.h"
#include "pr/draw_select.h"
#include "pr/gpu_select.h"
#include "pr/compress_select.h"
#include "pr/image_select.h"
#include "pr/font_set.h"
#include "pr/hash.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/studio_app.h"
#include "pr/studio_window.h"
#include "pr/text_font.h"
#include "pr/time.h"
#include "pr/ui.h"
#include "pr/ui_records.h"

#include "board.h"
#include "client_home.h"
#include "frame_host.h"
#include "hitch.h"
#include "link/client_port.h"
#include "link/inapp_ssh.h"
#include "model.h"
#include "platform.h"
#include "server_choice.h"
#include "state_window.h"
#include "style.h"
#include "tab_order.h"
#include "url_opener.h"
#include "remote_terminals.h"
#include "view.h"

#if NEXUS_PROCESSES
#error "src/main_ios.cpp is the iOS app's: build it for iOS only (cmake/ios.cmake)"
#endif

namespace {

using namespace nexus;

struct App {
    Model* model = nullptr;
    View view;
    Look look;
    PrFontSet* fonts = nullptr;
    PrStudioWindow* window = nullptr;
    std::unique_ptr<UrlOpener> urls;
    FrameHost host;
    std::atomic<PrApp*> wakeApp{nullptr};   // while the window is open: for the link's thread to wake it
    std::unique_ptr<link::ClientPort> link;
    bool linked = false;                    // the window reaches a server (not a state file): it has the sprint tab
    std::string home;                       // the client home, for the board's storage
    std::unique_ptr<PraxBoard> board;       // the sprint tab's page, while the window is open
    // The terminal tabs, whose shells run on the server (remote_terminals.h), while the window is open and linked.
    std::unique_ptr<RemoteTerminals> terminals;
    float keyboardCover = 0;                // px of the viewport's foot that the on-screen keyboard covers, this frame
    HitchDetector hitch{HitchClock{[](void*) { return prTimeMonotonicMilliseconds(); }, nullptr},
        HitchLog{[](void*, const char* line) { std::fprintf(stderr, "%s\n", line); }, nullptr}};
};

// The server the window asks for when nothing names one (server_choice.h), kept in its home's server.txt. The
// first one kept starts the link; a change made while linked is kept for the next launch, as the window's link is to
// one server for the life of the process (frame_host.h connect), and the dialog says so.
struct AskedServer : ServerChoice {
    TabOrderStore store;
    std::string kept;               // the server kept, or empty for none
    std::string field;              // the dialog's field
    std::string why;                // the last Enter's answer, when it was not simply taken
    bool open = false;
    bool linked = false;            // a link was started this launch
    std::function<void(const std::string&)> start;  // the first server kept: start the link to it

    bool asking() override { return open; }
    std::string_view current() override { return kept; }
    std::string_view typed() override { return field; }
    std::string_view problem() override { return why; }
    void type(std::string_view text) override {
        field = std::string(text);
        why.clear();
    }
    void submit() override {
        std::string address;
        std::string reason;
        if (!parseServerAddress(field, true, &address, &reason)) {
            why = reason;
            return;
        }
        if (address != kept && !store.write(formatServerFile(address), &reason)) {
            why = reason;
            return;
        }
        const bool changed = address != kept;
        kept = address;
        field = address;
        if (linked && changed) {
            why = "Kept. nexus connects to " + address + " the next time it opens.";
            return;
        }
        why.clear();
        open = false;
        if (!linked && start) {
            linked = true;
            start(address);
        }
    }
    void change() override {
        field = kept;
        why.clear();
        open = true;
    }
    void cancel() override {
        if (!kept.empty()) open = false;
    }
};

// A token of this launch's, which names the terminals this window asks the server for (TerminalView::opener): random,
// so that two devices' windows never take each other's new terminal for their own.
std::string windowToken() {
    std::random_device random;
    char token[33];
    std::snprintf(token, sizeof token, "%08x%08x%08x%08x", random(), random(), random(), random());
    return std::string("ios-") + token;
}

App& appOf(void* context) {
    return *static_cast<App*>(context);
}

bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    a.window = window;
    a.wakeApp = prStudioWindowApp(window);
    PrKitTheme theme = windowTheme(a.fonts, false);
    prKitSetTheme(prStudioWindowKit(window), &theme);
    a.urls = std::make_unique<UrlOpener>(systemBrowser(prStudioWindowApp(window)));
    a.view.openLink = a.urls->handle();
    a.host.signIn.openUrl = a.urls->handle();
    a.host.signIn.shown();
    a.host.ui = prStudioWindowUi(window);
    a.view.copy = [&a](const std::string& text) {
        return a.window && prAppDataWriteText(prStudioWindowApp(a.window), kPrAppDataSourceClipboard,
            {text.data(), text.size()}) == PrStatus_Ok;
    };
    // The sprint tab's page, the board at the address the server sends (board.h offer): this build looks none up.
    if (a.linked) {
        PraxBoard::Desc board;
        board.kit = prStudioWindowKit(window);
        board.fonts = a.fonts;
        board.app = prStudioWindowApp(window);
        board.window = prStudioWindowAppWindow(window);
        if (!a.home.empty()) board.storageDirectory = a.home + "/dashboard-storage";
        board.initialExtent = {1000, 780};
        board.open = a.urls->handle();
        board.fromServer = true;
        a.board = std::make_unique<PraxBoard>(board);
        a.view.board = a.board.get();
        a.host.board = a.board.get();
        a.terminals = std::make_unique<RemoteTerminals>(prStudioWindowApp(window), a.fonts, windowToken());
        a.terminals->setLink(a.link.get());
        a.terminals->setKit(prStudioWindowKit(window));
        a.view.terminals = a.terminals.get();
        a.view.newTerminalButton = true;
    }
    PrAppDisplayInfo info{};
    a.hitch.setRefreshRate(prAppDisplays(prStudioWindowApp(window), &info, 1) != 0 ? double(info.refreshRate) : 0);
    return true;
}

void closeWindow(void* context, PrStudioWindow*, PrStatus) {
    App& a = appOf(context);
    a.window = nullptr;
    a.wakeApp = nullptr;
    a.view.openLink = nullptr;
    a.view.copy = nullptr;
    a.host.signIn.openUrl = nullptr;
    a.host.ui = nullptr;
    a.host.board = nullptr;
    a.view.board = nullptr;
    a.board.reset();
    a.view.terminals = nullptr;
    a.terminals.reset();
    a.urls.reset();
}

// The kit's own palette, so that Studio keeps the window's theme (main.cpp palette says why).
void palette(void*, PrStudioWindow* window, const PrAppUserPreferences*, PrKitPalette* out) {
    *out = prStudioWindowKit(window)->theme.palette;
}

// How much of the viewport's foot the on-screen keyboard covers: the viewport is the frame less its safe area
// (studio_window.h PrStudioWindowBuildFn), and the keyboard's rect is in the frame's coordinates, empty when it is
// hidden.
float keyboardCover(const PrAppFrame& f) {
    if (f.keyboard.bottom <= f.keyboard.top || f.keyboard.right <= f.keyboard.left) return 0;
    const float foot = f.extent.y - f.safeArea.bottom;
    return f.keyboard.top < foot ? foot - f.keyboard.top : 0;
}

void beginFrame(void* context, PrStudioWindow* window, const PrAppFrame* f) {
    App& a = appOf(context);
    a.hitch.begin();
    a.keyboardCover = keyboardCover(*f);
    a.host.beginFrame(prStudioWindowKit(window), a.fonts, f->time, f->display);
}

bool event(void* context, PrStudioWindow*, const PrAppEvent* e) {
    auto& a = appOf(context);
    if (e->kind == PrAppEventKind_KeyDown || e->kind == PrAppEventKind_Text || e->kind == PrAppEventKind_PointerDown)
        a.view.lastInput = e->time;
    return a.host.event(*e);
}

// The window above the on-screen keyboard: while it shows, the window is laid out in the part of the viewport it leaves,
// so that the prompt and the transcript's end stay in sight, as a desktop window shorter by that much would.
struct AboveKeyboard {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;
    float height;

    void make(const App&, PrLayoutBoxStyle& box) const {
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.display = PrLayoutDisplay_Flex;
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(height);
    }
};

void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent) {
    App& a = appOf(context);
    const float cover = std::min(a.keyboardCover, extent.y);
    if (cover <= 0) {
        a.view.extent = extent;
        a.host.build(prStudioWindowKit(window), root);
        return;
    }
    a.view.extent = PrVec2{extent.x, extent.y - cover};
    PrUi* ui = prStudioWindowUi(window);
    const PrLayoutNode above = prUiElement(ui, root, prHashString(PrStringView{"above-keyboard", 14}, 0),
        pr::uiStyle(ui, a, AboveKeyboard{a.view.extent.x, a.view.extent.y}), 0);
    a.host.build(prStudioWindowKit(window), above);
}

bool laidOut(void* context, PrStudioWindow*) {
    return windowWantsFrame(appOf(context).view);
}

void trace(void* context, const PrStudioWindowFrameTimes* t) {
    HitchDetector& hitch = appOf(context).hitch;
    if (hitch.inFrame()) {
        hitch.add(Phase::Layout, t->build);
        hitch.add(Phase::Draw, t->scene + t->play);
        hitch.add(Phase::Present, t->render);
        hitch.end();
    }
}

// The client home's files, as the desktop window reads and keeps them (main.cpp): the applied client policy
// (policy.bundle.json, docs/policy.md), the friends' saved order, the tab layout, the selected tab and the text scale.
void keepHome(App& a, const std::string& home) {
    static StoreWriter writer;
    static TabOrderStore order;
    static TabOrderStore selection;
    static TabOrderStore scale;
    static TabOrderStore layoutStore;
    if (home.empty()) return;
    a.view.application->watch(home + "/policy.bundle.json");
    order = writer.wrap(fileTabOrderStore());
    selection = writer.wrap(fileSelectedTabStore());
    scale = writer.wrap(fileTextScaleStore());
    layoutStore = writer.wrap(fileLayoutStore());
    std::string warning;
    std::vector<std::string> savedOrder = loadTabOrderNames(order, &warning);
    if (!warning.empty()) std::fprintf(stderr, "nexus: %s\n", warning.c_str());

    SavedLayout savedLayout;
    bool haveLayout = false;
    bool layoutReadable = true;
    std::string text, error;
    if (layoutStore.read(&text, &error)) {
        haveLayout = parseLayout(text, &savedLayout);
        if (!haveLayout) error = "unrecognized or malformed layout";
    }
    if (!error.empty()) {
        layoutReadable = false;
        std::fprintf(stderr, "nexus: %s could not be restored (%s); leaving the file in place\n",
            layoutStore.where.c_str(), error.c_str());
    }
    a.host.layout = TabLayout(std::move(savedLayout));
    a.host.layoutStore = layoutReadable ? &layoutStore : nullptr;
    a.view.restoreTabLayout = [&a] {
        if (!a.view.windowApp.layoutGeneration) return;
        a.host.groupPolicyReady = true;
        a.host.restoreLayout();
    };

    SavedTab savedTab;
    bool savedSelection = haveLayout && !a.host.layout.saved().selected.empty();
    if (!haveLayout) {
        warning.clear();
        savedTab = loadSavedTab(selection, &warning, &savedSelection);
        if (!warning.empty()) std::fprintf(stderr, "nexus: %s\n", warning.c_str());
    }
    const bool waitForSaved = savedTab.kind == TabKind::Friend;
    a.host.strip = sync::FriendStrip(std::move(savedOrder), waitForSaved ? savedTab.name : std::string(), {});
    if (savedSelection && !waitForSaved) a.host.strip.personSelected();
    a.view.moved = [&a] {
        if (!a.host.strip.taken()) return;
        std::string why;
        if (!saveTabOrder(order, *a.model, &why)) std::fprintf(stderr, "nexus: %s\n", why.c_str());
    };
    a.host.keep = true;
    a.host.selectionStore = &selection;
    a.view.selected = [&a] { a.host.personSelected(); };
    warning.clear();
    a.view.askedTextSteps = loadTextScale(scale, -style::kTextStepsDown, style::kTextStepsUp, &warning,
        style::kDefaultTextSteps);
    if (!warning.empty()) std::fprintf(stderr, "nexus: %s\n", warning.c_str());
    a.view.scaled = [&a] {
        std::string why;
        if (!saveTextScale(scale, a.look.textSteps, &why)) std::fprintf(stderr, "nexus: %s\n", why.c_str());
    };
}

} // namespace

int main(int argc, char** argv) {
    const char* state = nullptr;
    const char* server = nullptr;
    bool lost = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state = argv[++i];
        } else if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            server = argv[++i];
        } else if (std::strcmp(argv[i], "--lost") == 0) {
            lost = true;
        } else {
            std::fprintf(stderr, "usage: nexus [--server USER@HOST | --state FILE [--lost]]\n");
            return 2;
        }
    }
    if ((lost && state == nullptr) || (state != nullptr && server != nullptr)) {
        std::fprintf(stderr, "nexus: --lost needs --state, and --state shows a file without --server\n");
        return 2;
    }
    static Model model({}, state == nullptr);   // the sprint tab (the Dashboard) when linked, as on the desktop
    App a;
    a.model = &model;
    a.view.model = &model;
    a.view.look = &a.look;
    a.host.model = &model;
    a.host.view = &a.view;
    a.host.hitch = &a.hitch;
    if (state != nullptr) {
        sync::Sessions held;
        std::string why;
        if (!readStateAt(state, &held, &why) || !showHeldState(held, lost, 0, model, a.view, a.host, &why)) {
            std::fprintf(stderr, "nexus: %s\n", why.c_str());
            return 2;
        }
    }
    // The link, unless the window shows a state file: to --server, the client config's server, or the server the
    // person gave the window (server_choice.h), which it asks for when none is kept.
    static AskedServer asked;
    if (state == nullptr) {
        std::string home;
        std::string why;
        if (!clientHome(&home, &why) || !makeClientHome(home, &why))
            std::fprintf(stderr, "nexus: %s\n", why.c_str());
        auto startLink = [&a, home](const std::string& serverHost) {
            link::InAppSshOptions ssh;
            ssh.dir = home;
            if (const char* h = std::getenv("HOME"))
                ssh.publicCopy = std::string(h) + "/Documents/nexus-ssh-key.pub";
            link::ClientLink::Options options;
            options.bootstrap.host = serverHost;
            options.run = link::inAppSsh(ssh);
            options.wake = [&a] {
                if (PrApp* app = a.wakeApp.load())
                    prAppWake(app);
            };
            a.link = std::make_unique<link::ClientPort>(std::move(options));
            if (a.terminals) a.terminals->setLink(a.link.get());
            a.host.connect(a.link.get(), serverHost);
        };
        a.linked = true;
        a.home = home;
        keepHome(a, home);
        if (server != nullptr || clientConfig().serverSet) {
            startLink(server != nullptr ? std::string(server) : clientConfig().serverHost);
        } else {
            asked.store = fileServerStore(home);
            asked.kept = loadServerAddress(asked.store, true, &why);
            if (!why.empty()) std::fprintf(stderr, "nexus: %s\n", why.c_str());
            asked.start = startLink;
            a.view.serverChoice = &asked;
            if (asked.kept.empty()) {
                asked.open = true;
            } else {
                asked.linked = true;
                startLink(asked.kept);
            }
        }
    }

    a.fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(a.fonts, system);
    prFontSetSetDefaultFamily(a.fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));

    PrStudioWindowDesc window{
        .title = "nexus",
        .extent = {1000, 780},
        .role = PrAppWindowRole(PrAppWindowRole_Normal),
        .backdrop = PrAppWindowBackdrop(PrAppWindowBackdrop_Opaque),
        .intent = PrGpuSwapChainIntent_Hdr,
        .open = open,
        .palette = palette,
        .beginFrame = beginFrame,
        .event = event,
        .build = build,
        .laidOut = laidOut,
        .close = closeWindow,
        .trace = trace,
        .context = &a,
    };
    PrAppOptions options = prAppOptionsDefault();
    options.windowModel = PrAppWindowModel_Single;
    PrStudioAppDesc desc{.fonts = a.fonts, .options = &options, .window = &window,
        .lastWindow = PrStudioAppLastWindow_Stop, .context = &a};
    const PrStatus status = prStudioAppRun(&desc);
    if (status != PrStatus_Ok)
        std::fprintf(stderr, "nexus: the window could not open or the run could not start (status %d)\n", int(status));
    if (a.link) a.link->close();
    prFontSetDestroy(a.fonts);
    prFontSystemDestroy(system);
    return status == PrStatus_Ok ? 0 : 1;
}
