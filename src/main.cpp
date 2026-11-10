// nexus: one person and several AIs ("friends") in one window. Layer 1 is the window only: tabs, transcripts and the
// prompt, with nothing connected to any AI yet.
//
// main() is the composition root: it reads the friends, makes the model, and either runs the window as a Prax Studio
// application of one Studio window (pr/studio_app.h, pr/studio_window.h) or lays the same tree out offscreen and writes
// one frame to a PNG (--screenshot).
//
// The window is the overlay (platform_mac.h): see-through, over the whole screen, shown and hidden by a global hotkey
// (style.h), and hidden by Escape. --windowed gives a normal opaque window, where Escape does nothing. --print-window-config makes the overlay window hidden, prints its
// configuration and exits.
//
// Command-T opens a terminal tab (terminals.h) running the login shell in the home directory; --demo-terminal makes the
// screenshot show one after a command has printed.
//
// The first tab is always the sprint tab: nova-sprint's served board (board.h), at the address in the sprint's record
// (sprint.h) or NEXUS_SPRINT_URL. The window opens on the tab selected when it last quit (tab_order.h), or the sprint
// tab if that tab is gone, was a terminal, or nothing is saved; --tab N opens tab N, and a screenshot without --tab
// shows the sprint tab. --backdrop draws the screenshot see-through,
// as the overlay is, over coloured stripes in place of the screen behind it.
//
// --server HOST makes the friend tabs the server's: the window runs `nexus-server new` on HOST over ssh and keeps in
// step with it over mosh's transport (link/client.h). A prompt goes to the server, and the transcripts and queues show
// what the server holds (sync/mirror.h). Until the server is first heard there is no prompt, only the line that says
// the window is connecting (or why it could not); afterwards that line shows only while the server is not heard.
//
// Usage: nexus [--windowed] [--tab N] [--demo] [--demo-terminal] [--screenshot PATH.png [--backdrop]]
//              [--bench FRAMES] [--print-window-config] [--server HOST]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pr/compress.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/image.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/layout_scene.h"
#include "pr/studio_app.h"
#include "pr/studio_window.h"
#include "pr/text.h"

#include "board.h"
#include "friends.h"
#include "model.h"
#include "platform_mac.h"
#include "style.h"
#include "tab_order.h"
#include "terminals.h"
#include "view.h"

#if NEXUS_LINK
#include "link/client.h"
#include "sync/client.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#endif

