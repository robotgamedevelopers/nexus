#include "pr/app_select.h"
#include "pr/app_device.h"
#include "pr/gpu_select.h"
#include "pr/draw_select.h"
#include "pr/compress_select.h"
#include "pr/image_select.h"
// nexus: one person and several AIs ("friends") in one window. Layer 1 is the window only: tabs, transcripts and the
// prompt, with nothing connected to any AI yet.
//
// main() is the composition root: it makes the model, and either runs the window as a Prax Studio
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
// (sprint.h) or the client config's sprint-url (client_home.h). The friend tabs are the server's friends (frame_host.h
// takeFriends), in the order the person last left them. The window opens on the tab selected when it last quit
// (tab_order.h), or the sprint tab if that tab is gone or nothing is saved; --tab N opens tab N, and a screenshot
// without --tab shows the sprint tab. The text scale (Command-plus and Command-minus) is saved as it changes and comes
// back at launch.
// --backdrop draws the screenshot see-through,
// as the overlay is, over coloured stripes in place of the screen behind it.
//
// --server HOST gives the window its friends and their sessions: the window runs `nexus-server new` on HOST over ssh,
// takes a tab for each friend in the server's list, as friends come and go, asks for a session with each (the server
// makes one for a friend that has none), and keeps in step with it over
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
// from the file alone: no home, no config, no saved tab order, and no sprint board; the friend tabs are the state's
// friends (sync/client.h friendsOf: its list, or the friends of its sessions in order of session id).
//
// Usage: nexus [--windowed] [--tab N] [--demo] [--demo-terminal] [--screenshot PATH.png [--backdrop]
//              [--state FILE [--lost]]] [--bench FRAMES] [--print-window-config] [--server HOST]

#include "pr/png.h"
#include "pr/compress_select.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pr/app_data.h"
#include "pr/app_shell.h"
#include "pr/compress_select.h"
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
#include "remote.h"
#include "sprint.h"
#include "hitch.h"
#include "model.h"
#include "no_core.h"
#include "overlay.h"
#include "platform.h"
#include "style.h"
#include "tab_order.h"
#include "terminals.h"
#include "url_opener.h"
#include "view.h"
#include "ui_audio.h"

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
    UiAudio audio;
    Look look;
    PrFontSet* fonts = nullptr;
    bool overlay = true;            // the overlay window; --windowed gives a normal window
    PrStudioWindow* window = nullptr;   // from open() to close(); the overlay's from start()
    const PrStudioWindowDesc* overlayWindow = nullptr;  // the overlay's window, which start() creates
    bool preparingReveal = false;
    bool revealFrameReady = false;
    std::optional<uint64_t> boardRevealStep;
    bool opened = false;            // open() ran for the window
    bool stopping = false;          // the run's shutdown has begun
    bool failed = false;            // Prax closed the overlay's window before it opened (overlayCloseFailed)
    uint64_t hotkeyGeneration = 0;
    PrAppHotkey registeredHotkey{};
    OverlayRules rules;             // when the overlay shows and hides (overlay.h)
    bool demoTerminal = false;      // --demo-terminal: the screenshot shows a terminal tab after a command
    bool backdrop = false;          // --backdrop: the screenshot is see-through over stripes
    std::unique_ptr<PraxTerminals> terminals;   // over the window's kit, from open() to close()
    std::vector<Machine> machines;  // the fleet's machines, which name a terminal tab in ssh to one (remote.h)
    std::unique_ptr<PraxBoard> board;           // the sprint tab's page, over the window's kit, from open() to close()
    // The sign-in page and the board's links, opened in the system's browser off the frame thread, open() to close().
    std::unique_ptr<UrlOpener> urls;
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
constexpr uint64_t kPolicyCheckTag = 3;
// How long after a show the overlay must have the keyboard. Not measured: Prax gives an overlay the keyboard within
// prAppWindowFocus, so this only has to outlast the frames that report it (a guess, as the earlier code's was).
constexpr double kKeyCheckSeconds = 0.5;
#if NEXUS_LINK
// How long a --tab past the launch's tabs waits for the server's friends. Not measured: the link's bootstrap (ssh, then
// nexus-server's start) takes a few seconds on the fleet, and this leaves room for a slow one.
constexpr int kTabWaitSeconds = 20;
#endif

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
#ifdef __APPLE__
    if ((act == OverlayAct::Hide) == bool(prAppWindowIsVisible(window))) prioritizeOverlayTransition();
