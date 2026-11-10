// nexus: one person and several AIs ("friends") in one window. Layer 1 is the window only: tabs, transcripts and the
// prompt, with nothing connected to any AI yet.
//
// main() is the composition root: it reads the friends, makes the model, and either runs the window as a Prax Studio
// application of one Studio window (pr/studio_app.h, pr/studio_window.h) or lays the same tree out offscreen and writes
// one frame to a PNG (--screenshot).
//
// The window is the overlay: Prax's Overlay window (a borderless panel above other apps, on every Space and beside
// fullscreen apps) with a Transparent backdrop, in an Accessory app (no Dock icon), shown and hidden by Prax's global
// hotkey (style.h kOverlayHotkey) and by Escape, and hidden when it loses the keyboard, as overlay.h's rules say.
// platform_mac.h makes it cover the whole screen, menu bar included, which Prax does not. --windowed gives a normal
// opaque window, where Escape does nothing. --print-window-config prints what the overlay asks of Prax and exits.
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
// Usage: nexus [--windowed] [--tab N] [--demo] [--demo-terminal] [--screenshot PATH.png [--backdrop]]
//              [--bench FRAMES] [--print-window-config]

#include <algorithm>
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
#include "overlay.h"
#include "platform_mac.h"
#include "style.h"
#include "tab_order.h"
#include "terminals.h"
#include "view.h"

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
    bool overlay = true;            // the overlay window; --windowed gives a normal window
    PrStudioWindow* window = nullptr;   // from open() to close(); the overlay's from start()
    const PrStudioWindowDesc* overlayWindow = nullptr;  // the overlay's window, which start() creates
    bool opened = false;            // open() ran for the window
    bool stopping = false;          // the run's shutdown has begun
    bool failed = false;            // Prax closed the overlay's window before it opened (overlayCloseFailed)
    OverlayRules rules;             // when the overlay shows and hides (overlay.h)
    bool demoTerminal = false;      // --demo-terminal: the screenshot shows a terminal tab after a command
    bool backdrop = false;          // --backdrop: the screenshot is see-through over stripes
    std::unique_ptr<PraxTerminals> terminals;   // over the window's kit, from open() to close()
    std::unique_ptr<PraxBoard> board;           // the sprint tab's page, over the window's kit, from open() to close()
    unsigned bench = 0;     // with --bench, frames to draw back to back before stopping
    FrameStats stats;
};

App& appOf(void* context) {
    return *static_cast<App*>(context);
}

// The overlay's tags: its hotkey's, and the timer of the key check after each show.
constexpr uint64_t kHotkeyTag = 1;
constexpr uint64_t kKeyCheckTag = 2;
// How long after a show the overlay must have the keyboard. Not measured: Prax gives an overlay the keyboard within
// prAppWindowFocus, so this only has to outlast the frames that report it (a guess, as the earlier code's was).
constexpr double kKeyCheckSeconds = 0.5;

PrAppWindowBackdrop overlayBackdrop() {
    return style::kOverlayBlur ? PrAppWindowBackdrop_Blur : PrAppWindowBackdrop_Transparent;
}

// Does what the overlay's rules answered. Show covers the screen with the window, shows it and gives it the keyboard
// (prAppWindowFocus shows a hidden window first, and gives an overlay the keyboard without activating nexus), then
// checks after kKeyCheckSeconds that it has the keyboard.
void carryOut(App& a, OverlayAct act) {
    if (act == OverlayAct::None || a.window == nullptr)
        return;
    PrAppWindow* window = prStudioWindowAppWindow(a.window);
    PrApp* app = prStudioWindowApp(a.window);
    if (act == OverlayAct::Hide) {
        prAppTimerCancel(app, kKeyCheckTag);
        prAppWindowSetVisible(window, false);
        return;
    }
    PrAppSurface surface{};
    if (prAppWindowSurface(window, &surface) != PrStatus_Ok || surface.kind != PrAppSurfaceKind_NsView ||
            !coverScreen(surface.handle))
        std::fprintf(stderr, "nexus: the overlay could not be given the whole screen (no NSView, window or screen), so "
            "it covers the screen's work area only, below the menu bar\n");
    prAppWindowFocus(window);
    prAppTimerSet(app, kKeyCheckTag, kKeyCheckSeconds, 0);
}

