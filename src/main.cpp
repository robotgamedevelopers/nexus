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
// platform.h makes it cover the whole screen, menu bar included, which Prax does not. --windowed gives a normal
// opaque window, where Escape does nothing. --print-window-config prints what the overlay asks of Prax and exits.
//
// Command-T opens a terminal tab (terminals.h) running the login shell in the home directory; --demo-terminal makes the
// screenshot show one after a command has printed. The terminal tabs open at quit come back at launch, in their order,
// each a new login shell in the directory its shell last worked in (tab_order.h).
//
// The first tab is always the sprint tab: nova-sprint's served board (board.h), at the address in the sprint's record
// (sprint.h) or the client config's sprint-url (client_home.h). The window opens on the tab selected when it last quit (tab_order.h), or the sprint
// tab if that tab is gone or nothing is saved; --tab N opens tab N, and a screenshot without --tab shows the sprint
// tab. The text scale (Command-plus and Command-minus) is saved as it changes and comes back at launch.
// --backdrop draws the screenshot see-through,
// as the overlay is, over coloured stripes in place of the screen behind it.
//
// --server HOST makes the friend tabs show the server's sessions: the window runs `nexus-server new` on HOST over ssh,
// asks for a session with each friend (the server makes one for a friend that has none), and keeps in step with it over
// mosh's transport (link/client.h). Each friend tab shows its friend's first session, the window's own choice. A prompt
// goes to that session, and the transcripts and queues show what the server holds (sync/mirror.h). Until the server is
// first heard there is no prompt, only the line that says the window is connecting (or why it could not); afterwards
// nothing is drawn around the prompt, and the state cursor under the last text says what the server's state is
// (cursor.h, client.h cursorOf): busy, idle, down, or grey while the window has lost the server. The friend's open
// entry is drawn as it grows, tool calls as one compact line each, and Command-Escape sends a stop the server decides.
//
// --state FILE draws the screenshot's friend tabs from a server's state file (sync/sessions.h stateFile; the friends
// layer's functional test writes them from the fake driver: tests/server/drivers_functional.cpp, case window), as the
// window shows it once the server is heard; --lost draws it as the window does once it has lost the server. It draws
// from the file alone: no home, no config, no friends from nova-config, no saved tab order, and no sprint board; the
// friend tabs are the friends of the state's sessions, in order of session id.
//
// Usage: nexus [--windowed] [--tab N] [--demo] [--demo-terminal] [--screenshot PATH.png [--backdrop]
//              [--state FILE [--lost]]] [--bench FRAMES] [--print-window-config] [--server HOST]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pr/app_data.h"
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
#include "pr/time.h"
#include "pr/ui.h"

#include "board.h"
#include "client_home.h"
#include "font_prefetch.h"
#include "frame_host.h"
#include "friends.h"
#include "hitch.h"
#include "model.h"
#include "overlay.h"
#include "platform.h"
#include "style.h"
#include "tab_order.h"
#include "terminals.h"
#include "view.h"

#if NEXUS_LINK
#include "link/client_port.h"
#include "sync/client.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#include "sync/wire.h"
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
    std::vector<Machine> machines;  // the fleet's machines, which name a terminal tab in ssh to one (remote.h)
    std::unique_ptr<PraxBoard> board;           // the sprint tab's page, over the window's kit, from open() to close()
    std::function<SprintBoardAddress()> findBoard; // the board's address; unset, the sprint's (board.h)
    unsigned bench = 0;     // with --bench, frames to draw back to back before stopping
    FrameStats stats;
    // Kept across launches (tab_order.h); a screenshot keeps nothing. The terminal tabs reopened at launch, by Tab::id,
    // with the directory each starts in, are opened with the window.
    bool keep = false;
    const TabOrderStore* terminalStore = nullptr;
    const TabOrderStore* selectionStore = nullptr;
    std::vector<std::pair<std::size_t, std::string>> reopen;
    std::atomic<PrApp*> wakeApp{nullptr};   // while the window is open: for the link's thread to wake it
    // What the window does around the view each frame (frame_host.h): the kept preferences, the link, the terminals
    // and the board; and the hitch detector, always on, measuring each frame against the display's budget.
    FrameHost host;
    HitchDetector hitch{HitchClock{[](void*) { return prTimeMonotonicMilliseconds(); }, nullptr},
        HitchLog{[](void*, const char* line) { std::fprintf(stderr, "%s\n", line); }, nullptr}};
#if NEXUS_LINK
    std::unique_ptr<link::ClientPort> link;
#endif
};

