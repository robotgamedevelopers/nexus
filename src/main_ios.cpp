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
// --server-command WORDS, or the client config's `server-command`, is what the link runs there in place of
// `nexus-server new` (link/bootstrap.h serverCommandWords), as on the desktop: the mock's command (docs/mock.md). A
// device key behind nexus-ios-gate runs only nexus-server new and history-page, so the gate refuses any other.
//
// The window is locked (client_lock.h, ios_lock.h): until the device's owner passes Face ID or Touch ID (or the
// passcode) it draws the lock screen alone and no input reaches nexus, and it locks again when the app goes to the
// background or goes inactive (the app switcher), when the device's screen locks, and at the idle timeout. While it is
// not active the window is covered, so the app switcher's snapshot shows nothing of nexus.
//
// Usage (as the simulator or devicectl passes a launch's arguments):
//   nexus [--server USER@HOST] [--server-command WORDS] | nexus --state FILE [--lost]

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
#include "client_lock.h"
#include "client_lock_platform.h"
#include "client_home.h"
#include "frame_host.h"
#include "hitch.h"
#include "ios_lock.h"
#include "keyboard_room.h"
#include "keyboard_start.h"
#include "link/bootstrap.h"
#include "link/client_port.h"
#include "link/inapp_ssh.h"
#include "model.h"
#include "platform.h"
#include "server_choice.h"
#include "state_window.h"
#include "style.h"
#include "tab_order.h"
#include "url_opener.h"
#include "view.h"
#include "window_app.h"

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
    TextInputStart textInput;               // whether the frames edit text, for the keyboard's start (keyboard_start.h)
    PrAppInsets safeArea{};                 // the frame's safe area, which Prax pads the window's panel by
    float keyboardRoom = 0;                 // px of content above the on-screen keyboard this frame; 0: none covers it
    // The client lock (client_lock.h, ios_lock.h): nothing of nexus is drawn and no input reaches it until the device's
    // owner passes Face ID, Touch ID or the passcode. Made in main, before the window opens.
    std::unique_ptr<ClientLock> lock;
    std::unique_ptr<IosLock> iosLock;
    std::unique_ptr<LockAuthenticator> authenticator;
    // The authenticator's answers, from its thread to the window's, taken at the next frame. Shared with what posts
    // them, so that an answer the system gives after the window has gone lands here, not in a freed App (as main.cpp).
    struct LockInbox {
        LockMailbox mail;
        std::atomic<PrApp*> app{nullptr};
    };
    std::shared_ptr<LockInbox> lockInbox = std::make_shared<LockInbox>();
    std::string lockMethod;                 // how the lock screen names the authentication: "Face ID or your passcode"
    LockState lockShown = LockState::Locked; // the lock's state as the window last showed it
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

App& appOf(void* context) {
    return *static_cast<App*>(context);
}

// The client lock's part of the window (ios_lock.h). The tag of its idle timer among the app's timers.
constexpr std::uint64_t kLockTimer = 0x6c6f636b;  // "lock"

// The lock screen (View::lockScreen) while nexus is not unlocked, and none once it is. Each change is said on stderr
// (the device's console), and an unlock lets a sign-in claim its flow, which a locked window never does (main.cpp).
void syncLockScreen(App& a) {
    const ClientLock& lock = a.iosLock->lock();
    if (lock.state() != a.lockShown) {
        const LockState before = a.lockShown;
        a.lockShown = lock.state();
        if (lock.state() == LockState::Unlocked) {
            std::fprintf(stderr, "nexus: unlocked\n");
            a.host.signIn.shown();
        } else if (lock.state() == LockState::Locked) {
            std::fprintf(stderr, "nexus: %s (reason %d)\n", before == LockState::Unlocked ? "locked" : "not unlocked",
                int(lock.reason()));
        }
    }
    if (a.iosLock->frame()) {
        a.view.lockScreen.reset();
        return;
    }
    a.view.lockScreen = View::LockScreen{lockTitle(lock.state()),
        lockMessage(lock.state(), lock.reason(), a.lockMethod, a.iosLock->detail())};
}

