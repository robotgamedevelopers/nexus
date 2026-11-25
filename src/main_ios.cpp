// nexus on iOS and iPadOS: the window as a UIKit app (Prax's UIKit backend through its Studio app), filling its scene
// on iPhone and iPad. A second client of nexus-server, a thin portal as the desktop window is (AGENTS.md): it sends the
// person's input and shows the server's state. There is no overlay and no hotkey (an app's window is the system's to
// show), and what an iOS app cannot run is left out of this build (platform.h NEXUS_PROCESSES): no terminal tabs, no
// sprint tab, no attachment upload.
//
// It reaches nexus-server as the desktop window does, over mosh's transport after `nexus-server new` over ssh
// (link/client.h), with the ssh inside the app (link/inapp_ssh.h): the server is the client config's `server user@host`
// (client_home.h), or `--server user@host`.
//
// Launched with `--state FILE`, the window shows a server's state file as the desktop's `--screenshot --state` draws
// it (state_window.h), live; `--lost` with it shows it as once the server is lost.
//
// Usage (as the simulator or devicectl passes a launch's arguments): nexus [--server USER@HOST | --state FILE [--lost]]

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "pr/app_select.h"
#include "pr/app_device.h"
#include "pr/draw_select.h"
#include "pr/gpu_select.h"
#include "pr/compress_select.h"
#include "pr/image_select.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/studio_app.h"
#include "pr/studio_window.h"
#include "pr/text_font.h"
#include "pr/time.h"
#include "pr/ui.h"

#include "client_home.h"
#include "frame_host.h"
#include "hitch.h"
#include "link/client_port.h"
#include "link/inapp_ssh.h"
#include "model.h"
#include "platform.h"
#include "state_window.h"
#include "style.h"
#include "tab_order.h"
#include "url_opener.h"
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
    HitchDetector hitch{HitchClock{[](void*) { return prTimeMonotonicMilliseconds(); }, nullptr},
        HitchLog{[](void*, const char* line) { std::fprintf(stderr, "%s\n", line); }, nullptr}};
};

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
    a.urls.reset();
}

// The kit's own palette, so that Studio keeps the window's theme (main.cpp palette says why).
void palette(void*, PrStudioWindow* window, const PrAppUserPreferences*, PrKitPalette* out) {
    *out = prStudioWindowKit(window)->theme.palette;
}

void beginFrame(void* context, PrStudioWindow* window, const PrAppFrame* f) {
    App& a = appOf(context);
    a.hitch.begin();
    a.host.beginFrame(prStudioWindowKit(window), a.fonts, f->time, f->display);
}

bool event(void* context, PrStudioWindow*, const PrAppEvent* e) {
    auto& a = appOf(context);
    if (e->kind == PrAppEventKind_KeyDown || e->kind == PrAppEventKind_Text || e->kind == PrAppEventKind_PointerDown)
        a.view.lastInput = e->time;
    return a.host.event(*e);
}

void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent) {
    App& a = appOf(context);
    a.view.extent = extent;
    a.host.build(prStudioWindowKit(window), root);
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
    a.view.askedTextSteps = loadTextScale(scale, -style::kTextStepsDown, style::kTextStepsUp, &warning);
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
    static Model model({}, false);
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
    // The link, unless the window shows a state file: to --server, or the client config's server.
    std::string serverHost;
    if (state == nullptr) {
        serverHost = server != nullptr ? server : clientConfig().serverHost;
        std::string home;
        std::string why;
        if (!clientHome(&home, &why) || !makeClientHome(home, &why))
            std::fprintf(stderr, "nexus: %s\n", why.c_str());
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
        a.host.connect(a.link.get(), serverHost);
        keepHome(a, home);
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