namespace {

using namespace nexus;

// Frame times from the Studio window's trace, in milliseconds.
struct FrameStats {
    unsigned frames = 0;
    double total = 0;       // the whole frame, swap chain wait included
    double work = 0;        // the frame less the swap chain wait: rounds, scene, play and render
    double worstTotal = 0;
    double worstWork = 0;
};

struct App {
    Model* model = nullptr;
    View view;
    Look look;
    PrFontSet* fonts = nullptr;
    bool overlay = true;            // the overlay window (platform_mac.h); --windowed gives Prax's own window
    bool overlayOpened = false;
    bool demoTerminal = false;      // --demo-terminal: the screenshot shows a terminal tab after a command
    bool backdrop = false;          // --backdrop: the screenshot is see-through over stripes
    std::unique_ptr<PraxTerminals> terminals;   // over the window's kit, from open() to close()
    std::unique_ptr<PraxBoard> board;           // the sprint tab's page, over the window's kit, from open() to close()
    unsigned bench = 0;     // with --bench, frames to draw back to back before stopping
    FrameStats stats;
    std::atomic<PrApp*> wakeApp{nullptr};   // while the window is open: for the link's thread to wake it
#if NEXUS_LINK
    std::string host;                       // --server
    std::unique_ptr<link::ClientLink> link;
    sync::Mirror mirror;
    std::string promptProblem;              // why the last prompt was not sent, until one is
#endif
};

#if NEXUS_LINK
// Each frame: the server's latest tabs into the model, and the link's state into the line where the prompt stands.
// The prompt opens once the server is first heard, and stays.
void pollLink(App& a) {
    link::ClientLink::Snapshot s;
    if (a.link == nullptr || !a.link->poll(&s))
        return;
    a.mirror.apply(*a.model, s.tabs);
    if (s.phase == sync::LinkPhase::Connected)
        a.view.promptOpen = true;
    const std::string status = sync::linkStatus(s.phase, a.host, s.heardMsAgo, s.problem);
    a.view.status = status.empty() ? a.promptProblem : status;
}
#endif

App& appOf(void* context) {
    return *static_cast<App*>(context);
}

OverlayConfig overlayConfig() {
    return OverlayConfig{
        .hotKeyCode = style::kOverlayHotKeyCode,
        .hotKeyModifiers = style::kOverlayHotKeyModifiers,
        .blur = style::kOverlayBlur,
        .accessory = style::kOverlayAccessory,
    };
}

// The window's kit theme (view.h). Its bg0 is the canvas, which the window clears to with its alpha, so the overlay's
// background is see-through and what is drawn on it is not.
bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    PrKitTheme theme = windowTheme(a.fonts, a.overlay);
    prKitSetTheme(prStudioWindowKit(window), &theme);
    a.terminals = std::make_unique<PraxTerminals>(prStudioWindowKit(window), a.fonts, prStudioWindowApp(window),
        prStudioWindowAppWindow(window), a.overlay);
    a.view.terminals = a.terminals.get();
    PraxBoard::Desc board;
    board.kit = prStudioWindowKit(window);
    board.fonts = a.fonts;
    board.app = prStudioWindowApp(window);
    board.window = prStudioWindowAppWindow(window);
    if (a.overlay) {
        board.visible = overlayVisible;
        setOverlayShown([](void* app) { prAppWake(static_cast<PrApp*>(app)); }, prStudioWindowApp(window));
    }
    a.board = std::make_unique<PraxBoard>(board);
    a.view.board = a.board.get();
    a.wakeApp = prStudioWindowApp(window);
    prAppWake(prStudioWindowApp(window));   // a link that changed before the window opened
    return true;
}

// The terminals and the board end with the window, while its kit still exists.
void closeWindow(void* context, PrStudioWindow*) {
    App& a = appOf(context);
    a.wakeApp = nullptr;
    setOverlayShown(nullptr, nullptr);
    a.view.board = nullptr;
    a.board.reset();
    a.view.terminals = nullptr;
    a.terminals.reset();
}

// The kit's own palette, the one open() and the text scale's keys set: the window gives the kit Studio's theme again
// whenever the palette this returns differs from the kit's (studio_window.cpp renderFrame 404-412), which would put
// Studio's type sizes back over the window's scaled ones.
void palette(void*, PrStudioWindow* window, const PrAppUserPreferences*, PrKitPalette* out) {
    *out = prStudioWindowKit(window)->theme.palette;
}

void beginFrame(void* context, PrStudioWindow* window, const PrAppFrame* f) {
    App& a = appOf(context);
#if NEXUS_LINK
    pollLink(a);
#endif
    beginWindowFrame(a.view, prStudioWindowKit(window), a.fonts);
    if (a.board != nullptr)
        a.board->frame(f->time, f->display);
    if (a.overlay && !a.overlayOpened) {
        // Prax's lib/app has no window styles; go through the window behind its NSView. Shown once at launch.
        a.overlayOpened = true;
        PrAppSurface surface{};
        if (prAppWindowSurface(prStudioWindowAppWindow(window), &surface) == PrStatus_Ok &&
                surface.kind == PrAppSurfaceKind_NsView && openOverlay(surface.handle, overlayConfig(), true))
            std::printf("nexus: overlay window\n%s", describeOverlay().c_str());
        else
            std::fprintf(stderr, "nexus: no NSView behind the window; the overlay is not set up\n");
    }
}

// While the sprint tab shows, its page takes the events it is routed (sprint.h routeBoardEvent); the rest go to the
// window's tree.
bool event(void* context, PrStudioWindow*, const PrAppEvent* e) {
    App& a = appOf(context);
    return a.board != nullptr && a.board->event(*e);
}