// The hitch budget from the main display's refresh rate (120 Hz on ProMotion), or 120 Hz when Prax gives none.
void setBudget(App& a, PrApp* app) {
    PrAppDisplayInfo info{};
    a.hitch.setRefreshRate(app != nullptr && prAppDisplays(app, &info, 1) != 0 ? double(info.refreshRate) : 0);
}

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
    if (prAppWindowSurface(window, &surface) != PrStatus_Ok || !coverScreen(surface))
        std::fprintf(stderr, "nexus: the overlay could not be given the whole screen (no native view, window or "
            "screen), so it covers the screen's work area only, inside the menu bar or the taskbar\n");
    prAppWindowFocus(window);
    prAppTimerSet(app, kKeyCheckTag, kKeyCheckSeconds, 0);
}

bool overlayVisible(const App& a) {
    return a.window != nullptr && prAppWindowIsVisible(prStudioWindowAppWindow(a.window));
}

bool overlayActive(const App& a) {
    return a.window != nullptr && prAppWindowIsActive(prStudioWindowAppWindow(a.window));
}

// The clipboard's text for the prompt (View::paste), on the app's thread: a Paste event through the host, which cuts it
// to the room the prompt has left, and otherwise to the focused control as Prax delivers its own paste.
void pasted(void* context, PrStatus status, PrStringView text) {
    App& a = appOf(context);
    if (status != PrStatus_Ok || text.size == 0 || a.host.ui == nullptr) {
        return;
    }
    PrAppEvent e{};
    e.kind = PrAppEventKind_Paste;
    e.input.text = text;
    if (!a.host.event(e)) {
        prUiPushEvent(a.host.ui, kPrUiViewportMain, &e);
    }
    if (a.window != nullptr) {
        prAppWindowRequestFrame(prStudioWindowAppWindow(a.window));
    }
}

// The window's kit theme (view.h). Over the overlay's Transparent backdrop the window draws no background of its own,
// so the see-through fill is the window part's (view.cpp, style::kOverlayOpacity), which the window clears to.
bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    a.window = window;
    a.opened = true;
    PrKitTheme theme = windowTheme(a.fonts, a.overlay);
    prKitSetTheme(prStudioWindowKit(window), &theme);
    // The faces of the window's text fonts made in the Studio app's shaper now, before the first frame (font_prefetch.h):
    // made on the first frame, they were most of its work.
    warmShaper(prStudioWindowShaper(window), a.fonts, a.overlay);
    a.terminals = std::make_unique<PraxTerminals>(prStudioWindowKit(window), a.fonts, prStudioWindowApp(window),
        prStudioWindowAppWindow(window), a.overlay);
    a.terminals->machines = a.machines;
    a.view.terminals = a.terminals.get();
    // The terminal tabs kept from the last launch, each a new shell in its directory; a tab whose shell cannot start
    // closes, as one opened with Command-T does.
    for (const auto& [id, dir] : a.reopen) {
        if (a.terminals->openIn(id, dir))
            continue;
        for (std::size_t i = 0; i < a.model->tab_count(); ++i) {
            if (a.model->tab(i).id == id) {
                a.model->close_tab(i);
                break;
            }
        }
    }
    a.reopen.clear();
    PraxBoard::Desc board;
    board.kit = prStudioWindowKit(window);
    board.fonts = a.fonts;
    board.app = prStudioWindowApp(window);
    board.window = prStudioWindowAppWindow(window);
    if (a.overlay)
        board.visible = [&a] { return overlayVisible(a); };
    a.board = std::make_unique<PraxBoard>(board);
    a.view.board = a.board.get();
    a.host.terminals = a.terminals.get();
    a.host.board = a.board.get();
    a.host.ui = prStudioWindowUi(window);
    // Command-V in the prompt: the clipboard's text comes back through the host, which cuts it to the prompt's room.
    a.view.paste = [&a] {
        if (a.window != nullptr) {
            prAppDataReadText(prStudioWindowApp(a.window), kPrAppDataSourceClipboard, pasted, &a);
        }
    };
    setBudget(a, prStudioWindowApp(window));
    a.wakeApp = prStudioWindowApp(window);
    prAppWake(prStudioWindowApp(window));   // a link that changed before the window opened
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
    // The directories as they are now, not as last read (terminals.h reads one a frame, each once a second).
    if (a.terminals != nullptr) {
        a.terminals->readDirectories();
    }
    a.host.keepTerminals();
    a.wakeApp = nullptr;
    a.window = nullptr;
    a.view.paste = nullptr;
    a.host.ui = nullptr;
    a.host.board = nullptr;
    a.host.terminals = nullptr;
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
    // The frame's work starts here, after the swap chain's wait; it ends in trace(), once the frame is rendered.
    a.hitch.begin();
    a.host.beginFrame(prStudioWindowKit(window), a.fonts, f->time, f->display);
    // A change of the window's visibility or keyboard asks for a frame, so each one is seen here.
    if (a.overlay)
        carryOut(a, a.rules.observe(overlayVisible(a), overlayActive(a)));
}

