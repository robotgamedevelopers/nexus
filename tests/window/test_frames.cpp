// The frame budget: the window's real frame code (frame_host.h over the view, Prax's terminals and the sprint tab's
// page) driven for a fixed number of frames against slow fakes, each frame's main-thread work measured by the hitch
// detector (hitch.h) on the real monotonic clock. The client runs at 120 Hz, so any frame whose work passes 8.3 ms is
// a hitch, and the test fails naming the case, the frame and the phases. The budget is never loosened for a case.
//
// Headless: no window and no GPU. A frame here is what Prax's Studio window does in one (studio_window.cpp renderFrame)
// short of its swap chain: the host's frame start, the rounds, then the scene taken and played into a draw list. The
// frames' own clock (what the view and the board read) is made up, from 1000 s at 120 Hz; the work is timed on the real
// clock, and where a case needs real time to pass (a slow ssh, a shell's output) the frames are paced at 120 Hz.
//
// The slow fakes: a link whose server stalls and comes back with a large state at once; a bootstrap over an ssh that
// hangs for seconds (the real link thread, link/client.h); preference files whose every write takes 200 ms; a history
// of thousands of entries with long lines; a reply streamed into a long open entry; a 1 MB paste; terminals opened,
// filled with 10,000 lines and closed; the sprint tab's page shown for the first time and polled; tabs switched; the
// window resized; the text scale changed; text that needs fallback fonts; the secret dialog opened, typed and pasted
// into, sent and cancelled.
//
// A case with a frame over the budget runs again, up to three times in all, in a fresh window. A real hitch is work on
// the frame thread and is over the budget on the same frame every run; a frame the scheduler held up on a loaded host
// is not. A frame fails only if it is over the budget in every run, and the report gives its phase and each run's
// wall-clock time beside the frame thread's CPU time, so a reader sees whether the time was work or waiting.
//
// Text is set in the system's fonts, shells run and real time passes: a functional test. Its working directory is its
// own, under the build tree. Usage: nexus_frame_tests SLOW_SSH [--inject CASE FRAME] [CASE...]; --inject gives that
// case's frame 10 ms of busy work in the Keep phase, the proof that a real hitch fails every time.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_surface.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/layout_style.h"
#include "pr/text.h"
#include "pr/time.h"
#include "pr/ui.h"

#include "board.h"
#include "font_prefetch.h"
#include "frame_host.h"
#include "hitch.h"
#include "link/client_port.h"
#include "model.h"
#include "style.h"
#include "tab_order.h"
#include "terminals.h"
#include "url_opener.h"
#include "view.h"

namespace {

using namespace nexus;

int failures = 0;
const char* slowSsh = nullptr;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

double realMs(void*) {
    return prTimeMonotonicMilliseconds();
}

// The CPU time the calling thread has used, ms: what the frame thread spent working, whatever the scheduler did.
double threadCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) * 1000 + double(t.tv_nsec) / 1e6;
}

// Spins for `ms` of real time: a stand-in for work on the frame thread.
void busyWait(double ms) {
    const double until = prTimeMonotonicMilliseconds() + ms;
    while (prTimeMonotonicMilliseconds() < until) {
    }
}

// A frame over the budget in one run of a case: its number, the input events pushed before it (which place it among
// the case's steps when frames are counted differently from run to run), its wall-clock work, the frame thread's CPU
// time over the frame, and the phase with the most of its time.
struct Over {
    std::uint64_t frame = 0;
    std::uint64_t events = 0;
    double wall = 0;
    double cpu = 0;
    std::string phase;
    std::string line;           // the hitch's log line
    bool first = false;         // the window's first frame, over kFirstFrameMs (not over the budget)
};

// The most a window's first frame may take: half the budget, so that a host that does the same work at half the speed
// (every core busy) still keeps the budget. Its fonts' faces are made before it (font_prefetch.h warmShaper), so it needs
// about 1 ms.
constexpr double kFirstFrameMs = 4;

// The hitch lines as the app logs them, kept for the case's report.
struct Lines {
    std::vector<std::string> lines;
};

void keepLine(void* context, const char* line) {
    static_cast<Lines*>(context)->lines.emplace_back(line);
}

// ---------------------------------------------------------------------------------------------------------------------
// The slow fakes

// A link whose state the test sets: poll hands the newest once, as the link's thread does, and never waits.
struct FakeLink final : LinkPort {
    LinkState state;
    bool changed = false;
    std::vector<sync::Action> sent;

    bool poll(LinkState* out) override {
        if (!changed)
            return false;
        *out = state;
        changed = false;
        return true;
    }
    std::uint64_t send(sync::Action action) override { sent.push_back(std::move(action)); return sent.size(); }

    void set(LinkState s) {
        state = std::move(s);
        changed = true;
    }
};

// A store in memory whose every write takes `ms` of real time, as a flush to a slow or stalled disk does.
TabOrderStore slowStore(const char* where, int ms, std::string* kept) {
    TabOrderStore store;
    store.where = where;
    store.read = [](std::string*, std::string* error) {
        error->clear();
        return false;
    };
    store.write = [ms, kept](const std::string& text, std::string*) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        *kept = text;
        return true;
    };
    return store;
}

// The sprint tab's fixture board: the served board's habits (a 1 s poll of /api/sprint that rewrites the page) over
// a page about the size of the real one, 400 rows of cards.
constexpr const char* kUrl = "http://board.test:8080/";

std::string boardPage() {
    std::string rows;
    for (int i = 0; i < 400; ++i) {
        rows += "<tr><td>card " + std::to_string(i) + "</td><td class=s>landed</td><td>rowan</td><td>" +
            std::to_string(i * 37 % 1000) + " ms</td><td>a card's title, long enough to wrap in a narrow column</td></tr>";
    }
    return "<!doctype html><html lang=en><head><meta charset=utf-8><title>board</title><style>body{margin:0;"
        "background:#0d0f12;color:#e6e8eb;font-family:system-ui} td{padding:4px 8px} .s{color:#7c7}</style></head>"
        "<body><h1>nova-sprint</h1><div id=n>-</div><table id=t>" + rows + "</table><script>"
        "var polls=0;function poll(){fetch('/api/sprint',{cache:'no-store'}).then(function(r){return r.json();})"
        ".then(function(j){polls++;document.getElementById('n').textContent='landed '+j.landed.toLocaleString('en-US')+"
        "' polls '+polls;var c=document.querySelectorAll('.s');for(var i=0;i<c.length;i++)c[i].textContent="
        "(i+polls)%3?'landed':'open';});}poll();setInterval(poll,1000);</script></body></html>";
}

struct BoardServer {
    std::string page = boardPage();
    std::string body;
    int polls = 0;