void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent) {
    App& a = appOf(context);
    a.view.extent = extent;
    buildWindow(a.view, prStudioWindowKit(window), root);
}

bool laidOut(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    if (a.bench == 0)
        return windowWantsFrame(a.view);
    if (a.stats.frames >= a.bench) {
        prAppStop(prStudioWindowApp(window));
        return false;
    }
    return true;
}

void trace(void* context, const PrStudioWindowFrameTimes* t) {
    FrameStats& s = appOf(context).stats;
    double work = t->total - t->wait;
    ++s.frames;
    s.total += t->total;
    s.work += work;
    s.worstTotal = std::max(s.worstTotal, t->total);
    s.worstWork = std::max(s.worstWork, work);
}

void printStats(const FrameStats& s) {
    if (s.frames == 0)
        return;
    std::printf("nexus: %u frames; work (rounds, layout, scene, render) %.2f ms average, %.2f ms worst; "
        "with the swap chain wait %.2f ms average, %.2f ms worst\n", s.frames, s.work / s.frames, s.worstWork,
        s.total / s.frames, s.worstTotal);
}

// The offscreen frame: the same tree, laid out and painted without a window. ---------------------------------------

struct Offscreen {
    App* app;
    PrKit* kit;
};

void buildOffscreen(void* context, PrUi*, const PrUiRound*) {
    Offscreen& off = *static_cast<Offscreen*>(context);
    buildWindow(off.app->view, off.kit, kPrLayoutNodeNone);
}

bool writePng(std::vector<uint8_t>& bytes, PrUVec2 size, const char* path) {
    // The render is premultiplied; the encoder takes straight alpha. Everything is opaque, so only alpha 0 needs care.
    for (size_t i = 0; i < bytes.size(); i += 4) {
        uint8_t alpha = bytes[i + 3];
        if (alpha != 0 && alpha != 255) {
            for (int c = 0; c < 3; ++c)
                bytes[i + c] = uint8_t(std::min(255, bytes[i + c] * 255 / alpha));
        }
    }
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* codec = compress != nullptr ? prImageCodecCreate(PrImageCodecBackend_Default, compress) : nullptr;
    if (codec == nullptr) {
        std::fprintf(stderr, "nexus: no image codec in this build\n");
        prCompressCodecDestroy(compress);
        return false;
    }
    PrPixelView view{bytes.data(), size_t(size.x) * 4, size};
    size_t encoded = 0;
    prImageCodecEncode(codec, "image/png", view, nullptr, 0, &encoded);
    std::vector<uint8_t> png(encoded);
    PrStatus status = prImageCodecEncode(codec, "image/png", view, png.data(), png.size(), &encoded);
    prImageCodecDestroy(codec);
    prCompressCodecDestroy(compress);
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: encoding the PNG failed (status %d)\n", int(status));
        return false;
    }
    if (!prFileWrite(PrStringView{path, std::strlen(path)}, PrByteView{png.data(), png.size()})) {
        std::fprintf(stderr, "nexus: could not write %s\n", path);
        return false;
    }
    std::printf("nexus: wrote %s (%zu bytes)\n", path, png.size());
    return true;
}