#endif
    if (act == OverlayAct::Hide) {
#ifdef __APPLE__
        stopDesktopBlur();
#endif
        a.preparingReveal = false;
        if (a.board) a.board->cancelPreparation();
        a.boardRevealStep.reset();
        a.view.accountPicker.reset();
        prAppTimerCancel(app, kKeyCheckTag);
        if (prAppWindowIsVisible(window)) a.audio.play(a.view.windowApp.surface.closeChirp);
        prAppWindowSetVisible(window, false);
        return;
    }
    PrSurface surface{};
    if (prAppWindowSurface(window, &surface) != PrStatus_Ok || !coverScreen(surface))
        std::fprintf(stderr, "nexus: the overlay could not be given the whole screen (no native view, window or "
            "screen), so it covers the screen's work area only, inside the menu bar or the taskbar\n");
    if (!prAppWindowIsVisible(window)) {
        if (a.board && a.model->tab_count() && a.model->tab(a.model->active()).kind == TabKind::Sprint)
            a.board->prepare({});
        a.preparingReveal = true;
        a.revealFrameReady = false;
        a.boardRevealStep.reset();
        prAppWindowSetOpacity(window, 0);
#ifdef __APPLE__
        startDesktopBlur(surface, a.view.windowApp.surface.overlayBlurSigma);
#endif
        prAppWindowSetVisible(window, true);
        prAppWindowRequestUnpacedFrame(window);
        a.host.signIn.shown();
        return;
    }
    if (!a.preparingReveal) {
        prAppWindowFocus(window);
        prAppTimerSet(app, kKeyCheckTag, kKeyCheckSeconds, 0);
    }
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

// Pin each asynchronous clipboard read to the tab and the focused composer that requested it.
struct ComposerPaste {
    App& app;
    size_t tab;
    struct File { std::string name, path; size_t size; };
    std::vector<File> files;
    uint32_t index = 0;
    bool active() const {
        return app.window && app.model->tab_count() && app.model->tab(app.model->active()).id == tab &&
            app.view.prompt != kPrLayoutNodeNone && prUiFocus(app.host.ui) == app.view.prompt;
    }
    void redraw() const { if (app.window) prAppWindowRequestFrame(prStudioWindowAppWindow(app.window)); }
};

void readClipboardFile(ComposerPaste* request);
void clipboardBytes(void* context, PrStatus status, PrByteView bytes) {
    std::unique_ptr<ComposerPaste> request(static_cast<ComposerPaste*>(context));
    if (!request->active()) return;
    const auto& file = request->files[request->index];
    // AppKit supplies clipboard file URLs but only supports indexed byte reads on retained drop sources.
    std::string local;
    if (status == PrStatus_NotFound && !file.path.empty()) {
        std::error_code error;
        const auto path = std::filesystem::path(std::u8string(file.path.begin(), file.path.end()));
        if (std::filesystem::is_regular_file(path, error)) {
            std::ifstream input(path, std::ios::binary);
            local.resize(file.size);
            if (input && input.read(local.data(), std::streamsize(local.size())) && input.peek() == EOF) {
                bytes = {reinterpret_cast<const uint8_t*>(local.data()), local.size()};
                status = PrStatus_Ok;
            }
        }
    }
    if (status != PrStatus_Ok || bytes.size != file.size)
        request->app.view.problem = "Could not read " + file.name + " from the clipboard.";
    else
        request->app.host.stageFile(file.name, bytes.size ? std::string(reinterpret_cast<const char*>(bytes.data), bytes.size) : std::string());
    request->redraw();
    if (++request->index < request->files.size()) readClipboardFile(request.release());
}

void readClipboardFile(ComposerPaste* request) {
    prAppDataReadFile(prStudioWindowApp(request->app.window), kPrAppDataSourceClipboard, request->index, 0,
        request->files[request->index].size, clipboardBytes, request);
}