    static void answer(void* context, PrFetchInterceptRequest* request) {
        BoardServer& s = *static_cast<BoardServer*>(context);
        std::string url(request->desc.url.data, request->desc.url.size);
        PrFetchHeader type{"Content-Type", "text/html; charset=utf-8"};
        if (url == kUrl) {
            s.body = s.page;
        } else {
            ++s.polls;
            type.value = PrStringView{"application/json", 16};
            s.body = "{\"landed\": " + std::to_string(1000 + s.polls) + "}";
        }
        PrFetchInterceptResponse response{
            .status = 200,
            .type = PrFetchResponseType_Basic,
            .headers = &type,
            .headerCount = 1,
            .body = PrByteView{reinterpret_cast<const uint8_t*>(s.body.data()), s.body.size()},
            .url = {},
        };
        prFetchInterceptRequestRespond(request, &response);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// The window with no window

struct Window {
    Lines lines;
    HitchDetector hitch{HitchClock{realMs, nullptr}, HitchLog{keepLine, &lines}};
    Model model;
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    PrDrawList* list = prDrawListCreate();
    const PrLayoutScene* scene = nullptr;
    Look look;
    View view;
    BoardServer server;
    std::atomic<double> boardMs{0};    // read on the board's page thread (board.h)
    std::unique_ptr<PraxTerminals> terminals;
    std::unique_ptr<PraxBoard> board;
    FrameHost host;
    FakeLink link;
    // The preference files on a slow disk, written through the writer as the app writes them (main.cpp).
    std::string keptTerminals;
    std::string keptSelection;
    StoreWriter writer;
    TabOrderStore terminalStore = writer.wrap(slowStore("terminals.txt", 200, &keptTerminals));
    TabOrderStore selectionStore = writer.wrap(slowStore("selected-tab.txt", 200, &keptSelection));
    double time = 1000;
    PrVec2 extent{1200, 800};
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
    // The frames whose work is a known hitch (Case::known), by number: no other frame of a case may pass the budget.
    std::vector<std::uint64_t> knownFrames;
    int pastes = 0;                     // Command-Vs that reached the prompt's View::paste
    std::uint64_t events = 0;           // input events pushed so far
    std::map<std::uint64_t, std::pair<std::uint64_t, double>> frameInfo; // over-budget frames: events before, CPU ms
    std::uint64_t injectFrame = 0;      // a frame given 10 ms of work in Keep (--inject); 0 for none
    double firstWall = 0;               // the first frame's work, ms
    double firstCpu = 0;                // and the frame thread's CPU time over it
    std::uint64_t firstEvents = 0;      // and the input events pushed before it
    Hitch firstHitch;                   // and the frame as the detector measured it
    std::string firstLine;              // and its phases, as a hitch's line gives them

    Window() : model(std::vector<FriendInfo>{FriendInfo{"alex", "m"}, FriendInfo{"emma", "m"}, FriendInfo{"stella", "m"}},
            true) {
        hitch.setRefreshRate(120);
        prFontSetSetSystem(fonts, system);
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
        // As the app does at launch, while nothing shows (main.cpp): the fallback fonts loaded before any frame.
        FontPrefetch(fonts).wait();
        // As the app does when its window opens, before the first frame (main.cpp open): the text fonts' faces made.
        warmShaper(shaper, fonts, true);
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 2};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        PrKitTheme theme = windowTheme(fonts, true);
        kit = prKitCreate(ui, &theme);
        look.overlay = true;
        view.model = &model;
        view.look = &look;
        view.extent = extent;
        terminals = std::make_unique<PraxTerminals>(kit, fonts, nullptr, nullptr);
        terminals->program = "/bin/sh";
        char cwd[4096];
        terminals->directory = getcwd(cwd, sizeof cwd) != nullptr ? cwd : "";
        view.terminals = terminals.get();
        PraxBoard::Desc b;
        b.kit = kit;
        b.fonts = fonts;
        b.url = kUrl;
        b.answer = BoardServer::answer;
        b.answerContext = &server;
        b.now = [](void* c) { return static_cast<std::atomic<double>*>(c)->load(); };
        b.wallNow = [](void* c) { return 1794326567000.0 + static_cast<std::atomic<double>*>(c)->load(); };
        b.clockContext = &boardMs;
        b.open = [](const std::string&) {};
        board = std::make_unique<PraxBoard>(b);
        view.board = board.get();
        host.model = &model;
        host.view = &view;
        host.terminals = terminals.get();
        host.board = board.get();
        host.hitch = &hitch;
        host.ui = ui;
        host.keep = true;
        host.terminalStore = &terminalStore;
        host.selectionStore = &selectionStore;
        view.selected = [this] { host.keepSelection(); };
        // Command-V in the prompt: the app reads the clipboard here, and its text comes back later (commandV).
        view.paste = [this] { ++pastes; };
        host.connect(&link, "server.test");
        model.select_tab(1);    // alex
    }

    ~Window() {
        host.terminals = nullptr;
        host.board = nullptr;
        view.board = nullptr;
        view.terminals = nullptr;
        board.reset();
        terminals.reset();
        prLayoutSceneRelease(scene);
        prDrawListRelease(list);
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    // One frame as Prax's Studio window makes it (studio_window.cpp renderFrame), without the swap chain: the host's
    // frame start, the rounds, the scene taken and played. With `paced`, it starts at the next 120 Hz tick of the real
    // clock, so that real time passes as it does in the app.
    void frame(bool paced = false) {
        if (paced) {
            next += std::chrono::microseconds(8333);
            std::this_thread::sleep_until(next);
        }
        time += 1.0 / 120;
        boardMs = boardMs.load() + 1000.0 / 120;
        const std::uint64_t hitchesBefore = hitch.hitches();
        const double cpuBefore = threadCpuMs();
        const double wallBefore = hitch.now();
        hitch.begin();
        view.extent = extent;
        if (hitch.frames() + 1 == injectFrame) {
            HitchScope scope(&hitch, Phase::Keep);
            busyWait(10);
        }
        host.beginFrame(kit, fonts, time, prAppDisplayDefault());
        {
            HitchScope scope(&hitch, Phase::Layout);
            PrUiFrameDesc desc{.time = time, .delta = 1.0 / 120, .extent = extent, .maxRounds = 4,
                .display = prAppDisplayDefault(), .preferences = {}};
            prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
                Window& w = *static_cast<Window*>(context);
                PrKitSurfaceStyle style{
                    .box = {.overflow = PrKitOverflow_Clip,
                        .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
                    .background = PrKitBackground(w.look.overlay ? PrKitBackground_None : PrKitBackground_App),
                };
                w.host.build(w.kit, prKitSurface(w.kit, kPrLayoutNodeNone, {}, style));
            }, this);
        }
        {
            HitchScope scope(&hitch, Phase::Draw);
            const PrLayoutScene* taken = prLayoutTreeScene(tree);
            prLayoutSceneRelease(scene);
            scene = taken;
            std::vector<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree, scene, nullptr, 0));
            prLayoutSceneScrolls(tree, scene, scrolls.data(), uint32_t(scrolls.size()));
            PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
            options.scrolls = scrolls.data();
            options.scrollCount = uint32_t(scrolls.size());
            options.canvasFilled = true;
            prDrawListClear(list);
            prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{2, 2}));
            prLayoutScenePlay(scene, list, &options);
            prDrawListPopTransform(list);
        }
        if (hitch.frames() == 0) {
            // The window's first frame, whatever its time: its work, CPU time and phases, for the case's report.
            Hitch& first = firstHitch;
            first = hitch.current();
            first.frame = 1;
            first.work = hitch.now() - wallBefore;
            firstWall = first.work;
            firstCpu = threadCpuMs() - cpuBefore;
            firstEvents = events;
            char line[1024];
            firstLine = HitchDetector::format(first, hitch.budget(), line, sizeof line);
        }
        hitch.end();
        if (hitch.hitches() != hitchesBefore) {
            frameInfo[hitch.frames()] = {events, threadCpuMs() - cpuBefore};
        }
    }

    void frames(int n, bool paced = false) {
        for (int i = 0; i < n; ++i)
            frame(paced);
    }

    // Runs frames until `done` or `limit` paced frames.
    bool until(const std::function<bool()>& done, int limit) {
        for (int i = 0; i < limit; ++i) {
            if (done())
                return true;
            frame(true);
        }
        return done();
    }

    // An event as the window gets it, measured as input the way the app's event callback is (main.cpp event).
    void push(PrAppEvent e) {
        ++events;
        e.time = time;
        if (!host.event(e)) {
            HitchScope scope(&hitch, Phase::Input);
            prUiPushEvent(ui, kPrUiViewportMain, &e);
        }
    }

    void key(PrAppKey k, PrAppKeyCode code, PrAppModifierFlags modifiers = 0, const char* text = nullptr) {
        PrAppEvent e{};
        e.kind = PrAppEventKind_KeyDown;
        e.modifiers = modifiers;
        e.keyboard.key = k;
        e.keyboard.code = code;
        push(e);
        if (text != nullptr) {
            PrAppEvent t{};
            t.kind = PrAppEventKind_Text;
            t.modifiers = modifiers;
            t.input.text = PrStringView{text, std::strlen(text)};
            push(t);
        }
        e.kind = PrAppEventKind_KeyUp;
        push(e);
        frame();
    }

    void type(const std::string& text) {
        for (char c : text) {
            const char s[2] = {c, 0};
            key(PrAppKey(c), 0, 0, s);
        }
    }

    // `text` typed all at once, between two frames, as a fast typist's keys arrive in one frame.
    void typeAtOnce(const std::string& text) {
        for (const char& c : text) {
            PrAppEvent e{};
            e.kind = PrAppEventKind_KeyDown;
            e.keyboard.key = PrAppKey(c);
            push(e);
            PrAppEvent t{};
            t.kind = PrAppEventKind_Text;
            t.input.text = PrStringView{&c, 1};
            push(t);
            e.kind = PrAppEventKind_KeyUp;
            push(e);
        }
    }

    // The next frame's work is a known hitch.
    void known() { knownFrames.push_back(hitch.frames() + 1); }

    // The wheel over the transcript, `dy` px (negative scrolls up).
    void wheel(float dy) {
        PrAppEvent m{};
        m.kind = PrAppEventKind_PointerMove;
        m.pointer.position = PrVec2{extent.x / 2, extent.y / 2};
        push(m);
        PrAppEvent s{};
        s.kind = PrAppEventKind_Scroll;
        s.scroll.position = m.pointer.position;
        s.scroll.delta = PrVec2{0, dy};
        s.scroll.unit = PrAppScrollUnit_Pixel;
        push(s);
    }

    void command(char c, PrAppKeyCode code) { key(PrAppKey(c), code, style::kCommand); }

    void commandDigit(int digit) {
        const PrAppKeyCode code = digit == 0 ? PrAppKeyCode_Digit0 : PrAppKeyCode(PrAppKeyCode_Digit1 + digit - 1);
        key(PrAppKey('0' + digit), code, style::kCommand);
    }

    // Command-V in the prompt, the clipboard holding `text`: the prompt captures the key and asks the app for the
    // clipboard (View::paste), whose text comes back as a Paste event through the host (main.cpp pasted). `known`: the
    // frame that sets the text in the prompt is a known hitch.
    void commandV(const std::string& text, bool isKnown) {
        const int before = pastes;
        command('V', PrAppKeyCode_KeyV);
        CHECK(pastes == before + 1);
        PrAppEvent t{};
        t.kind = PrAppEventKind_Paste;
        t.input.text = PrStringView{text.data(), text.size()};
        if (isKnown) {
            known();
        }
        push(t);
        frame();
    }

    // The node under `node` whose text at the last layout is `text`, or kPrLayoutNodeNone.
    PrLayoutNode findText(PrLayoutNode node, const std::string& text) const {
        PrStringView t = prLayoutNodeText(tree, node);
        if (t.data != nullptr && std::string(t.data, t.size) == text) {
            return node;
        }
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, node); n != kPrLayoutNodeNone;
                n = prLayoutNodeNextSibling(tree, n)) {
            const PrLayoutNode found = findText(n, text);
            if (found != kPrLayoutNodeNone) {
                return found;
            }
        }
        return kPrLayoutNodeNone;
    }

    // The node under `node` whose text at the last layout holds `text` anywhere, or kPrLayoutNodeNone.
    PrLayoutNode findPart(PrLayoutNode node, const std::string& text) const {
        const PrStringView t =
            prLayoutNodeKind(tree, node) == PrLayoutNodeKind_Text ? prLayoutNodeText(tree, node) : PrStringView{};
        if (t.data != nullptr && std::string(t.data, t.size).find(text) != std::string::npos) {
            return node;
        }
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, node); n != kPrLayoutNodeNone;
                n = prLayoutNodeNextSibling(tree, n)) {
            const PrLayoutNode found = findPart(n, text);
            if (found != kPrLayoutNodeNone) {
                return found;
            }
        }
        return kPrLayoutNodeNone;
    }

    // True if any node of the window holds `text`: under the viewport's root, and under the top of the secret
    // dialog's field (which the kit may place in a layer of its own).
    bool holdsText(const std::string& text) const {
        const PrLayoutNode root = prLayoutTreeRoot(tree);
        if (root != kPrLayoutNodeNone && findPart(root, text) != kPrLayoutNodeNone) {
            return true;
        }
        PrLayoutNode top = view.secretField;
        if (top == kPrLayoutNodeNone || !prLayoutNodeIsValid(tree, top)) {
            return false;
        }
        while (prLayoutNodeParent(tree, top) != kPrLayoutNodeNone) {
            top = prLayoutNodeParent(tree, top);
        }
        return findPart(top, text) != kPrLayoutNodeNone;
    }

    // What the text nodes inside the secret dialog's field show, joined: Prax's edit shows its text, or a password's
    // mask (U+2022 for each code point), with a space after it when empty (ui_text_edit.cpp).
    std::string fieldShows() const {
        std::string out;
        std::function<void(PrLayoutNode)> add = [&](PrLayoutNode node) {
            for (PrLayoutNode n = prLayoutNodeFirstChild(tree, node); n != kPrLayoutNodeNone;
                    n = prLayoutNodeNextSibling(tree, n)) {
                const PrStringView t =
                    prLayoutNodeKind(tree, n) == PrLayoutNodeKind_Text ? prLayoutNodeText(tree, n) : PrStringView{};
                if (t.data != nullptr) {
                    out.append(t.data, t.size);
                }
                add(n);
            }
        };
        if (view.secretField != kPrLayoutNodeNone) {
            add(view.secretField);
        }
        return out;
    }

    bool prUiFocusIsField() const { return view.secretField != kPrLayoutNodeNone && prUiFocus(ui) == view.secretField; }

    // True if a line of the transcript reads `text` and shows whole in the transcript's view.
    bool showsLine(const std::string& text) const {
        const PrLayoutNode n = findText(view.transcript, text);
        if (n == kPrLayoutNodeNone) {
            return false;
        }
        const PrRect port = prLayoutRectToPx(prLayoutNodeBounds(tree, view.transcript, PrLayoutCoordinates_Viewport));
        const PrRect b = prLayoutRectToPx(prLayoutNodeBounds(tree, n, PrLayoutCoordinates_Viewport));
        return b.top >= port.top - 0.5f && b.bottom <= port.bottom + 0.5f;
    }
};