bool overlayVisible(const App& a) {
    return a.window != nullptr && prAppWindowIsVisible(prStudioWindowAppWindow(a.window));
}

bool overlayActive(const App& a) {
    return a.window != nullptr && prAppWindowIsActive(prStudioWindowAppWindow(a.window));
}

// The window's kit theme (view.h). Over the overlay's Transparent backdrop the window draws no background of its own,
// so the see-through fill is the window part's (view.cpp, style::kOverlayOpacity), which the window clears to.
bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    a.window = window;
    a.opened = true;
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
    if (a.overlay)
        board.visible = [&a] { return overlayVisible(a); };
    a.board = std::make_unique<PraxBoard>(board);
    a.view.board = a.board.get();
    return true;
}

// The terminals and the board end with the window, while its kit still exists. An overlay window that Prax closes
// before it opened could not draw (its swap chain): the run stops with it, as its last window, and fails.
void closeWindow(void* context, PrStudioWindow*) {
    App& a = appOf(context);
    if (a.overlay && overlayCloseFailed(a.opened, a.stopping)) {
        std::fprintf(stderr, "nexus: the overlay window could not open (Prax could not make its swap chain)\n");
        a.failed = true;
    }
    a.window = nullptr;
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
    beginWindowFrame(a.view, prStudioWindowKit(window), a.fonts);
    if (a.board != nullptr)
        a.board->frame(f->time, f->display);
    // A change of the window's visibility or keyboard asks for a frame, so each one is seen here.
    if (a.overlay)
        carryOut(a, a.rules.observe(overlayVisible(a), overlayActive(a)));
}

// The overlay at launch: creates its window, registers the hotkey and shows the overlay, as a press of the hotkey would.
// nexus creates the window here rather than through the app's description because a Studio window's open comes with
// its first frame and a hidden window has no frames (Prax 2908525 studio_window.h prStudioWindowCreate, app_window.h
// prAppWindowSetVisible): waiting on open() for a.window, nothing ever showed the overlay (models/Overlay.tla held,
// witnesses/WOverlayHeldAfterOpen).
bool start(void* context, PrStudioApp* studio) {
    App& a = appOf(context);
    if (!a.overlay)
        return true;
    PrStatus created = prStudioWindowCreate(studio, a.overlayWindow, &a.window);
    if (created != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: the overlay window could not be created (status %d)\n", int(created));
        return false;
    }
    PrStatus status = prAppHotkeyRegister(prStudioAppApp(studio), style::kOverlayHotkey, kHotkeyTag);
    if (status != PrStatus_Ok)
        std::fprintf(stderr, "nexus: the overlay hotkey (Command-backtick) did not register (status %d: %s), so only "
            "launching nexus shows the overlay; another app may hold the key: change kOverlayHotkey in src/style.h\n",
            int(status), status == PrStatus_NotAllowed ? "another app or the system holds it" : "no global hotkeys");
    carryOut(a, a.rules.hotkey(overlayVisible(a), overlayActive(a)));
    return true;
}

// The run stops: the windows close after this, which is no failure of theirs.
void shutdown(void* context, PrStudioApp*) {
    appOf(context).stopping = true;
}