IosLifecycle iosLifecycle(PrAppLifecycle lifecycle) {
    switch (lifecycle) {
    case PrAppLifecycle_Active: return IosLifecycle::Active;
    case PrAppLifecycle_Inactive: return IosLifecycle::Inactive;
    default: return IosLifecycle::Background;
    }
}

// The window opened: the lock asks the system through it, and covers it for the app switcher; a timer each second
// counts the idle time; and the app's state now is the lock's first (an app launched active asks at once).
void startLock(App& a, PrStudioWindow* window) {
    PrApp* app = prStudioWindowApp(window);
    IosLockSystem system;
    system.request = [&a](std::uint64_t attempt) {
        PrSurface surface{};
        if (a.window) prAppWindowSurface(prStudioWindowAppWindow(a.window), &surface);
        a.authenticator->request(attempt, surface, [inbox = a.lockInbox](LockNotice notice) {
            inbox->mail.post(std::move(notice));
            if (PrApp* app = inbox->app.load()) prAppWake(app);
        });
    };
    system.cancel = [&a] { a.authenticator->cancel(); };
    system.cover = [&a](bool on) {
        PrSurface surface{};
        if (a.window && prAppWindowSurface(prStudioWindowAppWindow(a.window), &surface) == PrStatus_Ok)
            iosCoverWindow(surface, on);
    };
    a.iosLock = std::make_unique<IosLock>(*a.lock, std::move(system));
    a.lockInbox->app = app;
    a.view.unlock = [&a] { a.iosLock->unlock(); };
    prAppTimerSet(app, kLockTimer, 1.0, 1.0);
    a.iosLock->lifecycle(iosLifecycle(prAppLifecycle(app)));
    syncLockScreen(a);
}

// The window closing: a prompt still up is taken down, and the lock stays as it is (locked, unless the window closes
// unlocked, when nothing more is drawn anyway).
void stopLock(App& a) {
    if (!a.iosLock) return;
    prAppTimerCancel(prStudioWindowApp(a.window), kLockTimer);
    a.lockInbox->app = nullptr;
    a.authenticator->cancel();
    a.view.unlock = nullptr;
    a.iosLock.reset();
}

// The app's own events (PrStudioAppDesc::event): its lifecycle, the screen lock, and the idle timer.
void appEvent(void* context, PrStudioApp*, const PrAppEvent* e) {
    App& a = appOf(context);
    if (!a.iosLock || !a.window) return;
    PrApp* app = prStudioWindowApp(a.window);
    switch (e->kind) {
    case PrAppEventKind_LifecycleChanged: a.iosLock->lifecycle(iosLifecycle(prAppLifecycle(app))); break;
    case PrAppEventKind_ScreenLocked: a.iosLock->screenLocked(); break;
    case PrAppEventKind_Timer: {
        if (e->tag != kLockTimer) return;
        const bool was = a.iosLock->drawsContent();
        a.iosLock->tick(e->time);
        if (a.iosLock->drawsContent() == was) return;   // a frame only when the idle timeout locked it
        break;
    }
    default: return;
    }
    // The lock screen is drawn in the frame this asks for: on going inactive, before the system's snapshot.
    syncLockScreen(a);
    prAppWindowRequestFrame(prStudioWindowAppWindow(a.window));
}

bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    a.window = window;
    a.wakeApp = prStudioWindowApp(window);
    startLock(a, window);
    PrKitTheme theme = windowTheme(a.fonts, false);
    prKitSetTheme(prStudioWindowKit(window), &theme);
    a.urls = std::make_unique<UrlOpener>(systemBrowser(prStudioWindowApp(window)));
    a.view.openLink = a.urls->handle();
    a.host.signIn.openUrl = a.urls->handle();
    if (a.iosLock->drawsContent()) a.host.signIn.shown();   // locked, a sign-in waits for the unlock
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
    }
    PrAppDisplayInfo info{};
    a.hitch.setRefreshRate(prAppDisplays(prStudioWindowApp(window), &info, 1) != 0 ? double(info.refreshRate) : 0);
    return true;
}