// A friend's history of `count` entries, as the mirror leaves it in the model after a long conversation: the person's
// prompts, the friend's replies (some long, wrapping over many lines), tool lines and notices.
void history(Model& model, std::size_t tab, int count, int salt = 0) {
    const std::string paragraph = "The layout of a long reply wraps across the column, and each line of it has to be set "
        "in the system's font before it can be drawn; a transcript of thousands of these is what a day of work leaves. ";
    for (int i = 0; i < count; ++i) {
        const int n = i + salt;
        switch (n % 5) {
            case 0:
                model.append(tab, Entry{EntryKind::Person, "question " + std::to_string(n) + ": " + paragraph});
                break;
            case 1:
            case 2: {
                std::string reply;
                for (int p = 0; p < 1 + n % 4; ++p)
                    reply += paragraph;
                model.append(tab, Entry{EntryKind::Friend, reply});
                break;
            }
            case 3:
                model.append(tab, Entry{EntryKind::Tool, "Read src/view.cpp (lines " + std::to_string(n) + ")"});
                break;
            default:
                model.append(tab, Entry{EntryKind::Notice, "read 3 files"});
                break;
        }
    }
}

// A state of the server: one session per friend, the friend's activity, and each tail.
sync::Sessions stateOf(const Model& model, sync::Activity activity, const std::string& open = std::string(),
        std::uint64_t first = 0, const std::vector<std::string>& closed = {}) {
    std::vector<sync::SessionRef> sessions;
    sync::SessionId id = 1;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind != TabKind::Friend)
            continue;
        auto v = std::make_shared<sync::SessionView>();
        v->id = id++;
        v->info.friendName = model.tab(i).name;
        v->activity = activity;
        v->first = first;
        for (const std::string& text : closed)
            v->entries.push_back(std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::Friend, text, true}));
        if (!open.empty())
            v->entries.push_back(std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::Friend, open, false}));
        sessions.push_back(std::move(v));
    }
    return sync::Sessions(std::move(sessions));
}