// The app's events: the hotkey, and the key check after a show.
void appEvent(void* context, PrStudioApp*, const PrAppEvent* e) {
    App& a = appOf(context);
    if (!a.overlay)
        return;
    if (e->kind == PrAppEventKind_Hotkey && e->tag == kHotkeyTag) {
        carryOut(a, a.rules.hotkey(overlayVisible(a), overlayActive(a)));
    } else if (e->kind == PrAppEventKind_Timer && e->tag == kKeyCheckTag) {
        OverlayAct act = a.rules.keyCheck(overlayVisible(a), overlayActive(a));
        if (act == OverlayAct::Hide)
            std::fprintf(stderr, "nexus: the overlay did not get the keyboard within %.1f s of showing, so it is "
                "hidden rather than cover other apps without it; press the hotkey again\n", kKeyCheckSeconds);
        carryOut(a, act);
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

// Writes the main display's size in logical px and its pixel ratio, from a headless Prax app that never runs (so it
// shows nothing). Returns false if there is no display.
bool mainDisplay(float* width, float* height, float* ratio) {
    PrAppOptions options = prAppOptionsDefault();
    options.headless = true;
    PrApp* app = prAppCreate(PrAppBackend_Default, &options);
    if (app == nullptr)
        return false;
    PrAppDisplayInfo info{};
    const bool found = prAppDisplays(app, &info, 1) != 0;   // the main display first
    prAppDestroy(app);
    if (!found)
        return false;
    *width = info.extent.x;
    *height = info.extent.y;
    *ratio = info.display.pixelRatio;
    return true;
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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--windowed") == 0) {
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
                "[--backdrop]] [--bench FRAMES] [--print-window-config]\n");
            return 2;
        }
    }

    if (printConfig) {
        std::printf("nexus: the overlay window, as nexus asks Prax for it\n"
            "role: Overlay (a borderless panel above other apps, on every Space and beside fullscreen apps)\n"
            "backdrop: %s; opens hidden; shown at launch\n"
            "presence: %s\n"
            "hotkey: Command-backtick (PrAppKeyCode %u, modifiers 0x%x)\n"
            "frame: the whole screen with the pointer, menu bar included, set through the NSView at each show\n"
            "background alpha: %.2f; tab strip fill alpha: %.2f\n",
            style::kOverlayBlur ? "Blur" : "Transparent", style::kOverlayAccessory ? "Accessory" : "Regular",
            unsigned(style::kOverlayHotkey.code), unsigned(style::kOverlayHotkey.modifiers),
            double(style::kOverlayOpacity), double(style::kOverlayStripFill));
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
        a.view.hide = [] { carryOut(a, a.rules.escape(overlayVisible(a))); };
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
        // The overlay opens hidden at the size Prax holds it to; each show gives it the whole screen (carryOut).
        PrStudioWindowDesc window{
            .title = "nexus",
            .extent = {1000, 780},
            .role = a.overlay ? PrAppWindowRole(PrAppWindowRole_Overlay) : PrAppWindowRole(PrAppWindowRole_Normal),
            .backdrop = a.overlay ? overlayBackdrop() : PrAppWindowBackdrop(PrAppWindowBackdrop_Opaque),
            .hidden = a.overlay,
            .placement = PrAppWindowPlacement_Center,
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
        if (a.overlay && style::kOverlayAccessory)
            options.presence = PrAppPresence_Accessory;
        // start() creates the overlay's window itself, so that nexus holds it before its first frame.
        a.overlayWindow = a.overlay ? &window : nullptr;
        PrStudioAppDesc desc{.fonts = a.fonts, .options = &options, .window = a.overlay ? nullptr : &window,
            .lastWindow = PrStudioAppLastWindow_Stop, .start = start, .event = appEvent, .shutdown = shutdown,
            .context = &a};
        PrStatus status = prStudioAppRun(&desc);
        if (status == PrStatus_NotSupported)
            std::fprintf(stderr, "nexus: no GPU device to render with\n");
        else if (status != PrStatus_Ok)
            std::fprintf(stderr, "nexus: the window could not open or the run could not start (status %d)\n",
                int(status));
        if (status != PrStatus_Ok || a.failed)
            code = 1;
        printStats(a.stats);
    }
    prFontSetDestroy(a.fonts);
    prFontSystemDestroy(system);
    return code;
}