void closeWindow(void* context, PrStudioWindow*, PrStatus) {
    App& a = appOf(context);
    stopLock(a);
    a.window = nullptr;
    a.wakeApp = nullptr;
    a.view.openLink = nullptr;
    a.view.copy = nullptr;
    a.host.signIn.openUrl = nullptr;
    a.host.ui = nullptr;
    a.host.board = nullptr;
    a.view.board = nullptr;
    a.board.reset();
    a.urls.reset();
}

// The kit's own palette, so that Studio keeps the window's theme (main.cpp palette says why).
void palette(void*, PrStudioWindow* window, const PrAppUserPreferences*, PrKitPalette* out) {
    *out = prStudioWindowKit(window)->theme.palette;
}

// How much of the viewport's foot the on-screen keyboard covers: the viewport is the frame less its safe area
// (studio_window.h PrStudioWindowBuildFn), and the keyboard's rect is in the frame's coordinates, empty when it is
// hidden.
void beginFrame(void* context, PrStudioWindow* window, const PrAppFrame* f) {
    App& a = appOf(context);
    a.hitch.begin();
    a.safeArea = f->safeArea;
    a.keyboardRoom = roomAboveKeyboard(f->extent, f->safeArea, f->keyboard);
    // An iPad's regular width takes a narrower prompt and column than the policy's (style.h kRegularWidthColumn); an
    // iPhone's compact width gives the replies the column's whole width (View::fullWidthReplies).
    PrSurface surface{};
    const bool known = prAppWindowSurface(prStudioWindowAppWindow(window), &surface) == PrStatus_Ok;
    a.view.columnScale = known && regularWidth(surface) ? style::kRegularWidthColumn : 1;
    a.view.fullWidthReplies = known && !regularWidth(surface);
    // The authenticator's answers, then the time: the frame draws the lock screen unless the lock is unlocked now.
    a.iosLock->notices(a.lockInbox->mail.take(), f->time);
    a.iosLock->tick(f->time);
    syncLockScreen(a);
    a.host.beginFrame(prStudioWindowKit(window), a.fonts, f->time, f->display);
}

bool event(void* context, PrStudioWindow*, const PrAppEvent* e) {
    auto& a = appOf(context);
    // The person's input reaches nexus only while it is unlocked (ClientLock::input, which also restarts the idle
    // time). Locked, only the lock screen takes it (FrameHost::event with View::lockScreen): nothing is queued for
    // later.
    if (isInputEvent(*e) && a.iosLock->drawsContent() && !a.iosLock->input(e->time, resetsIdle(*e))) {
        syncLockScreen(a);   // the idle timeout locked it at this input, which is dropped
        return true;
    }
    if (e->kind == PrAppEventKind_KeyDown || e->kind == PrAppEventKind_Text || e->kind == PrAppEventKind_PointerDown)
        a.view.lastInput = e->time;
    return a.host.event(*e);
}

// The window above the on-screen keyboard: while it shows, the window is laid out in the part of the viewport it leaves,
// so that the prompt and the transcript's end stay in sight, as a desktop window shorter by that much would. The
// content starts at the safe area's top (Prax's Studio window pads its panel to it), so the part is the room from there
// to the keyboard's top. Without a keyboard it is the viewport less its safe area: `extent` is the whole frame's, and a
// box that tall ran past the screen's foot by the safe area's top and bottom. It is built in the same box with the
// keyboard and without it (FrameHost::buildAbove).
void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2 extent) {
    App& a = appOf(context);
    a.host.buildAbove(prStudioWindowKit(window), root, contentBox(extent, a.safeArea, a.keyboardRoom));
}