LinkState connected(sync::Sessions sessions, std::uint64_t heardMsAgo = 0) {
    LinkState s;
    s.phase = sync::LinkPhase::Connected;
    s.heardMsAgo = heardMsAgo;
    s.sessions = std::move(sessions);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// The cases

struct Case {
    const char* name;
    void (*run)(Window& w);
    // A known hitch whose fix is outside nexus (docs/frame-budget.md, known hitches): the frames the case marks known
    // (Window::known) may pass the budget while under `ceiling` ms, the worst measured on the studio and about 15%,
    // so that a regression on nexus's side still fails; every other frame of the case keeps the budget. Null for every
    // case nexus keeps within the budget.
    const char* known = nullptr;
    double ceiling = 0;
};

// The window comes up connected on a friend's tab, and its first frames are drawn.
void firstFrames(Window& w) {
    w.link.set(connected(stateOf(w.model, sync::Activity::Idle)));
    w.frames(30);
}

// Typing into the prompt over a friend's tab with a history of 5,000 entries, the busy cursor breathing.
void typingOverALongHistory(Window& w) {
    history(w.model, 1, 5000);
    w.link.set(connected(stateOf(w.model, sync::Activity::Busy)));
    w.frames(10);
    w.type("a prompt typed over a long history");
    w.frames(60);
}

// The server stalls (nothing heard for seconds, the state cursor goes grey), comes back, and a long transcript
// arrives at once: thousands of entries for a friend whose tab shows.
void stalledLinkThenALongTranscript(Window& w) {
    w.link.set(connected(stateOf(w.model, sync::Activity::Idle)));
    w.frames(10);
    w.link.set(connected(stateOf(w.model, sync::Activity::Idle), 5000));
    w.frames(10);
    w.link.set(connected(stateOf(w.model, sync::Activity::Idle), 12000));
    w.frames(10);
    // Back, with the whole history the window was away for: the mirror appends what it had not shown.
    history(w.model, 1, 4000, 7);
    w.link.set(connected(stateOf(w.model, sync::Activity::Busy)));
    w.frames(60);
}

// A reply streams into the open entry, a few hundred bytes a frame, to 32 KB (the server's text limit).
void streamingReply(Window& w) {
    history(w.model, 1, 200);
    std::string open;
    const std::string chunk = "streamed text arrives in pieces as the friend writes it, and the window shows it as it "
        "grows; ";
    for (int i = 0; open.size() < 32 * 1024; ++i) {
        open += chunk;
        if (i % 6 == 5)
            open += "\n\n";
        w.link.set(connected(stateOf(w.model, sync::Activity::Busy, open)));
        w.frame();
    }
}

// The person pastes 1 MB into the prompt with Command-V, edits the full prompt, switches tabs, selects it all and types
// over it, pastes 40 KB and sends it. The frames that set or edit a full prompt's text are the known hitch; every other
// frame keeps the budget.
void bigPaste(Window& w) {
    history(w.model, 1, 50);
    w.link.set(connected(stateOf(w.model, sync::Activity::Idle)));
    w.frames(5);
    std::string big;
    while (big.size() < 1024 * 1024) {
        big += "a pasted line of a log, long enough to wrap, with numbers 0123456789 and words in it\n";
    }
    const std::size_t alex = w.model.tab(1).id;
    w.commandV(big, true);
    // Emma's tab the next frame: the paste went into alex's prompt whole, in its frame, and none of it follows to emma.
    w.commandDigit(3);
    CHECK(w.model.tab(1).draft == big.substr(0, kPromptBytes));
    CHECK(w.model.tab(2).draft.empty());
    CHECK(w.view.cut == cutLine(big.size(), kPromptBytes) && w.view.cutTab == alex);
    w.known();          // alex's full prompt set in the area again
    w.commandDigit(2);
    w.frames(10);
    // The end of alex's transcript says how much was left out, in sight.
    CHECK(w.showsLine(w.view.cut));
    // Edits at the end of a full prompt: four characters taken back and typed again, each a known hitch. The line
    // about the cut goes with the first edit.
    for (int i = 0; i < 4; ++i) {
        w.known();
        w.key(PrAppKey_Backspace, PrAppKeyCode_Backspace);
    }
    CHECK(w.view.cut.empty());
    for (const char c : std::string("more")) {
        const char s[2] = {c, 0};
        w.known();
        w.key(PrAppKey(c), 0, 0, s);
    }
    CHECK(w.model.tab(1).draft.size() == kPromptBytes);
    // Typed at the limit: nothing goes in, and the line says the byte was left out.
    w.type("z");
    CHECK(w.model.tab(1).draft.size() == kPromptBytes && w.view.cut == cutLine(1, 0));
    // The line is alex's: emma's tab shows none, and her prompt took nothing of alex's text; back on alex it shows.
    w.commandDigit(3);
    w.frames(3);
    CHECK(w.model.tab(2).draft.empty());
    CHECK(w.findText(w.view.transcript, w.view.cut) == kPrLayoutNodeNone);
    w.known();          // alex's full prompt set in the area again
    w.commandDigit(2);
    w.frames(3);
    CHECK(w.showsLine(w.view.cut));
    // At the limit, the whole prompt selected and typed over: the selection is room.
    w.command('A', PrAppKeyCode_KeyA);
    w.type("x");
    CHECK(w.model.tab(1).draft == "x");
    CHECK(w.view.cut.empty());
    // 40 KB pasted after the "x", then sent: the whole 32 KB goes, the paste in one piece.
    const std::string forty = big.substr(0, 40 * 1024);
    w.commandV(forty, true);
    CHECK(w.model.tab(1).draft == "x" + forty.substr(0, kPromptBytes - 1));
    const std::size_t sent = w.link.sent.size();
    w.key(PrAppKey_Enter, PrAppKeyCode_Enter);
    CHECK(w.link.sent.size() == sent + 1 && w.link.sent.back().text == "x" + forty.substr(0, kPromptBytes - 1));
    CHECK(w.model.tab(1).draft.empty() && w.view.cut.empty());
    w.frames(30);
}

// Switching between friends with long histories, and to the sprint tab and back.
void switchingTabs(Window& w) {
    history(w.model, 1, 3000);
    history(w.model, 2, 3000, 1);
    history(w.model, 3, 3000, 2);
    w.link.set(connected(stateOf(w.model, sync::Activity::Idle)));
    w.frames(5);
    for (int round = 0; round < 3; ++round) {
        w.commandDigit(2);
        w.frames(3);
        w.commandDigit(3);
        w.frames(3);
        w.commandDigit(4);
        w.frames(3);
    }
}

// The sprint tab shown for the first time (its page made, loaded and painted), polled for a few seconds, and left.
void sprintTab(Window& w) {
    w.frames(5);
    w.commandDigit(1);
    w.until([&] { return w.board->settled(); }, 600);
    w.frames(360, true);
    w.commandDigit(2);
    w.frames(10);
    w.commandDigit(1);
    w.frames(60, true);
}

// Terminal tabs opened with Command-T, one filled with 10,000 lines, typed into, and closed with Command-W; the
// preference files take 200 ms each write, and the terminals and selection change as tabs open and close.
void terminals(Window& w) {
    w.frames(5);
    // Typed into a new tab before its shell has started: every key is held, and reaches the shell in order.
    w.command('T', PrAppKeyCode_KeyT);
    const std::size_t first = w.model.tab(w.model.active()).id;
    const std::string early = "echo held-keys-arrived\r";
    w.typeAtOnce(early);
    CHECK(w.terminals->held(first) == 3 * early.size());
    const bool arrived = w.until([&] { return w.terminals->text(first).find("\nheld-keys-arrived") !=
        std::string::npos; }, 600);
    if (!arrived) {
        const std::string text = w.terminals->text(first);
        std::fprintf(stderr, "  screen (%zu bytes): %s\n", text.size(), text.substr(0, 400).c_str());
    }
    CHECK(arrived);
    CHECK(w.terminals->held(first) == 0);
    // The real terminal paints its default background on the window itself. A second layer here would
    // make the terminal darker than the conversation under the same policy.
    const auto root = prLayoutTreeRoot(w.tree);
    const auto pane = prLayoutNodeFirstChild(w.tree, root);
    auto background = [&](PrLayoutNode node) {
        const auto style = prLayoutStyleRecord(w.tree, prLayoutNodeStyle(w.tree, node));
        return prLayoutDecorationStyleRecord(w.tree, style.groups[PrLayoutGroup_Decoration]).background;
    };
    CHECK(background(root).a == 0);
    CHECK(background(pane).a > 0.595f && background(pane).a < 0.605f);
    w.frames(30, true);
    for (int i = 0; i < 2; ++i) {
        w.command('T', PrAppKeyCode_KeyT);
        w.frames(30, true);
    }
    w.type("seq 1 10000\r");
    const std::size_t id = w.model.tab(w.model.active()).id;
    const bool filled = w.until([&] { return w.terminals->text(id).find("\n10000 ") != std::string::npos; }, 1200);
    if (!filled) {
        const std::string text = w.terminals->text(id);
        std::fprintf(stderr, "  screen (%zu bytes): %s\n", text.size(), text.substr(0, 400).c_str());
    }
    CHECK(filled);
    w.frames(30, true);
    for (int i = 0; i < 3; ++i) {
        w.command('W', PrAppKeyCode_KeyW);
        w.frames(30, true);
    }
    // A shell that cannot start: its tab shows at once and closes once the start thread has failed.
    w.terminals->program = "/no/such/shell";
    const std::size_t tabs = w.model.tab_count();
    w.command('T', PrAppKeyCode_KeyT);
    CHECK(w.model.tab_count() == tabs + 1);
    CHECK(w.until([&] { return w.model.tab_count() == tabs; }, 600));
    w.terminals->program = "/bin/sh";
}

// Command-plus and Command-minus with a long history and a terminal holding 10,000 lines of scrollback.
void textScale(Window& w) {
    history(w.model, 1, 3000);
    w.command('T', PrAppKeyCode_KeyT);
    w.frames(20, true);
    w.type("seq 1 10000\r");
    const std::size_t id = w.model.tab(w.model.active()).id;
    CHECK(w.until([&] { return w.terminals->text(id).find("\n10000 ") != std::string::npos; }, 1200));
    w.commandDigit(2);
    w.frames(5);
    for (int i = 0; i < 3; ++i) {
        w.key(PrAppKey('='), PrAppKeyCode_Equal, style::kCommand);
        w.frames(5);
    }
    w.key(PrAppKey('-'), PrAppKeyCode_Minus, style::kCommand);
    w.frames(5);
    w.commandDigit(5);
    w.frames(10, true);
}

// The window resized step by step, as a live resize does, over a long history.
void resizing(Window& w) {
    history(w.model, 1, 3000);
    w.frames(5);
    for (int i = 0; i < 60; ++i) {
        w.extent = PrVec2{1200.0f - 8 * float(i), 800.0f - 4 * float(i)};
        w.frame();
    }
}

// Text that needs fallback fonts (emoji, CJK, Arabic) arrives for the first time.
void fallbackFonts(Window& w) {
    history(w.model, 1, 20);
    w.frames(5);
    w.model.append(1, Entry{EntryKind::Friend, "done \xF0\x9F\x8E\x89\xF0\x9F\x9A\x80 \xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96"
        "\xE7\x95\x8C \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"});
    w.known();      // the first frame that sets the text: the kit's fonts match their fallback faces
    w.frames(20);
}

// A long history scrolled up from its end a screen at a time, then back: each step declares items not laid out
// before, whose estimated heights the next round replaces with measured ones.
void scrollingUpALongHistory(Window& w) {
    history(w.model, 1, 5000);
    w.frames(5);
    for (int i = 0; i < 60; ++i) {
        w.wheel(-600);
        w.frame();
    }
    for (int i = 0; i < 60; ++i) {
        w.wheel(600);
        w.frame();
    }
    w.frames(10);
}

// The secret dialog (docs/secrets.md) over a friend's tab with a long history: a request arrives in the server's state
// and the dialog opens; the person types a few characters, takes them back, pastes a token, and presses Enter: the
// value goes to the link in one action and the dialog closes, and the next state, without the request, keeps it
// closed. A second request is cancelled with Escape. Every frame within the budget. The values are made up here.
std::string bullets(std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out += "\xe2\x80\xa2";
    }
    return out;
}