// Lays the window out at `extent` px for a display of `ratio`, renders it on the GPU into memory, and writes a PNG.
// A workaround: Prax has no public offscreen render of a UI tree (only its test fixture, tools/harness/kit_fixture.h).
int screenshot(App& a, const char* path, PrVec2 extent, float ratio) {
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = shaper, .targetPixelRatio = ratio};
    PrLayoutTree* tree = prLayoutTreeCreate(&treeDesc);
    // With --backdrop the window is drawn as the overlay draws it, see-through; otherwise opaque, since an offscreen
    // frame has nothing behind it to show through.
    PrKitTheme theme = windowTheme(a.fonts, a.backdrop);
    prLayoutTreeSetCanvasColor(tree, prColor16FromColor(theme.palette.bg0));
    PrUi* ui = prUiCreate(tree);
    PrKit* kit = prKitCreate(ui, &theme);
    Offscreen off{&a, kit};
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = ratio;
    a.view.extent = extent;
    auto terminals = std::make_unique<PraxTerminals>(kit, a.fonts, nullptr, nullptr, a.backdrop);
    a.view.terminals = terminals.get();
    PraxBoard::Desc boardDesc;
    boardDesc.kit = kit;
    boardDesc.fonts = a.fonts;
    auto board = std::make_unique<PraxBoard>(boardDesc);
    a.view.board = board.get();
    int frames = 0;
    auto frame = [&] {
        const double time = 1000.0 + frames++ / 60.0;
        board->frame(time, display);
        PrUiFrameDesc desc{.time = time, .delta = 1 / 60.0, .extent = extent, .maxRounds = 4,
            .display = display, .preferences = {}};
        prUiFrame(ui, &desc, buildOffscreen, &off);
    };
    if (a.model->tab_count() != 0 && a.model->tab(a.model->active()).kind == TabKind::Sprint) {
        // The board's page, once it has loaded and painted its first answers, within 20 s; or the line that says why
        // it has none.
        for (int i = 0; i < 1000 && !board->settled(); ++i) {
            frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!board->settled())
            std::fprintf(stderr, "nexus: the sprint board did not settle: %s\n", board->problem().c_str());
    }
    if (a.demoTerminal) {
        // A terminal tab running /bin/sh in the home directory, after a command has printed, within 10 s.
        terminals->program = "/bin/sh";
        const std::size_t id = a.model->add_terminal();
        if (!terminals->open(id))
            a.model->close_tab(a.model->active());
        const char* command = "echo nexus terminal tab; uname -sm\r";
        bool typed = false;
        for (int i = 0; i < 500 && terminals->text(id).find("Darwin") == std::string::npos; ++i) {
            frame();
            if (!typed && i == 10) {
                PrAppEvent e{};
                e.kind = PrAppEventKind_Text;
                e.input.text = PrStringView{command, std::strlen(command)};
                prUiPushEvent(ui, kPrUiViewportMain, &e);
                typed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    // Enough frames for every query to read a layout: the first round routes nothing, and scrolling reads the last one.
    for (int i = 0; i < 4; ++i)
        frame();
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    std::vector<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, scrolls.data(), uint32_t(scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data();
    options.scrollCount = uint32_t(scrolls.size());
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{ratio, ratio}));
    if (a.backdrop) {
        // Stripes of four colours, 160 px wide, in place of the screen behind the overlay.
        const PrColor stripes[] = {{0.93f, 0.45f, 0.18f, 1}, {0.16f, 0.62f, 0.56f, 1}, {0.86f, 0.86f, 0.82f, 1},
            {0.55f, 0.35f, 0.85f, 1}};
        for (int i = 0; float(i) * 160 < extent.x; ++i) {
            PrDrawPaint paint{};
            paint.color = prColor16FromColor(stripes[i % 4]);
            prDrawListRect(list, PrRect{float(i) * 160, 0, float(i + 1) * 160, extent.y}, paint);
        }
    }
    prLayoutScenePlay(scene, list, &options);
    prDrawListPopTransform(list);
    prLayoutSceneRelease(scene);

    int code = 1;
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: no GPU device or renderer for the screenshot\n");
    } else {
        PrUVec2 size{uint32_t(extent.x * ratio), uint32_t(extent.y * ratio)};
        std::vector<uint8_t> bytes(size_t(size.x) * size.y * 4, 0);
        PrPixels pixels{bytes.data(), size_t(size.x) * 4, size};
        PrStatus status = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8);
        if (status != PrStatus_Ok)
            std::fprintf(stderr, "nexus: rendering the screenshot failed (status %d)\n", int(status));
        else if (writePng(bytes, size, path))
            code = 0;
    }
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
    prDrawListRelease(list);
    a.view.board = nullptr;
    board.reset();
    a.view.terminals = nullptr;
    terminals.reset();
    prKitDestroy(kit);
    prUiDestroy(ui);
    prLayoutTreeDestroy(tree);
    prTextShaperDestroy(shaper);
    return code;
}