void clipboardForms(void* context, PrStatus status, PrAppDataFormFlags forms) {
    std::unique_ptr<ComposerPaste> request(static_cast<ComposerPaste*>(context));
    if (!request->active()) return;
    auto* app = prStudioWindowApp(request->app.window);
    if (status != PrStatus_Ok) {
        request->app.view.problem = "Could not read the clipboard.";
        request->redraw();
    } else if (forms & PrAppDataFormFlag_Files) {
        prAppDataReadFiles(app, kPrAppDataSourceClipboard, [](void* ctx, PrStatus read, const PrAppDataFile* files, uint32_t count) {
            std::unique_ptr<ComposerPaste> r(static_cast<ComposerPaste*>(ctx));
            if (!r->active()) return;
            if (read != PrStatus_Ok || !count) {
                r->app.view.problem = "No files could be read from the clipboard.";
                r->redraw();
                return;
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (files[i].directory || files[i].size > kMaxAttachmentBytes) {
                    r->app.view.problem = "Paste individual files up to 32 MiB each.";
                    r->redraw();
                    return;
                }
                r->files.push_back({std::string(files[i].name.data, files[i].name.size),
                    files[i].path.size ? std::string(files[i].path.data, files[i].path.size) : std::string(), size_t(files[i].size)});
            }
            readClipboardFile(r.release());
        }, request.release());
    } else if (forms & PrAppDataFormFlag_Image) {
        prAppDataReadImage(app, kPrAppDataSourceClipboard, [](void* ctx, PrStatus read, PrPixelView pixels) {
            std::unique_ptr<ComposerPaste> r(static_cast<ComposerPaste*>(ctx));
            if (!r->active()) return;
            std::unique_ptr<PrCompressCodecSet, decltype(&prCompressCodecSetDestroy)> codecs(
                prCompressCodecSetCreateDefault(), prCompressCodecSetDestroy);
            size_t size = 0;
            if (read == PrStatus_Ok && codecs && uint64_t(pixels.extent.x) * pixels.extent.y <= 32 * 1024 * 1024)
                size = prPngEncode(pixels, 0, codecs.get(), nullptr, 0);
            if (size && size <= kMaxAttachmentBytes) {
                std::string png(size, '\0');
                prPngEncode(pixels, 0, codecs.get(), reinterpret_cast<uint8_t*>(png.data()), size);
                r->app.host.stageFile("pasted-image.png", std::move(png));
            } else r->app.view.problem = "The clipboard image could not be read within the 32 MiB limit.";
            r->redraw();
        }, request.release());
    } else if (forms & PrAppDataFormFlag_Text) {
        prAppDataReadText(app, kPrAppDataSourceClipboard, [](void* ctx, PrStatus read, PrStringView text) {
            std::unique_ptr<ComposerPaste> r(static_cast<ComposerPaste*>(ctx));
            if (r->active()) pasted(&r->app, read, text);
        }, request.release());
    }
}

// The window's kit theme (view.h). Over the overlay's Transparent backdrop the window draws no background of its own,
// so the see-through fill is the window part's (view.cpp, style::kOverlayOpacity), which the window clears to.
bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    a.window = window;
    a.opened = true;
#ifdef __APPLE__
    PrSurface surface{};
    if (prAppWindowSurface(prStudioWindowAppWindow(window), &surface) == PrStatus_Ok)
        trackForegroundPriority(surface, a.view.hide);
#endif
    const auto power = prAppPower(prStudioWindowApp(window));
    a.view.savingPower = power.lowPower || power.source == PrAppPowerSource_Battery;
    PrKitTheme theme = windowTheme(a.fonts, a.overlay);
    prKitSetTheme(prStudioWindowKit(window), &theme);
    // The faces of the window's text fonts made in the Studio app's shaper now, before the first frame (font_prefetch.h):
    // made on the first frame, they were most of its work.
    a.view.windowShaper = prStudioWindowShaper(window);
    warmShaper(a.view.windowShaper, a.fonts, a.overlay);
    a.terminals = std::make_unique<PraxTerminals>(prStudioWindowKit(window), a.fonts, prStudioWindowApp(window),
        prStudioWindowAppWindow(window));
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
    a.urls = std::make_unique<UrlOpener>(systemBrowser(prStudioWindowApp(window)));
    PraxBoard::Desc board;
    board.kit = prStudioWindowKit(window);
    board.fonts = a.fonts;
    board.app = prStudioWindowApp(window);
    board.window = prStudioWindowAppWindow(window);
    std::string clientDir, why;
    if (clientHome(&clientDir, &why)) board.storageDirectory = clientDir + "/dashboard-storage";
    board.initialExtent = {1000, 780};
    PrAppDisplayInfo screen{};
    if (a.overlay && prAppDisplays(board.app, &screen, 1))
        board.initialExtent = {float(screen.extent.x), float(screen.extent.y)};
    board.initialExtent.y -= a.look.tabs.height * style::textScale(a.look.textSteps) + 1;
    board.open = a.urls->handle();
    if (a.overlay)
        board.visible = [&a] { return overlayVisible(a); };
    if (clientConfig().sprintEnabled) a.board = std::make_unique<PraxBoard>(board);
    a.view.board = a.board.get();
    a.host.terminals = a.terminals.get();
    a.host.board = a.board.get();
    a.host.ui = prStudioWindowUi(window);
    // Command-V in the prompt: the clipboard's text comes back through the host, which cuts it to the prompt's room.
    a.view.copy = [&a](const std::string& text) {
        if (!a.window || prAppDataWriteText(prStudioWindowApp(a.window), kPrAppDataSourceClipboard,
                {text.data(), text.size()}) != PrStatus_Ok) return false;
        a.audio.play(a.view.windowApp.surface.copyChirp);
        return true;
    };
    a.view.paste = [&a] {
        if (a.window != nullptr) {
            prAppDataReadForms(prStudioWindowApp(a.window), kPrAppDataSourceClipboard, clipboardForms,
                new ComposerPaste{a, a.model->tab(a.model->active()).id, {}});
        }
    };
    // In-app sign-in's URL, in the default browser off the frame thread (frame_host.h LinkSignIn, url_opener.h); a URL
    // the system cannot open is the flow's to end at its deadline, and is never printed. A window that is not an overlay
    // shows from the start, so it may claim now.
    a.view.openLink = a.urls->handle();
    a.view.playChirp = [&a](const Chirp& cue) { a.audio.play(cue); };
    a.host.signIn.openUrl = a.urls->handle();
    if (!a.overlay) {
        a.host.signIn.shown();
    }
    setBudget(a, prStudioWindowApp(window));
    a.wakeApp = prStudioWindowApp(window);
    prAppWake(prStudioWindowApp(window));   // a link that changed before the window opened
    return true;
}