void secretDialog(Window& w) {
    history(w.model, 1, 3000);
    sync::Sessions state = stateOf(w.model, sync::Activity::Busy);
    w.link.set(connected(state));
    w.frames(5);
    const std::string token = "fake-frame-test-token-" + std::string(200, 'k');
    state.setRequests({sync::SecretRequest{7, "work", "oauth-token", 1, "alex", "the login expired"}});
    w.link.set(connected(state));
    w.frames(5);
    CHECK(w.view.secretShown == 7 && w.view.secretField != kPrLayoutNodeNone);
    CHECK(w.prUiFocusIsField());
    // The dialog names the account, what is asked, who asks and why; the empty field shows no text (no placeholder).
    CHECK(w.holdsText("work: oauth-token") && w.holdsText("alex") && w.holdsText("the login expired"));
    CHECK(w.fieldShows() == " ");
    w.type("abc");
    CHECK(w.fieldShows() == bullets(3) && !w.holdsText("abc"));
    for (int i = 0; i < 3; ++i) {
        w.key(PrAppKey_Backspace, PrAppKeyCode_Backspace);
    }
    CHECK(w.host.secrets.typed().empty());
    PrAppEvent paste{};
    paste.kind = PrAppEventKind_Paste;
    const std::string pasted = token + "\n";
    paste.input.text = PrStringView{pasted.data(), pasted.size()};
    w.push(paste);
    w.frame();
    // A one-line field: the paste's line break does not go in.
    CHECK(w.host.secrets.typed() == token);
    // Drawn masked, a bullet for each byte of it (all ASCII): no node of the window holds the value's text.
    CHECK(w.fieldShows() == bullets(token.size()) && !w.holdsText(token));
    // Nothing of it reached the prompt.
    CHECK(w.model.tab(1).draft.empty());
    const std::size_t sent = w.link.sent.size();
    w.key(PrAppKey_Enter, PrAppKeyCode_Enter);
    CHECK(w.link.sent.size() == sent + 1);
    if (w.link.sent.size() == sent + 1) {
        const sync::Action& a = w.link.sent.back();
        CHECK(a.kind == sync::ActionKind::SecretAnswer && a.request == 7 && a.secret.bytes() == token);
    }
    CHECK(w.view.secretShown == 0 && w.host.secrets.typed().empty());
    w.frames(5);
    // The server's next state still holds the request (the answer is on its way): the dialog stays closed.
    CHECK(w.view.secretShown == 0);
    state.setRequests({});
    w.link.set(connected(state));
    w.frames(5);
    CHECK(w.view.secretShown == 0);
    // A second request, cancelled with Escape.
    state.setRequests({sync::SecretRequest{8, "work", "oauth-token", 0, "", ""}});
    w.link.set(connected(state));
    w.frames(5);
    CHECK(w.view.secretShown == 8);
    w.type("x");
    w.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(w.link.sent.size() == sent + 2);
    if (w.link.sent.size() == sent + 2) {
        const sync::Action& a = w.link.sent.back();
        CHECK(a.kind == sync::ActionKind::SecretCancel && a.request == 8 && a.secret.empty());
    }
    CHECK(w.view.secretShown == 0);
    w.frames(30);
}