// Sample entries for --demo: every kind, and a person's message long enough to wrap.
void seedDemo(Model& model, std::size_t tab) {
    model.append(0, Entry{EntryKind::Person,
        "I'd like the transcript to keep up with a long conversation, so here is a message long enough to wrap onto a "
        "second line and show how a bubble grows when it runs past most of the column."});
    model.append(0, Entry{EntryKind::Friend,
        "Sure. The bubble is laid out by Prax, so it wraps at the column's width and keeps its corners however many "
        "lines it takes."});
    model.append(0, Entry{EntryKind::Notice, "read 3 files"});
    model.append(0, Entry{EntryKind::Friend, "Anything else before I start?"});
    model.set_draft(tab, "Can you look at why the nightly build went red?");
    model.send(tab);
    model.set_draft(tab, "After that, review the open PR");
    model.queue(tab);
}

} // namespace

int main(int argc, char** argv) {
    const char* shot = nullptr;
    std::size_t tab = 1;
    bool tabGiven = false;
    bool demo = false;
    bool demoTerminal = false;
    bool backdrop = false;
    bool windowed = false;
    bool printConfig = false;
    unsigned bench = 0;
    const char* server = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            server = argv[++i];
        } else if (std::strcmp(argv[i], "--windowed") == 0) {
            windowed = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (std::strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            tab = std::size_t(std::strtoul(argv[++i], nullptr, 10));
            tabGiven = true;
        } else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            bench = unsigned(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (std::strcmp(argv[i], "--demo-terminal") == 0) {
            demoTerminal = true;
        } else if (std::strcmp(argv[i], "--backdrop") == 0) {
            backdrop = true;
        } else if (std::strcmp(argv[i], "--print-window-config") == 0) {
            printConfig = true;
        } else {
            std::fprintf(stderr, "usage: nexus [--windowed] [--tab N] [--demo] [--demo-terminal] [--screenshot PATH.png "
                "[--backdrop]] [--bench FRAMES] [--print-window-config] [--server HOST]\n");
            return 2;
        }
    }
    if (server != nullptr) {
#if NEXUS_LINK
        if (shot != nullptr || demo || demoTerminal || bench != 0 || printConfig) {
            std::fprintf(stderr, "nexus: --server runs the live window only, without --screenshot, --demo, "
                "--demo-terminal, --bench or --print-window-config\n");
            return 2;
        }
#else
        std::fprintf(stderr, "nexus: this build has no --server (it was configured with -DNEXUS_LINK=OFF)\n");
        return 2;
#endif
    }

    if (printConfig) {
        std::string text = describeHiddenOverlay(overlayConfig());
        if (text.empty()) {
            std::fprintf(stderr, "nexus: no screen to make the overlay window on\n");
            return 1;
        }
        std::printf("nexus: overlay window (made hidden, as Prax's backend makes its window)\n%s"
            "background alpha: %.2f; tab strip fill alpha: %.2f; blur: %s\n", text.c_str(),
            double(style::kOverlayOpacity), double(style::kOverlayStripFill), style::kOverlayBlur ? "on" : "off");
        return 0;
    }

    FriendList friends = loadFriends();
    if (friends.error.empty())
        std::printf("nexus: %zu friends from nova-config\n", friends.friends.size());
    else
        std::fprintf(stderr, "nexus: %s\n", friends.error.c_str());
    // The friend tabs in the order the person last left them (tab_order.h).
    static const TabOrderStore order = fileTabOrderStore();
    std::string orderWarning;
    friends.friends = loadTabOrder(order, friends.friends, &orderWarning);
    if (!orderWarning.empty())
        std::fprintf(stderr, "nexus: %s\n", orderWarning.c_str());
    // The sprint tab first, then the friends.
    static Model model(friends.friends, true);
    // The window opens on the tab --tab names; without it, on the tab selected when nexus last quit, found by who it
    // is (tab_order.h), or the sprint tab. A screenshot without --tab shows the sprint tab, whatever was saved.
    static const TabOrderStore selection = fileSelectedTabStore();
    const bool live = shot == nullptr;
    if (!tabGiven && live) {
        std::string selectionWarning;
        tab = loadSelectedTab(selection, model, &selectionWarning) + 1;
        if (!selectionWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", selectionWarning.c_str());
    }
    if (tab < 1 || !model.select_tab(tab - 1)) {
        std::fprintf(stderr, "nexus: no tab %zu (there are %zu)\n", tab, model.tab_count());
        return 2;
    }
    if (demo && model.tab(model.active()).kind == TabKind::Friend)
        seedDemo(model, model.active());

    static App a;
    a.model = &model;
    a.view.model = &model;
    a.view.look = &a.look;
    a.view.notice = "No friends to show.\n" + friends.error;
    // The screenshot stays opaque: an offscreen frame has nothing behind it to show through.
    a.overlay = !windowed && shot == nullptr;
    a.look.overlay = a.overlay;
    // Escape hides the overlay, as the hotkey does; a normal window has nothing to hide.
    if (a.overlay)
        a.view.hide = hideOverlay;
    a.view.moved = [] {
        std::string error;
        if (!saveTabOrder(order, model, &error))
            std::fprintf(stderr, "nexus: %s\n", error.c_str());
    };
    // Each change of the selected tab is saved as it happens, so a quit of any kind leaves it saved. A screenshot saves
    // nothing.
    if (live) {
        a.view.selected = [] {
            std::string error;
            if (!saveSelectedTab(selection, model, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        };
    }
#if NEXUS_LINK
    if (server != nullptr) {
        a.host = server;
        a.view.promptOpen = false;
        a.view.status = sync::linkStatus(sync::LinkPhase::Connecting, a.host, 0, "");
        a.view.submit = [](std::size_t i, bool queue) {
            std::string text;
            std::string why;
            if (sync::takePrompt(model, i, sync::Limits{}.text, &text, &why)) {
                a.promptProblem.clear();
                a.link->send(queue ? sync::ActionKind::Queue : sync::ActionKind::Send, model.tab(i).name,
                    std::move(text));
            } else if (!why.empty()) {
                a.promptProblem = why;
                if (a.view.status.empty())
                    a.view.status = why;
            }
        };
        link::ClientLink::Options options;
        options.bootstrap.host = a.host;
        options.wake = [] {
            if (PrApp* app = a.wakeApp.load())
                prAppWake(app);
        };
        a.link = std::make_unique<link::ClientLink>(std::move(options));
    }
#endif
    a.bench = bench;
    a.demoTerminal = demoTerminal;
    a.backdrop = backdrop && shot != nullptr;
    if (a.backdrop)
        a.look.overlay = true;
    a.fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(a.fonts, system);
    prFontSetSetDefaultFamily(a.fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));

    int code = 0;
    if (shot != nullptr) {
        // A fullscreen-sized frame of the main display (a 1000x780 window with --windowed).
        float w = 1000, h = 780, scale = 2;
        if (!windowed && !mainDisplay(&w, &h, &scale)) {
            std::fprintf(stderr, "nexus: no main display to size the screenshot by\n");
            code = 1;
        } else {
            code = screenshot(a, shot, PrVec2{w, h}, scale);
        }
    } else {
        PrStudioWindowDesc window{
            .title = "nexus",
            .extent = {1000, 780},
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
        PrStudioAppDesc desc{.fonts = a.fonts, .window = &window, .lastWindow = PrStudioAppLastWindow_Stop};
        PrStatus status = prStudioAppRun(&desc);
        if (status == PrStatus_NotSupported) {
            std::fprintf(stderr, "nexus: no GPU device to render with\n");
            code = 1;
        }
        closeOverlay();
#if NEXUS_LINK
        a.link.reset();     // mosh's shutdown, so the server ends this session
#endif
        printStats(a.stats);
    }
    prFontSetDestroy(a.fonts);
    prFontSystemDestroy(system);
    return code;
}