// The overlay at launch: creates its window, registers the hotkey, and leaves the overlay hidden.
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
    // Sized to the screen now, while hidden, so that the first show (carryOut) finds the window at the size it shows at
    // and its first frame makes the swap chain once, at that size, rather than once and again after a resize.
    PrAppSurface surface{};
    if (prAppWindowSurface(prStudioWindowAppWindow(a.window), &surface) == PrStatus_Ok) {
        coverScreen(surface);
    }
    PrStatus status = prAppHotkeyRegister(prStudioAppApp(studio), style::kOverlayHotkey, kHotkeyTag);
    if (status != PrStatus_Ok) {
        char line[512];
        std::snprintf(line, sizeof line, "nexus: the overlay hotkey (%s-backtick) did not register (status %d: %s), so "
            "only launching nexus shows the overlay; another app may hold the key: change kOverlayHotkey in src/style.h",
            style::kCommandName, int(status),
            status == PrStatus_NotAllowed ? "another app or the system holds it" : "no global hotkeys");
        std::fprintf(stderr, "%s\n", line);
        // The one launch failure the person must be told: on Windows nexus has no terminal, so a message box says it.
        showLaunchError(line);
    }
    // Launch shows nothing and takes no focus (the owner 2026-11-10: "I want nothing visible no change, no input focus
    // change ... until the hotkey"); the link connects meanwhile. Only with no hotkey does launch show it, else never.
    if (status != PrStatus_Ok)
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
    HitchScope scope(&a.hitch, Phase::Input);
    if (e->kind == PrAppEventKind_DisplaysChanged && a.window != nullptr)
        setBudget(a, prStudioWindowApp(a.window));
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
    return appOf(context).host.event(*e);
}