// The frame is laid out, and Prax will raise or lower the keyboard by what it edits once it is drawn. A frame that starts
// editing text lets the view go as the first responder first, so the keyboard rises with its keys (keyboard_start.h).
bool laidOut(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    if (a.textInput.frameEnds(prUiOutput(prStudioWindowUi(window)).hasTextInput) == KeyboardStart::Present) {
        PrSurface surface{};
        if (prAppWindowSurface(prStudioWindowAppWindow(window), &surface) == PrStatus_Ok)
            releaseFirstResponder(surface);
    }
    return windowWantsFrame(a.view);
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
// (policy.bundle.json, docs/policy.md), the AIs' saved order, the tab layout, the selected tab and the text scale.
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
    const bool waitForSaved = savedTab.kind == TabKind::AI;
    a.host.strip = sync::AIStrip(std::move(savedOrder), waitForSaved ? savedTab.name : std::string(), {});
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
    const char* serverCommand = nullptr;
    bool lost = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state = argv[++i];
        } else if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            server = argv[++i];
        } else if (std::strcmp(argv[i], "--server-command") == 0 && i + 1 < argc) {
            serverCommand = argv[++i];
        } else if (std::strcmp(argv[i], "--lost") == 0) {
            lost = true;
        } else {
            std::fprintf(stderr, "usage: nexus [--server USER@HOST] [--server-command WORDS] | --state FILE [--lost]\n");
            return 2;
        }
    }
    if ((lost && state == nullptr) || (state != nullptr && (server != nullptr || serverCommand != nullptr))) {
        std::fprintf(stderr, "nexus: --lost needs --state, and --state shows a file without --server or "
            "--server-command\n");
        return 2;
    }
    // --server-command, else the client config's server-command: the words the link runs in place of nexus-server new.
    std::vector<std::string> serverWords;
    {
        const std::string command = serverCommand != nullptr ? serverCommand : clientConfig().serverCommand;
        std::string why;
        if (!command.empty() && !link::serverCommandWords(command, &serverWords, &why)) {
            std::fprintf(stderr, "nexus: --server-command: %s\n", why.c_str());
            return 2;
        }
    }
    static Model model({}, state == nullptr);   // the sprint tab (the Dashboard) when linked, as on the desktop
    App a;
    a.model = &model;
    a.view.model = &model;
    a.view.dialogsAtTop = true;
    a.view.tapEndsEditing = true;   // a tap outside the prompt lowers the keyboard (View::tapEndsEditing)     // the on-screen keyboard covers the lower part (View::dialogsAtTop)
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
        auto startLink = [&a, home, serverWords](const std::string& serverHost) {
            link::InAppSshOptions ssh;
            ssh.dir = home;
            if (const char* h = std::getenv("HOME"))
                ssh.publicCopy = std::string(h) + "/Documents/nexus-ssh-key.pub";
            link::ClientLink::Options options;
            options.bootstrap.host = serverHost;
            if (!serverWords.empty()) options.bootstrap.command = serverWords;
            options.run = link::inAppSsh(ssh);
            options.wake = [&a] {
                if (PrApp* app = a.wakeApp.load())
                    prAppWake(app);
            };
            a.link = std::make_unique<link::ClientPort>(std::move(options));
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

    // The client lock, locked from the start: the first frame is the lock screen's (client_lock.h). The idle timeout is
    // the client policy's (window_app.h lockIdleSeconds).
    {
        std::string why;
        const double idle = lockIdleSeconds(*a.view.application, why);
        if (!why.empty()) std::fprintf(stderr, "nexus: %s\n", why.c_str());
        a.lock = std::make_unique<ClientLock>(idle);
        a.authenticator = systemLockAuthenticator();
        a.lockMethod = a.authenticator->method();
        a.view.lockScreen = View::LockScreen{lockTitle(a.lock->state()),
            lockMessage(a.lock->state(), a.lock->reason(), a.lockMethod, {})};
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
        .lastWindow = PrStudioAppLastWindow_Stop, .event = appEvent, .context = &a};
    const PrStatus status = prStudioAppRun(&desc);
    if (status != PrStatus_Ok)
        std::fprintf(stderr, "nexus: the window could not open or the run could not start (status %d)\n", int(status));
    if (a.link) a.link->close();
    prFontSetDestroy(a.fonts);
    prFontSystemDestroy(system);
    return status == PrStatus_Ok ? 0 : 1;
}