// The terminals and the board end with the window, while its kit still exists. An overlay window that Prax closes
// before it opened could not draw (its swap chain): the run stops with it, as its last window, and fails.
void closeWindow(void* context, PrStudioWindow*, PrStatus) {
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
    a.view.openLink = nullptr;
    a.host.signIn.openUrl = nullptr;
    a.host.ui = nullptr;
    a.host.board = nullptr;
    a.host.terminals = nullptr;
    a.view.board = nullptr;
    a.board.reset();
    a.urls.reset();
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
    if (a.preparingReveal) {
        const bool sprint = a.board && a.model->tab_count() && a.model->tab(a.model->active()).kind == TabKind::Sprint;
        a.revealFrameReady = !sprint || (a.boardRevealStep && a.board->readyToReveal(*a.boardRevealStep));
#ifdef __APPLE__
        a.revealFrameReady = a.revealFrameReady && desktopBlurReady();
#endif
        return;
    }
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
    PrSurface surface{};
    if (prAppWindowSurface(prStudioWindowAppWindow(a.window), &surface) == PrStatus_Ok) {
        coverScreen(surface);
    }
    a.hotkeyGeneration = a.view.application->refresh();
    std::string why;
    a.registeredHotkey = overlayBinding(*a.view.application, why);
    if (!why.empty()) std::fprintf(stderr, "nexus: %s\n", why.c_str());
    PrStatus status = prAppHotkeyRegister(prStudioAppApp(studio), a.registeredHotkey, kHotkeyTag);
    prAppTimerSet(prStudioAppApp(studio), kPolicyCheckTag, 1, 1);
    if (status != PrStatus_Ok) {
        char line[512];
        std::snprintf(line, sizeof line, "nexus: the overlay hotkey (key code %u) did not register (status %d: %s), so "
            "only launching nexus shows the overlay; another app may hold the key: edit overlay_hotkey in the client profile",
            unsigned(a.registeredHotkey.code), int(status),
            status == PrStatus_NotAllowed ? "another app or the system holds it" : "no global hotkeys");
        std::fprintf(stderr, "%s\n", line);
        // The one launch failure the person must be told: on Windows nexus has no terminal, so a message box says it.
        showLaunchError(line);
    } else {
        a.audio.play(a.view.windowApp.surface.startupChirp);
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
void appEvent(void* context, PrStudioApp* studio, const PrAppEvent* e) {
    App& a = appOf(context);
    HitchScope scope(&a.hitch, Phase::Input);
    if (e->kind == PrAppEventKind_Wake) a.host.finishUpload();
    if (e->kind == PrAppEventKind_PowerChanged) {
        const auto power = prAppPower(prStudioAppApp(studio));
        a.view.savingPower = power.lowPower || power.source == PrAppPowerSource_Battery;
        if (a.window && overlayVisible(a)) prAppWindowRequestFrame(prStudioWindowAppWindow(a.window));
    }
    if (e->kind == PrAppEventKind_DisplaysChanged && a.window != nullptr)
        setBudget(a, prStudioWindowApp(a.window));
    if (!a.overlay)
        return;
    if (e->kind == PrAppEventKind_Hotkey && e->tag == kHotkeyTag) {
        carryOut(a, a.preparingReveal ? OverlayAct::Hide : a.rules.hotkey(overlayVisible(a), overlayActive(a)));
    } else if (e->kind == PrAppEventKind_Timer && e->tag == kPolicyCheckTag) {
        const auto generation = a.view.application->refresh();
        if (generation == a.hotkeyGeneration) return;
        a.hotkeyGeneration = generation;
        std::string why;
        const auto next = overlayBinding(*a.view.application, why);
        auto* app = prStudioAppApp(studio);
        if (why.empty() && (next.code != a.registeredHotkey.code || next.modifiers != a.registeredHotkey.modifiers)) {
            if (prAppHotkeyRegister(app, next, kHotkeyTag) == PrStatus_Ok) a.registeredHotkey = next;
            else {
                prAppHotkeyRegister(app, a.registeredHotkey, kHotkeyTag);
                why = "The overlay shortcut is unavailable. Choose another overlay_hotkey in the client profile.";
            }
        }
        if (!why.empty()) std::fprintf(stderr, "nexus: %s\n", why.c_str());
        if (a.window) prAppWindowRequestFrame(prStudioWindowAppWindow(a.window));
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
    auto& a = appOf(context);
    if (e->kind == PrAppEventKind_KeyDown || e->kind == PrAppEventKind_Text ||
            e->kind == PrAppEventKind_PointerDown) a.view.lastInput = e->time;
    return a.host.event(*e);
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
    App& a = appOf(context);
    if (a.preparingReveal) {
        auto* window = prStudioWindowAppWindow(a.window);
        if (a.revealFrameReady && t->extent.x > 0 && t->extent.y > 0) {
            // Studio submitted the updated frame. Complete it before exposing the surface.
            prGpuDeviceWaitIdle(prStudioAppDevice(prStudioWindowStudioApp(a.window)));
            a.preparingReveal = false;
            prAppWindowFocus(window);
            prAppWindowSetOpacity(window, 1);
            a.audio.play(a.view.windowApp.surface.openChirp);
            prAppTimerSet(prStudioWindowApp(a.window), kKeyCheckTag, kKeyCheckSeconds, 0);
        } else {
            if (a.board && !a.boardRevealStep) a.boardRevealStep = a.board->postedStep();
            prAppWindowRequestFrame(window);
        }
    }
    HitchDetector& hitch = a.hitch;
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
    // The render is premultiplied; the encoder takes straight alpha.
    for (size_t i = 0; i < bytes.size(); i += 4) {
        uint8_t alpha = bytes[i + 3];
        if (alpha != 0 && alpha != 255) {
            for (int c = 0; c < 3; ++c)
                bytes[i + c] = uint8_t(std::min(255, bytes[i + c] * 255 / alpha));
        }
    }
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* codec = compress != nullptr ? prImageCodecSetCreateDefault(compress) : nullptr;
    if (codec == nullptr) {
        std::fprintf(stderr, "nexus: no image codec in this build\n");
        prCompressCodecSetDestroy(compress);
        return false;
    }
    PrPixelView view{bytes.data(), size_t(size.x) * 4, size};
    size_t encoded = 0;
    prImageCodecSetEncode(codec, {"image/png", 9}, view, nullptr, 0, &encoded);
    std::vector<uint8_t> png(encoded);
    PrStatus status = prImageCodecSetEncode(codec, {"image/png", 9}, view, png.data(), png.size(), &encoded);
    prImageCodecSetDestroy(codec);
    prCompressCodecSetDestroy(compress);
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

// Lays the window out at `extent` px for a display of `ratio`, renders it into a pixel buffer, and writes a PNG.
// A workaround: Prax has no public offscreen render of a UI tree (only its test fixture, tools/harness/kit_fixture.h).
int screenshot(App& a, const char* path, PrVec2 extent, float ratio) {
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = shaper, .targetPixelRatio = ratio};
    PrLayoutTree* tree = prLayoutTreeCreate(&treeDesc);
    // With --backdrop the window is drawn as the overlay draws it, see-through; otherwise opaque, since an offscreen
    // frame has nothing behind it to show through.
    PrKitTheme theme = windowTheme(a.fonts, a.backdrop);
    // The window paints its fill once over the transparent canvas, as in the live overlay.
    PrUi* ui = prUiCreate(tree);
    PrKit* kit = prKitCreate(ui, &theme);
    Offscreen off{&a, kit};
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = ratio;
    a.view.extent = extent;
    auto terminals = std::make_unique<PraxTerminals>(kit, a.fonts, nullptr, nullptr);
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
    PrGpuDevice* device = prGpuDeviceCreate(PrGpuBackend_Memory, nullptr);
    PrDrawRenderer* renderer = nullptr;
    if (!device ||
            prDrawRendererCreate(PrDrawRendererBackend_Cpu, device, nullptr, &renderer) != PrStatus_Ok) {
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
    // No core file holds a secret typed into the dialog (no_core.h).
    if (!nexus::noCoreDumps()) {
        std::fprintf(stderr, "nexus: core dumps could not be turned off: %s\n", std::strerror(errno));
    }
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
    std::string home;
    if (state == nullptr) {
        std::string homeWhy;
        const bool haveHome = clientHome(&home, &homeWhy);
        const bool madeHome = haveHome && makeClientHome(home, &homeWhy);
        if (!usage && !shot && !printConfig) {
            if (!madeHome || !claimClientHome(home, &homeWhy)) {
                if (homeWhy.empty()) return 0;
                std::fprintf(stderr, "nexus: %s\n", homeWhy.c_str());
                return 1;
            }
        }
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
    if ((state != nullptr && (shot == nullptr || server != nullptr)) || (lost && state == nullptr)) {
        std::fprintf(stderr, "nexus: --state draws a screenshot only, without --server; --lost needs --state\n");
        return 2;
    }
#if !NEXUS_LINK
    if (state != nullptr) {
        std::fprintf(stderr, "nexus: this build has no --state (it was configured with -DNEXUS_LINK=OFF)\n");
        return 2;
    }
#endif
#if NEXUS_LINK
    if (server == nullptr && state == nullptr && shot == nullptr && !demo && !demoTerminal && !bench && !printConfig)
        server = clientConfig().serverHost.c_str();
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
        policy::Host policy("client");
        if (!home.empty()) policy.watch(home + "/policy.bundle.json");
        WindowApplication description;
        description.update(policy, "sprint", {}, false, false, 0, 0);
        std::string why;
        const auto hotkey = overlayBinding(policy, why);
        if (!why.empty()) std::fprintf(stderr, "nexus: %s\n", why.c_str());
        std::string name;
        for (const auto modifier : {PrAppModifierFlag_Shift, PrAppModifierFlag_Control, PrAppModifierFlag_Alt, PrAppModifierFlag_Super}) {
            if (!(hotkey.modifiers & modifier)) continue;
            const auto label = prAppModifierName(modifier);
            name += modifier == style::kCommand ? style::kCommandName : std::string(label.data, label.size);
            name += '-';
        }
        const auto code = prAppKeyCodeName(hotkey.code);
        name += hotkey.code == PrAppKeyCode_Backquote ? "backtick" : std::string(code.data, code.size);
        std::printf("nexus: the overlay window, as nexus asks Prax for it\n"
            "role: Overlay (a borderless panel above other apps, on every Space and beside fullscreen apps)\n"
            "backdrop: %s; opens hidden; shown by the hotkey\n"
            "presence: %s\n"
            "hotkey: %s (PrAppKeyCode %u, modifiers 0x%x)\n"
            "frame: the whole screen with the pointer, menu bar or taskbar included, set through the native window at "
            "each show\n"
            "default background alpha: %.2f; tab strip fill alpha: %.2f\n",
            style::kOverlayBlur ? "Blur" : "Transparent", style::kOverlayAccessory ? "Accessory" : "Regular",
            name.c_str(), unsigned(hotkey.code), unsigned(hotkey.modifiers),
            double(description.overlayOpacity), double(description.surface.tabs.opacity));
        return 0;
    }

    // The friends are the server's (AGENTS.md "Friend tabs"): none until its state arrives over the link, or, for a
    // screenshot of a state file, the file's (sync/client.h friendsOf).
    const bool fromState = state != nullptr;
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
    }
#endif
    // The fleet's machines, by which a terminal tab in ssh is named. A screenshot runs no ssh.
    MachineList machines;
    if (shot == nullptr) {
        machines = loadMachines();
        if (machines.error.empty())
            std::printf("nexus: %zu machines from nova-config\n", machines.machines.size());
        else
            std::fprintf(stderr, "nexus: %s\n", machines.error.c_str());
    }
    // The order the person last left the friend tabs in (tab_order.h), which the server's friends take as they arrive.
    // The stores of what the window keeps across launches are each a file in its home, which a screenshot of a state
    // file never reads. They are read here, before the window shows, and written by the writer's thread, so that no
    // frame waits on the disk; the writer makes every waiting write before nexus exits.
    static StoreWriter writer;
    static TabOrderStore order;
    static TabOrderStore terminalStore;
    static TabOrderStore selection;
    static TabOrderStore scale;
    static KeptDraftsStore keptDrafts;
    std::vector<std::string> savedOrder;
    sync::KeptDrafts kept;
    bool keptReadable = false;
    if (!fromState) {
        order = writer.wrap(fileTabOrderStore());
        terminalStore = writer.wrap(fileTerminalsStore());
        selection = writer.wrap(fileSelectedTabStore());
        scale = writer.wrap(fileTextScaleStore());
        keptDrafts = writer.wrap(fileKeptDraftsStore());
        std::string orderWarning;
        savedOrder = loadTabOrderNames(order, &orderWarning);
        if (!orderWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", orderWarning.c_str());
        // The drafts kept for this window's server only: a draft goes back to a friend of that name on that server.
        if (server != nullptr && keptServer(server)) {
            std::string keptWarning;
            kept = loadKeptDrafts(keptDrafts, server, &keptReadable, &keptWarning);
            if (!keptWarning.empty())
                std::fprintf(stderr, "nexus: %s\n", keptWarning.c_str());
        } else if (server != nullptr) {
            std::fprintf(stderr, "nexus: the server \"%s\" holds a space or a control character, so the drafts of "
                "friends taken off it are kept in this window only, until it quits\n", loggable(server).c_str());
        }
    }
    // The sprint tab first, and no friends yet.
    static Model model({}, fromState || clientConfig().sprintEnabled);
#if NEXUS_LINK
    if (fromState) {
        sync::FriendStrip{}.take(model, held);
        if (!tabGiven) tab = model.active() + 1;
    }
#endif
    const bool live = shot == nullptr;
    TabOrderStore layoutStore;
    SavedLayout savedLayout;
    bool haveLayout = false;
    bool layoutReadable = true;
    if (live) {
        layoutStore = writer.wrap(fileLayoutStore());
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
    }
    TabLayout layout(std::move(savedLayout));
    if (tabGiven) layout.personSelected();
    std::vector<std::pair<std::size_t, std::string>> reopen;
    if (haveLayout) {
        reopen = layout.reopenTerminals(model);
        const auto savedTerminals = std::count_if(layout.saved().tabs.begin(), layout.saved().tabs.end(),
            [](const auto& row) { return row.key.starts_with("terminal:"); });
        if (static_cast<std::size_t>(savedTerminals) > reopen.size())
            std::fprintf(stderr, "nexus: the saved layout contains %zu terminals; reopening the first %zu\n",
                         static_cast<std::size_t>(savedTerminals), reopen.size());
    } else if (live) {
        std::string terminalsWarning;
        for (const std::string& dir : loadTerminals(terminalStore, &terminalsWarning))
            reopen.emplace_back(model.add_terminal(), dir);
        if (!terminalsWarning.empty()) std::fprintf(stderr, "nexus: %s\n", terminalsWarning.c_str());
    }
    App a;  // Destroy the embedded VM before process-level thread-local teardown.
    a.model = &model;
    a.view.model = &model;
    a.view.look = &a.look;
    if (live && !home.empty()) {
        const auto store = fileTabOrderStore(home + "/quiet.txt");
        std::string saved, error;
        if (store.read(&saved, &error)) a.audio.setQuiet(saved == "on\n");
        a.view.setQuiet = [&a, store](bool quiet) {
            a.audio.setQuiet(quiet);
            std::string why;
            if (!store.write(quiet ? "on\n" : "off\n", &why)) a.view.problem = why;
        };
    }
    a.host.model = &model;
    a.host.view = &a.view;
    if (live) {
        if (!home.empty()) a.view.application->watch(home + "/policy.bundle.json");
        a.host.layout = std::move(layout);
        a.host.layoutStore = layoutReadable ? &layoutStore : nullptr;
        a.view.restoreTabLayout = [&a] {
            if (!a.view.windowApp.layoutGeneration) return;
            a.host.groupPolicyReady = true;
            a.host.restoreLayout();
        };
        a.view.windowApp.update(*a.view.application, "sprint", {}, false, false, 0, model.tab_count(), {});
        model.set_tab_groups(a.view.windowApp.groups);
        model.select_tab(savedTabPosition(model, SavedTab{}));
        a.view.restoreTabLayout();
        if (haveLayout && !tabGiven) tab = model.active() + 1;
    }
    // The window opens on the tab --tab names; without it, on the tab selected when nexus last quit, found by who it is
    // or, for a terminal, by its place among the terminals (tab_order.h), or the sprint tab. A saved friend's tab is
    // selected once the server's friends arrive (sync/friend_strip.h). A --tab past the tabs there at launch, with a
    // server, is judged once the server's friends have arrived (below, after the link starts). A screenshot without
    // --tab uses the state file's primary chat when present, or the sprint tab.
    SavedTab savedTab;
    bool savedSelection = haveLayout && !a.host.layout.saved().selected.empty();
    if (!tabGiven && live && !haveLayout) {
        std::string selectionWarning;
        savedTab = loadSavedTab(selection, &selectionWarning, &savedSelection);
        if (!selectionWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", selectionWarning.c_str());
        tab = savedTabPosition(model, savedTab) + 1;
    }
    const bool tabWaits = sync::tabWaitsForFriends(tabGiven, server != nullptr, tab, model.tab_count());
    if ((model.tab_count() != 0 || tabGiven) && !tabWaits) {
        if (tab < 1 || !model.select_tab(tab - 1)) {
            std::fprintf(stderr, "nexus: no tab %zu (there are %zu)\n", tab, model.tab_count());
            return 2;
        }
    }

    // The tab selected at launch is no choice of the person's (FrameHost::personSelected): only a later one is.
    if (model.tab_count() != 0) {
        a.view.selectedId = model.tab(model.active()).id;
    }
    // The screenshot stays opaque: an offscreen frame has nothing behind it to show through.
    a.overlay = !windowed && shot == nullptr;
    a.look.overlay = a.overlay;
    // Escape hides the overlay, as the hotkey does; a normal window has nothing to hide.
    if (a.overlay)
        a.view.hide = [&a] { carryOut(a, a.rules.escape(overlayVisible(a))); };
    // The friends' order is saved as the person moves a tab, once the server's friends are in the strip: a move before
    // them (a terminal's) leaves the saved order as it was.
    if (!fromState) {
        a.view.moved = [&a] {
            a.host.keepTerminals();
            if (!a.host.strip.taken()) {
                return;
            }
            std::string error;
            if (!saveTabOrder(order, model, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        };
    }
    // Each change of the selected tab, and of the terminal tabs, is saved as it happens, so a quit of any kind leaves
    // them saved. A screenshot saves nothing.
    // A saved friend's tab is selected when the server's friends arrive; until then nothing overwrites it. The drafts
    // kept for friends without a tab are saved as they change, unless their file did not read (tab_order.h).
    const bool waitForSaved = live && server != nullptr && savedTab.kind == TabKind::Friend;
    a.host.strip = sync::FriendStrip(std::move(savedOrder), waitForSaved ? savedTab.name : std::string(),
        std::move(kept));
    if (tabGiven || (savedSelection && !waitForSaved)) a.host.strip.personSelected();
    if (live) {
        a.host.keep = true;
        a.host.terminalStore = &terminalStore;
        a.host.selectionStore = &selection;
        a.host.keptStore = keptReadable ? &keptDrafts : nullptr;
        a.reopen = std::move(reopen);
        a.view.selected = [&a] { a.host.personSelected(); };
    }
    // The text scale the person last chose, applied at the first frame, and each change saved as it is applied. A
    // screenshot draws at the default scale and saves nothing.
    if (live) {
        std::string scaleWarning;
        a.view.askedTextSteps = loadTextScale(scale, -style::kTextStepsDown, style::kTextStepsUp, &scaleWarning);
        if (!scaleWarning.empty())
            std::fprintf(stderr, "nexus: %s\n", scaleWarning.c_str());
        a.view.scaled = [&a] {
            std::string error;
            if (!saveTextScale(scale, a.look.textSteps, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        };
    }
    a.host.hitch = &a.hitch;
#if NEXUS_LINK
    if (server != nullptr) {
        link::ClientLink::Options options;
        options.bootstrap.host = server;
        options.wake = [&a] {
            if (PrApp* app = a.wakeApp.load())
                prAppWake(app);
        };
        a.host.uploadWake = options.wake;
        a.link = std::make_unique<link::ClientPort>(std::move(options));
        a.host.connect(a.link.get(), server);
        // A --tab past the tabs at launch names a friend's tab, perhaps, that the server's friends bring: the window
        // waits for them, up to kTabWaitSeconds or until the link fails, before it judges the tab, and opens on it.
        if (tabWaits) {
            const auto nowMs = [] {
                return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            };
            const long long until = nowMs() + kTabWaitSeconds * 1000LL;
            while (sync::tabWaitGoesOn(a.host.strip, a.host.phase == sync::LinkPhase::Failed, nowMs(), until)) {
                a.host.pollLink();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (tab < 1 || !model.select_tab(tab - 1)) {
                std::fprintf(stderr, "nexus: no tab %zu (there are %zu%s)\n", tab, model.tab_count(),
                    a.host.strip.taken() ? "" : ", and the server's friends did not arrive in time");
                return 2;
            }
            a.view.selectedId = model.tab(model.active()).id;
        }
    }
    // --state: the server's state from a file, as the window holds it once the server is heard (or, with --lost, once
    // it has lost the server).
    if (fromState) {
        a.host.showState(held, lost);
        a.findBoard = [] { return SprintBoardAddress{"", "a screenshot of a state file shows no sprint board"}; };
    }
#endif
    // --demo: sample entries after what the state file holds, in the active tab if it is a friend's.
    if (demo && model.tab_count() != 0 && model.tab(model.active()).kind == TabKind::Friend) {
        seedDemo(model, model.active());
    }
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
            .transparent = a.overlay,
            .hidden = a.overlay,
            .placement = PrAppWindowPlacement_Center,
            // Prax negotiates an extended-range surface on HDR displays and an SDR surface elsewhere.
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
        options.windowModel = PrAppWindowModel_Desktop;
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