void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent) {
    App& a = appOf(context);
    a.view.extent = extent;
    a.host.build(prStudioWindowKit(window), root);
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

// The frame's end: Prax's own phases (studio_window.cpp renderFrame) into the hitch detector, which ends the frame.
// Prax's wait (the swap chain's) is not work; the rounds are the layout, the scene and its play the draw, and the
// render the present.
void trace(void* context, const PrStudioWindowFrameTimes* t) {
    HitchDetector& hitch = appOf(context).hitch;
    if (hitch.inFrame()) {
        hitch.add(Phase::Layout, t->build);
        hitch.add(Phase::Draw, t->scene + t->play);
        hitch.add(Phase::Present, t->render);
        hitch.end();
    }
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
    boardDesc.find = a.findBoard;
    auto board = std::make_unique<PraxBoard>(boardDesc);
    a.view.board = board.get();
    int frames = 0;
    auto frame = [&] {
        const double time = 1000.0 + frames++ / 60.0;
        a.view.time = time;
        board->frame(time, display);
        board->wait();      // a screenshot waits for the page's thread, so that it shows the page as of this frame
        PrUiFrameDesc desc{.time = time, .delta = 1 / 60.0, .extent = extent, .maxRounds = 4,
            .display = display, .preferences = {}};
        prUiFrame(ui, &desc, buildOffscreen, &off);
        board->wait();
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
            // Once the shell has started (on the terminals' thread) and drawn its prompt.
            if (!typed && i >= 10 && terminals->text(id).find_first_not_of(" \n") != std::string::npos) {
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
    const char* server = nullptr;
    const char* state = nullptr;
    bool lost = false;
    bool usage = false;
    for (int i = 1; i < argc && !usage; ++i) {
        if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            server = argv[++i];
        } else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state = argv[++i];
        } else if (std::strcmp(argv[i], "--lost") == 0) {
            lost = true;
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
            usage = true;
        }
    }
    // The window's home, ~/.nexus/client (client_home.h), made mode 0700 if missing. On Windows the window has no
    // terminal: its error lines go to nexus.log there (src/platform.h). Then its config, whose problems are logged
    // (clientConfig also says when there is no home to read it from). A screenshot of a state file reads neither.
    if (state == nullptr) {
        std::string home;
        std::string homeWhy;
        const bool haveHome = clientHome(&home, &homeWhy);
        const bool madeHome = haveHome && makeClientHome(home, &homeWhy);
        startErrorLog(madeHome ? home : std::string());
        if (haveHome && !madeHome) {
            std::fprintf(stderr, "nexus: %s\n", homeWhy.c_str());
        }
        // No dialog for it: launch shows nothing but a hotkey that failed (AGENTS.md). With no home, Windows has no
        // log.
        clientConfig();
    }
    if (usage) {
        std::fprintf(stderr, "usage: nexus [--windowed] [--tab N] [--demo] [--demo-terminal] [--screenshot PATH.png "
            "[--backdrop] [--state FILE [--lost]]] [--bench FRAMES] [--print-window-config] [--server HOST]\n");
        return 2;
    }
    if ((state != nullptr && (shot == nullptr || server != nullptr || demo)) || (lost && state == nullptr)) {
        std::fprintf(stderr, "nexus: --state draws a screenshot only, without --server or --demo; --lost needs --state\n");
        return 2;
    }
#if !NEXUS_LINK
    if (state != nullptr) {
        std::fprintf(stderr, "nexus: this build has no --state (it was configured with -DNEXUS_LINK=OFF)\n");
        return 2;
    }
#endif
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
        std::printf("nexus: the overlay window, as nexus asks Prax for it\n"
            "role: Overlay (a borderless panel above other apps, on every Space and beside fullscreen apps)\n"
            "backdrop: %s; opens hidden; shown by the hotkey\n"
            "presence: %s\n"
            "hotkey: %s-backtick (PrAppKeyCode %u, modifiers 0x%x)\n"
            "frame: the whole screen with the pointer, menu bar or taskbar included, set through the native window at "
            "each show\n"
            "background alpha: %.2f; tab strip fill alpha: %.2f\n",
            style::kOverlayBlur ? "Blur" : "Transparent", style::kOverlayAccessory ? "Accessory" : "Regular",
            style::kCommandName, unsigned(style::kOverlayHotkey.code), unsigned(style::kOverlayHotkey.modifiers),
            double(style::kOverlayOpacity), double(style::kOverlayStripFill));
        return 0;
    }

    // The friends: from nova-config, or, for a screenshot of a state file, the friends of its sessions in order of id.
    const bool fromState = state != nullptr;
    FriendList friends;
#if NEXUS_LINK
    sync::Sessions held;
    if (fromState) {
        std::ifstream in(state, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "nexus: could not read %s\n", state);
            return 2;
        }
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string why;
        if (!sync::readStateFile(bytes, &held, &why)) {
            std::fprintf(stderr, "nexus: %s: %s\n", state, why.c_str());
            return 2;
        }
        for (const sync::SessionRef& s : held.sessions()) {
            const std::string& name = s->info.friendName;
            if (std::none_of(friends.friends.begin(), friends.friends.end(),
                             [&](const FriendInfo& f) { return f.name == name; })) {
                friends.friends.push_back(FriendInfo{name, s->info.model});
            }
        }
    }