// In-app sign-in (docs/secrets.md "Sign-in") over a friend's tab with a long history, the server on another machine
// ("server.test"): a flow is offered and the overlay shown, so the window claims it; the state then gives this window
// the URL, so it binds its gate (the real one, on its own thread) and opens it through the window's own path, a
// UrlOpener (url_opener.h) as main.cpp gives it, over a stub browser that takes 300 ms to answer, as a slow Launch
// Services or ShellExecute may: no frame waits on it. It says it opened it; the flow then leaves the state and the
// gate closes. The URL is made up, and no node of the window holds it.
void signIn(Window& w) {
    std::atomic<int> browsed{0};
    UrlOpener urls([&browsed](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ++browsed;
    });
    w.host.signIn.openUrl = urls.handle();
    history(w.model, 1, 3000);
    sync::Sessions state = stateOf(w.model, sync::Activity::Down);
    w.link.set(connected(state));
    w.frames(5);
    const std::string url = "https://claude.com/cai/oauth/authorize?code=true&client_id=fake-frame-client"
                            "&redirect_uri=http%3A%2F%2Flocalhost%3A54611%2Fcallback&state=fakeStFrameQz8";
    sync::SignInOffer offer;
    offer.id = 4;
    offer.account = "claude-main";
    offer.provider = "claude";
    offer.host = "server.test";
    state.setOffers({offer});
    w.link.set(connected(state));
    w.host.signIn.shown();
    const std::size_t sent = w.link.sent.size();
    w.frames(5);
    CHECK(w.link.sent.size() == sent + 1);
    if (w.link.sent.size() == sent + 1) {
        CHECK(w.link.sent.back().kind == sync::ActionKind::SignInClaim && w.link.sent.back().request == 4);
    }
    offer.opener = w.host.signIn.opener.token();
    offer.url = sync::SecretValue(url);
    state.setOffers({offer});
    w.link.set(connected(state));
    w.frames(5);
    CHECK(w.link.sent.size() == sent + 2);
    if (w.link.sent.size() == sent + 2) {
        CHECK(w.link.sent.back().kind == sync::ActionKind::SignInOpened && w.link.sent.back().request == 4);
    }
    CHECK(!w.holdsText("claude.com/cai") && !w.holdsText("fakeStFrameQz8"));
    w.type("typed while the sign-in waits");
    w.frames(10);
    state.setOffers({});
    w.link.set(connected(state));
    w.frames(10);
    CHECK(!w.host.signIn.gateOpen());
    w.frames(10);
    // The stub browser was handed the page once, on the opener's thread.
    for (int i = 0; i < 200 && browsed.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(browsed.load() == 1 && urls.opened() == 1);
    w.host.signIn.openUrl = nullptr;
}

// The real link over an ssh that hangs for seconds before it fails: the link's thread waits, never the window.
void slowSshLink(Window& w) {
    link::ClientLink::Options options;
    options.ip = "127.0.0.1";
    options.bootstrap.ssh = slowSsh;
    options.bootstrap.host = "stalled.test";
    link::ClientPort port(std::move(options));
    w.host.link = &port;
    w.view.promptOpen = false;
    w.frames(5);
    w.command('W', PrAppKeyCode_KeyW);      // nothing to close: a key while the link hangs
    w.type("typed while the link hangs");
    w.until([&] { return w.view.status.find("stalled") != std::string::npos && w.host.phase ==
        sync::LinkPhase::Failed; }, 600);
    w.frames(10);
    CHECK(w.host.phase == sync::LinkPhase::Failed);
    w.host.link = &w.link;
}

// The server's list of 2,000 friends while a reply streams into a friend's tab. The window's first frames come before
// the link does, as in the app; the list arrives with the stream's first state, its first 256 friends' tabs added
// that frame (sync/client.h kMaxFriendTabs); then each frame's state carries the stream alone, as the link brings it:
// the server's state each frame holds a list equal to the one before, so its diff carries none
// (Sessions::diff_from), and the window's state keeps the list it holds (Sessions::friendsList), which the window does
// not take again (sync/friend_strip.h): no ensure is sent after the first. Every frame keeps the budget.
void manyFriendsWhileStreaming(Window& w) {
    history(w.model, 1, 200);
    w.frames(5);
    std::vector<sync::ServerFriend> list{{"alex"}, {"emma"}, {"stella"}};
    for (int i = 0; list.size() < 2000; ++i) {
        list.push_back(sync::ServerFriend{"friend-" + std::to_string(i)});
    }
    sync::Sessions sent;    // the server's last state
    sync::Sessions held;    // the window's, each state's diff applied to it as the link's thread does
    std::string open;
    const std::string chunk = "streamed text arrives in pieces as the friend writes it, and the window shows it as it "
        "grows; ";
    while (open.size() < 16 * 1024) {
        open += chunk;
        auto v = std::make_shared<sync::SessionView>();
        v->id = 1;
        v->info.friendName = "alex";
        v->activity = sync::Activity::Busy;
        v->entries.push_back(std::make_shared<sync::SessionEntry>(sync::SessionEntry{EntryKind::Friend, open, false}));
        sync::Sessions next(std::vector<sync::SessionRef>{std::move(v)});
        next.setFriends(list);
        held.apply_string(next.diff_from(sent));
        sent = std::move(next);
        w.link.set(connected(held));
        w.frame();
        CHECK(held.friendsList() == w.link.state.sessions.friendsList());
    }
    // The sprint tab and the first 256 friends, alex, emma and stella among them; an ensure for each new one, once.
    CHECK(w.model.tab_count() == 1 + sync::kMaxFriendTabs);
    CHECK(w.link.sent.size() == sync::kMaxFriendTabs - 3);
    w.frames(10);
}

const Case kCases[] = {
    {"many_friends_while_streaming", manyFriendsWhileStreaming},
    {"first_frames", firstFrames},
    {"typing_over_a_long_history", typingOverALongHistory},
    {"stalled_link_then_a_long_transcript", stalledLinkThenALongTranscript},
    {"streaming_reply", streamingReply},
    {"big_paste", bigPaste, "the frame that sets 32 KB in the prompt, and each edit of a full prompt, shapes the whole "
        "prompt (Prax's text area lays its text out whole at each change: 11 ms at 32 KB); nexus cuts a paste to the "
        "32 KB the server takes", 15},
    {"switching_tabs", switchingTabs},
    {"sprint_tab", sprintTab},
    {"terminals", terminals},
    {"text_scale", textScale},
    {"resizing", resizing},
    {"scrolling_up_a_long_history", scrollingUpALongHistory},
    {"fallback_fonts", fallbackFonts, "each text font of the kit matches the fallback faces on the frame that first "
        "needs one (Prax text_font.cpp matchFallback); nexus loads the faces ahead (font_prefetch.h), Prax has no way "
        "to match a kit's text fonts ahead or off the frame thread", 145},
    {"secret_dialog", secretDialog},
    {"sign_in", signIn},
    {"slow_ssh_link", slowSshLink},
};

// What a case's frames came to.
struct Outcome {
    int hitched = 0;        // frames over the budget in every run that are not known: each fails the test
    int known = 0;          // known frames over the budget, each under the case's ceiling
};

// The case and frame given 10 ms of work (--inject CASE FRAME): the proof that a real hitch fails every time.
const char* injectCase = nullptr;
std::uint64_t injectAt = 0;

// The runs a case gets at most. A real hitch is work on the frame thread and comes back on the same frame every run;
// a frame the scheduler held up (the host loaded with other jobs) does not. A frame fails only if it is over the
// budget in every run.
constexpr int kRuns = 3;

// One run of a case: its frame count, the frames that fail in it alone, and its known frames under the ceiling.
struct Pass {
    std::uint64_t frames = 0;
    std::vector<Over> over;
    int known = 0;
};

// The phase with the most of the hitch's time.
const char* mostPhase(const Hitch& h) {
    std::size_t most = 0;
    for (std::size_t i = 1; i < h.phaseCount; ++i) {
        if (h.phases[i].ms > h.phases[most].ms) {
            most = i;
        }
    }
    return h.phaseCount == 0 ? "none" : phaseName(h.phases[most].phase);
}

// Runs the case once in a window of its own and prints every hitch: its frame, its work, its CPU time and its phases.
// A hitch fails the run unless its frame is one the case marked known (Window::known) and its work is under the case's
// ceiling.
Pass once(const Case& c, int run) {
    Window w;
    if (injectCase != nullptr && std::strcmp(injectCase, c.name) == 0) {
        w.injectFrame = injectAt;
    }
    const double started = prTimeMonotonicMilliseconds();
    c.run(w);
    const double took = prTimeMonotonicMilliseconds() - started;
    std::printf("%-40s %5llu frames, worst %6.2f ms, %llu hitches (%.1f s)%s\n", c.name,
        static_cast<unsigned long long>(w.hitch.frames()), w.hitch.worst(),
        static_cast<unsigned long long>(w.hitch.hitches()), took / 1000,
        run == 1 ? "" : run == 2 ? " [run 2]" : " [run 3]");
    const std::size_t phases = w.firstLine.find("): ");
    std::printf("  %s: first frame %.2f ms, thread cpu %.2f ms: %s\n", c.name, w.firstWall, w.firstCpu,
        phases == std::string::npos ? w.firstLine.c_str() : w.firstLine.c_str() + phases + 3);
    Pass pass;
    pass.frames = w.hitch.frames();
    if (w.firstWall > kFirstFrameMs) {
        Over o;
        o.frame = 1;
        o.events = w.firstEvents;
        o.wall = w.firstWall;
        o.cpu = w.firstCpu;
        o.phase = mostPhase(w.firstHitch);
        o.line = w.firstLine;
        o.first = true;
        pass.over.push_back(o);
    }
    if (w.hitch.hitches() > w.hitch.held()) {
        std::printf("  %s: %llu hitches, more than the detector holds: the ones not held count as hitches\n", c.name,
            static_cast<unsigned long long>(w.hitch.hitches()));
        Over o;
        o.phase = "unheld";
        o.line = "more hitches than the detector holds";
        pass.over.push_back(o);
    }
    if (c.known == nullptr && !w.knownFrames.empty() && run == 1) {
        std::printf("  %s: marks known frames but has no known reason\n", c.name);
        ++failures;
    }
    for (std::size_t i = 0; i < w.hitch.held(); ++i) {
        const Hitch& h = w.hitch.hitch(i);
        const bool marked = std::find(w.knownFrames.begin(), w.knownFrames.end(), h.frame) != w.knownFrames.end();
        const bool known = c.known != nullptr && marked && h.work <= c.ceiling;
        const auto info = w.frameInfo.find(h.frame);
        const double cpu = info != w.frameInfo.end() ? info->second.second : 0;
        char line[1024];
        HitchDetector::format(h, w.hitch.budget(), line, sizeof line);
        std::printf("  %s: %s thread cpu %.2f ms%s\n", c.name, line, cpu,
            known ? " (known)" : marked ? " (known frame, past the ceiling)" : "");
        if (known) {
            ++pass.known;
            continue;
        }
        Over o;
        o.frame = h.frame;
        o.events = info != w.frameInfo.end() ? info->second.first : 0;
        o.wall = h.work;
        o.cpu = cpu;
        o.phase = mostPhase(h);
        o.line = line;
        pass.over.push_back(o);
    }
    return pass;
}

// The same frame in another run: the same number when both runs had as many frames, else the same place among the
// case's input events with the same phase the most (cases that wait on real time have a frame count of their own each
// run).
const Over* again(const Over& o, std::uint64_t frames, const Pass& other) {
    for (const Over& p : other.over) {
        const bool same = frames == other.frames ? p.frame == o.frame : p.events == o.events && p.phase == o.phase;
        if (same && p.first == o.first) {
            return &p;
        }
    }
    return nullptr;
}

// Runs a case up to kRuns times and reports each frame over the budget: a frame over it in every run fails, named with
// its phase and every run's wall-clock and CPU time; one that is not over again was the host, and is said so.
Outcome run(const Case& c) {
    Outcome out;
    std::vector<Pass> passes;
    passes.reserve(kRuns);      // `still` points into passes[0]
    passes.push_back(once(c, 1));
    out.known = passes[0].known;
    std::vector<const Over*> still;
    for (const Over& o : passes[0].over) {
        still.push_back(&o);
    }
    for (int r = 2; r <= kRuns && !still.empty(); ++r) {
        passes.push_back(once(c, r));
        std::vector<const Over*> next;
        for (const Over* o : still) {
            if (again(*o, passes[0].frames, passes.back()) != nullptr) {
                next.push_back(o);
            } else {
                std::printf("  %s: frame %llu (%s) at %.2f ms, cpu %.2f ms: not over %s again in run %d, the host held "
                    "it up\n", c.name, static_cast<unsigned long long>(o->frame), o->phase.c_str(), o->wall, o->cpu,
                    o->first ? "the first frame's 4 ms" : "the budget", r);
            }
        }
        still = next;
    }
    for (const Over* o : still) {
        std::printf("  %s: %s in every run: frame %llu, phase %s:", c.name,
            o->first ? "FIRST FRAME over 4 ms" : "HITCH", static_cast<unsigned long long>(o->frame), o->phase.c_str());
        for (std::size_t r = 0; r < passes.size(); ++r) {
            const Over* p = r == 0 ? o : again(*o, passes[0].frames, passes[r]);
            std::printf(" %.2f ms (cpu %.2f)", p->wall, p->cpu);
        }
        std::printf("\n");
        ++out.hitched;
    }
    return out;
}

// What the detector itself costs a frame: a frame's markers (begin, a dozen scopes, Prax's three phases, end) on the
// real clock, many times over, with no work between them.
void detectorCost() {
    Lines lines;
    HitchDetector d{HitchClock{realMs, nullptr}, HitchLog{keepLine, &lines}};
    constexpr int kFrames = 200000;
    const double started = prTimeMonotonicMilliseconds();
    for (int f = 0; f < kFrames; ++f) {
        d.begin();
        for (int s = 0; s < 6; ++s) {
            HitchScope scope(&d, Phase(s));
        }
        for (std::uint32_t t = 0; t < 6; ++t) {
            HitchScope scope(&d, Phase::TerminalTab, t);
        }
        d.add(Phase::Layout, 1);
        d.add(Phase::Draw, 1);
        d.add(Phase::Present, 1);
        d.end();
    }
    const double perFrameUs = (prTimeMonotonicMilliseconds() - started) * 1000 / kFrames;
    std::printf("detector overhead: %.3f us per frame (12 scopes, 3 phases added, begin and end)\n", perFrameUs);
    CHECK(d.frames() == kFrames);
    // Tiny next to the budget: under 1% of 8.3 ms.
    CHECK(perFrameUs < 83);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: nexus_frame_tests SLOW_SSH [--inject CASE FRAME] [CASE...]\n");
        return 2;
    }
    slowSsh = argv[1];
#if defined(__APPLE__)
    // The frames run at the QoS of an app's main thread, user-interactive, as the window's own frames do: on a loaded
    // host the scheduler keeps it on the performance cores, where a default-QoS thread may be moved to the efficiency
    // cores and do the same work in twice the time.
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0);
#endif
    int first = 2;
    if (argc >= 5 && std::strcmp(argv[2], "--inject") == 0) {
        injectCase = argv[3];
        injectAt = std::strtoull(argv[4], nullptr, 10);
        first = 5;
    }
    detectorCost();
    int hitched = 0;
    int known = 0;
    int ran = 0;
    for (const Case& c : kCases) {
        bool chosen = argc == first;
        for (int i = first; i < argc && !chosen; ++i) {
            chosen = std::strcmp(argv[i], c.name) == 0;
        }
        if (!chosen) {
            continue;
        }
        ++ran;
        const Outcome o = run(c);
        hitched += o.hitched != 0 ? 1 : 0;
        if (c.known != nullptr) {
            known += o.known != 0 ? 1 : 0;
            std::printf("  %s: %s: %s\n", c.name, o.known != 0 ? "known" :
                "every frame within the budget now; the known mark can go", c.known);
        }
    }
    if (ran == 0) {
        std::fprintf(stderr, "no case of that name\n");
        return 2;
    }
    if (hitched != 0 || failures != 0) {
        std::fprintf(stderr, "%d of %d cases had a frame over the %.2f ms budget or a first frame over %.0f ms in every "
            "run; %d other failures\n", hitched, ran, 1000.0 / 120, kFirstFrameMs, failures);
        return 1;
    }
    std::printf("every frame of %d cases within the %.2f ms budget, but for %d known cases; every first frame within "
        "%.0f ms\n", ran - known, 1000.0 / 120, known, kFirstFrameMs);
    return 0;
}