#endif
    if (!fromState) {
        friends = loadFriends();
        if (friends.error.empty())
            std::printf("nexus: %zu friends from nova-config\n", friends.friends.size());
        else
            std::fprintf(stderr, "nexus: %s\n", friends.error.c_str());
    }
    // The fleet's machines, by which a terminal tab in ssh is named. A screenshot runs no ssh.
    MachineList machines;
    if (shot == nullptr) {
        machines = loadMachines();
        if (machines.error.empty())
            std::printf("nexus: %zu machines from nova-config\n", machines.machines.size());
        else
            std::fprintf(stderr, "nexus: %s\n", machines.error.c_str());
    }
    // The friend tabs in the order the person last left them (tab_order.h). The stores of what the window keeps across
    // launches are each a file in its home, which a screenshot of a state file never reads. They are read here, before
    // the window shows, and written by the writer's thread, so that no frame waits on the disk; the writer makes every
    // waiting write before nexus exits.
    static StoreWriter writer;
    static TabOrderStore order;
    static TabOrderStore terminalStore;
    static TabOrderStore selection;
    static TabOrderStore scale;
    if (!fromState) {
        order = writer.wrap(fileTabOrderStore());
        terminalStore = writer.wrap(fileTerminalsStore());
        selection = writer.wrap(fileSelectedTabStore());
        scale = writer.wrap(fileTextScaleStore());
        std::string orderWarning;
        friends.friends = loadTabOrder(order, friends.friends, &orderWarning);
        if (!orderWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", orderWarning.c_str());
    }
    // The sprint tab first, then the friends.
    static Model model(friends.friends, true);
    // The terminal tabs open when nexus last quit, after the friends in the order they stood, each to start a new shell
    // in its directory when the window opens (tab_order.h). A screenshot reopens none.
    const bool live = shot == nullptr;
    std::vector<std::pair<std::size_t, std::string>> reopen;
    if (live) {
        std::string terminalsWarning;
        for (const std::string& dir : loadTerminals(terminalStore, &terminalsWarning))
            reopen.emplace_back(model.add_terminal(), dir);
        if (!terminalsWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", terminalsWarning.c_str());
    }
    // The window opens on the tab --tab names; without it, on the tab selected when nexus last quit, found by who it
    // is or, for a terminal, by its place among the terminals (tab_order.h), or the sprint tab. A screenshot without
    // --tab shows the sprint tab, whatever was saved.
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
    if (!fromState) {
        a.view.moved = [] {
            std::string error;
            if (!saveTabOrder(order, model, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        };
    }
    // Each change of the selected tab, and of the terminal tabs, is saved as it happens, so a quit of any kind leaves
    // them saved. A screenshot saves nothing.
    if (live) {
        a.host.keep = true;
        a.host.terminalStore = &terminalStore;
        a.host.selectionStore = &selection;
        a.reopen = std::move(reopen);
        a.view.selected = [] { a.host.keepSelection(); };
    }
    // The text scale the person last chose, applied at the first frame, and each change saved as it is applied. A
    // screenshot draws at the default scale and saves nothing.
    if (live) {
        std::string scaleWarning;
        a.view.askedTextSteps = loadTextScale(scale, -style::kTextStepsDown, style::kTextStepsUp, &scaleWarning);
        if (!scaleWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", scaleWarning.c_str());
        a.view.scaled = [] {
            std::string error;
            if (!saveTextScale(scale, a.look.textSteps, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        };
    }
    a.host.model = &model;
    a.host.view = &a.view;
    a.host.hitch = &a.hitch;
#if NEXUS_LINK
    if (server != nullptr) {
        link::ClientLink::Options options;
        options.bootstrap.host = server;
        options.wake = [] {
            if (PrApp* app = a.wakeApp.load())
                prAppWake(app);
        };
        a.link = std::make_unique<link::ClientPort>(std::move(options));
        a.host.connect(a.link.get(), server);
    }
    // --state: the server's state from a file, as the window holds it once the server is heard (or, with --lost, once
    // it has lost the server).
    if (fromState) {
        a.host.showState(held, lost);
        a.findBoard = [] { return SprintBoardAddress{"", "a screenshot of a state file shows no sprint board"}; };
    }
#endif
    a.bench = bench;
    a.machines = std::move(machines.machines);
    a.demoTerminal = demoTerminal;
    a.backdrop = backdrop && shot != nullptr;
    if (a.backdrop)
        a.look.overlay = true;
    a.fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(a.fonts, system);
    prFontSetSetDefaultFamily(a.fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
    // The fallback fonts (emoji, CJK, ...) loaded on a thread of their own while nothing shows, before a frame needs
    // them; a screenshot loads them as it sets its text.
    std::unique_ptr<FontPrefetch> prefetch;
    if (shot == nullptr)
        prefetch = std::make_unique<FontPrefetch>(a.fonts);

    int code = 0;
    if (shot != nullptr) {
        // A fullscreen-sized frame of the main display (a 1000x780 window with --windowed).
        float w = 1000, h = 780, ratio = 2;
        if (!windowed && !mainDisplay(&w, &h, &ratio)) {
            std::fprintf(stderr, "nexus: no main display to size the screenshot by\n");
            code = 1;
        } else {
            code = screenshot(a, shot, PrVec2{w, h}, ratio);
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
            // Frames come from Prax's display link at the display's own rate (120 Hz on a ProMotion or XDR display)
            // and only when one is asked for, so the idle overlay draws nothing. The swap chain stays SDR: the window,
            // the board and the terminals draw only sRGB colours at or below SDR white, and Prax's HDR path presents
            // every pixel opaque (lib/draw/src/draw_display.slang convertPixel), which would cover the screen behind
            // the see-through overlay. HDR waits on that (docs/roadmap.sexp "hdr").
            .intent = PrGpuSwapChainIntent_Sdr,
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
#if NEXUS_LINK
        a.link.reset();     // mosh's shutdown, so the server ends this session
#endif
        printStats(a.stats);
    }
    prefetch.reset();
    prFontSetDestroy(a.fonts);
    prFontSystemDestroy(system);
    return code;
}
