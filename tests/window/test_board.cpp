// The sprint tab's page (src/board.h) over Prax's HTML stack, headless: a fixture board served from memory through the
// board's test network (Prax's null network and an intercepting fetcher: no socket, no live board), shown by the nexus
// view over a Prax UI context with no window and no GPU. The page's clocks are made up and stepped by the test; the
// fixture's script uses what the served board uses: a 1 s poll of /api/sprint with fetch, Intl.DateTimeFormat's parts,
// toLocaleTimeString, toLocaleString on Number and BigInt, session storage and a reload on a new build. Text is set in
// the system's fonts and scripts run in QuickJS: a functional test.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <regex>
#include <string>
#include <vector>

#include "pr/css_view.h"
#include "pr/dom.h"
#include "pr/font_set.h"
#include "pr/html.h"
#include "pr/kit.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "board.h"
#include "model.h"
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

bool has(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

constexpr const char* kUrl = "http://board.test:8080/";

// The fixture: a dark page whose own background is the board's --bg, with an opaque tile, and the board's script
// habits.
constexpr const char* kPage = R"HTML(<!doctype html>
<html lang="en" data-theme="dark"><head><meta charset="utf-8"><title>fixture</title>
<style>:root, :root[data-theme="dark"] { --bg: #0d0f12; } body { background: var(--bg); margin: 0; }
.tile { background: #14171b; }</style></head>
<body><div class="tile" id="n">-</div><div id="clock">-</div><div id="zone">-</div><div id="polls">0</div>
<div id="again">first</div>
<script>
var polls = 0, build = null;
try { if (sessionStorage.getItem("reloaded") === "yes") document.getElementById("again").textContent = "reloaded"; } catch (e) {}
function zone(d) {
  var p = new Intl.DateTimeFormat("en-US", { timeZoneName: "short" }).formatToParts(d)
    .filter(function (x) { return x.type === "timeZoneName"; })[0];
  return p ? p.value : "";
}
function tick() {
  var d = new Date();
  var parts = new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit", second: "2-digit", hour12: true }).formatToParts(d);
  var period = parts.filter(function (q) { return q.type === "dayPeriod"; })[0];
  document.getElementById("clock").textContent = d.toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit", second: "2-digit" }) + "|" + (period ? period.value : "");
  document.getElementById("zone").textContent = zone(d);
}
function poll() {
  fetch("/api/sprint", { cache: "no-store" }).then(function (r) { return r.json(); }).then(function (j) {
    polls++;
    document.getElementById("polls").textContent = "polls " + polls;
    document.getElementById("n").textContent = "landed " + j.landed.toLocaleString("en-US") + " cents " + BigInt(j.cents).toLocaleString("en-US");
    if (build == null) build = j.build;
    else if (build !== j.build) { sessionStorage.setItem("reloaded", "yes"); location.reload(); }
  });
}
tick(); poll(); setInterval(function () { tick(); poll(); }, 1000);
</script></body></html>)HTML";

// The served board's first screen in its shape: a header and a row of tiles, then the Cost breakdown panel (the pie in
// a column of its own beside the table) and the next block below it, sized in rem as the board sizes them.
constexpr const char* kSpendPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>spend</title>
<style>html, body { margin: 0; } body { padding: 2rem 2rem 4rem; }
.top { height: 7.5rem; margin-bottom: 2rem; } .hero { height: 10rem; margin-bottom: 2rem; }
.panel { border: 1px solid #333; border-radius: 1.5rem; overflow: hidden; }
.panel-head { padding: 2rem 3rem 1rem; } .panel.folded > .scroll { display: none; }
.spend { margin-bottom: 4rem; }
.spend-body { display: grid; grid-template-columns: 3fr 7fr; align-items: stretch; }
.pie-wrap { display: flex; align-items: flex-start; justify-content: center; padding: 2rem 3rem; }
.pie { width: 100%; max-width: 12rem; aspect-ratio: 1 / 1; height: auto; }
.next { height: 20rem; } .rest { height: 3000px; }</style></head>
<body><header class="top">board</header><section class="hero" id="hero">tiles</section>
<section class="panel spend" id="spend"><div class="panel-head"><h2>Cost breakdown</h2></div>
<div class="scroll spend-body"><div class="pie-wrap"><svg class="pie" id="pie" viewBox="0 0 100 100"></svg></div>
<div class="rows toprows"><div>stream</div><div>a</div><div>b</div></div></div></section>
<section class="next" id="next">cards by state</section><section class="rest">work</section>
</body></html>)HTML";

// The board's links: one to another site (the repo's), one to another page of the board's server, one to a place on
// the board but with target _blank, one to a place on the board, one the page's own script handles, and one to
// another site that the page cancels itself.
constexpr const char* kLinksPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>links</title>
<style>html, body { margin: 0; } a { display: block; height: 40px; } .far { height: 3000px; }</style></head>
<body><a id="repo" href="https://github.test/nova-sprint">nova-sprint</a>
<a id="card" href="/cards/12">card 12</a>
<a id="blank" href="#later" target="_blank">later, new tab</a>
<a id="here" href="#later"><span id="inner">later</span></a>
<a id="script" href="javascript:void(document.title='ran')">run</a>
<a id="kept" href="https://github.test/kept">kept</a>
<div class="far"></div><div id="later">later</div>
<script>document.getElementById("kept").addEventListener("click", function (e) { e.preventDefault(); });</script>
</body></html>)HTML";

// The served board's header and footer in their shape (nova-sprint's index.html): the logo tile and the wordmark in the
// brand's h1, the live clock on the right, the footer's link to the repository; and a link to another repository.
constexpr const char* kBrandPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>nova-sprint</title>
<style>:root { --text: #e6e8eb; } html, body { margin: 0; color: var(--text); }
.top { display: flex; align-items: center; justify-content: space-between; padding: 2rem; }
.brand, .meta, .right { display: contents; }
.brand h1 { margin: 0 1rem 0 0; display: inline-flex; align-items: center; gap: 1rem; font-size: 2.5rem; line-height: 3rem; font-weight: 650; letter-spacing: -.01em; }
img#logo.logo-tile { width: 6rem; height: 6rem; flex: none; border-radius: 22.5%; }
.wordmark { font-family: ui-rounded, "SF Pro Rounded", "Nunito", system-ui, sans-serif; font-weight: 800; font-size: 5.5rem; line-height: 6rem; letter-spacing: -.02em; color: var(--text); text-transform: lowercase; }
.wordmark.nofit { display: none; }
footer { margin-top: 3rem; font-size: 1.5rem; line-height: 2.25rem; text-align: center; }
a { display: inline-block; }</style></head>
<body><header class="top"><div class="brand"><h1><img id="logo" class="logo-tile" width="120" height="120"
alt="nova-sprint" src="data:image/gif;base64,R0lGODlhAQABAAAAACw="><span class="wordmark" id="wordmark">nova-sprint</span></h1>
<div class="meta"><span class="chip">epoch <b>3</b></span></div></div>
<div class="right"><span class="live" id="live">Live</span></div></header>
<p><a id="other" href="https://github.com/mas-bandwidth/nova-tools">nova-tools</a></p>
<footer id="foot"><a id="repo" href="https://github.com/mas-bandwidth/nova-sprint">https://github.com/mas-bandwidth/nova-sprint</a></footer>
<footer id="more"><span id="note">made by the sprint</span> <a id="repo2" href="https://github.com/mas-bandwidth/nova-sprint/">repo</a></footer>
</body></html>)HTML";

// What the fixture server answers, and what it was asked.
struct Server {
    bool down = false;              // every request fails as a refused connection
    const char* page = kPage;       // the page's HTML
    std::string build = "b1";
    int pages = 0;                  // requests for the page
    int polls = 0;                  // requests for /api/sprint
    int cards = 0;                  // requests for a card's page, /cards/...
    std::string body;               // the last answer's body, kept while the fetch copies it

    static void answer(void* context, PrFetchInterceptRequest* request) {
        Server& s = *static_cast<Server*>(context);
        std::string url(request->desc.url.data, request->desc.url.size);
        if (s.down) {
            prFetchInterceptRequestFail(request, PrNetError_Refused);
            return;
        }
        PrFetchHeader type{"Content-Type", "text/html; charset=utf-8"};
        uint16_t status = 200;
        if (url == kUrl) {
            ++s.pages;
            s.body = s.page;
        } else if (has(url, "/api/sprint")) {
            ++s.polls;
            type.value = PrStringView{"application/json", 16};
            s.body = R"({"landed": 1234567, "cents": "12345678901", "build": ")" + s.build + "\"}";
        } else if (has(url, "/cards/")) {
            ++s.cards;
            s.body = "<!doctype html><title>card</title><p>a card</p>";
        } else {
            status = 404;
            s.body = "not here";
        }
        PrFetchInterceptResponse response{
            .status = status,
            .type = PrFetchResponseType_Basic,
            .headers = &type,
            .headerCount = 1,
            .body = PrByteView{reinterpret_cast<const uint8_t*>(s.body.data()), s.body.size()},
            .url = {},
        };
        prFetchInterceptRequestRespond(request, &response);
    }
};

// The page's clocks: monotonic from 0 and a wall clock from a fixed instant, both stepped by the test.
struct Clock {
    double ms = 0;
    static double now(void* context) { return static_cast<Clock*>(context)->ms; }
    static double wall(void* context) { return 1794326567000.0 + static_cast<Clock*>(context)->ms; }
};

// The window with no window, the sprint tab first and one friend, and the board over the fixture.
struct Harness {
    Server server;
    Clock clock;
    Model model{std::vector<FriendInfo>{FriendInfo{"f1", "m"}}, true};
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    View view;
    PraxBoard* board = nullptr;
    std::vector<std::string> opened;    // what the board gave the system to open: the test's opener, no browser
    bool visible = true;
    double time = 1000;
    PrVec2 extent{1000, 780};       // the window's, px

    explicit Harness(std::function<SprintBoardAddress()> find = nullptr) {
        prFontSetSetSystem(fonts, system);
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        PrKitTheme theme = windowTheme(fonts, false);
        kit = prKitCreate(ui, &theme);
        PraxBoard::Desc b;
        b.kit = kit;
        b.fonts = fonts;
        if (find)
            b.find = find;
        else
            b.url = kUrl;
        b.answer = Server::answer;
        b.answerContext = &server;
        b.now = Clock::now;
        b.wallNow = Clock::wall;
        b.clockContext = &clock;
        b.visible = [this] { return visible; };
        b.open = [this](const std::string& url) { opened.push_back(url); };
        board = new PraxBoard(b);
        view.model = &model;
        view.look = &look;
        view.board = board;
        view.extent = extent;
    }

    ~Harness() {
        delete board;
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    // One frame, `seconds` (50 ms) after the last on both clocks.
    void frame(double seconds = 0.05) {
        time += seconds;
        clock.ms += seconds * 1000;
        board->frame(time, prAppDisplayDefault());
        view.extent = extent;
        PrUiFrameDesc desc{.time = time, .delta = seconds, .extent = extent, .maxRounds = 4,
            .display = prAppDisplayDefault(), .preferences = {}};
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Harness& h = *static_cast<Harness*>(context);
            buildWindow(h.view, h.kit, kPrLayoutNodeNone);
        }, this);
    }

    void frames(int n) {
        for (int i = 0; i < n; ++i)
            frame();
    }

    std::string text() { return board->bodyText(); }

    bool hasStyle() {
        PrHtmlPage* page = board->page();
        if (page == nullptr)
            return false;
        PrDomDocument* d = prHtmlWindowDocument(prHtmlPageWindow(page));
        return prDomNodeGetElementById(prDomDocumentAsNode(d), PrStringView{kSprintStyleId.data(),
            kSprintStyleId.size()}) != nullptr;
    }

    PrDomDocument* document() { return prHtmlWindowDocument(prHtmlPageWindow(board->page())); }

    PrDomElement* byId(const char* id) {
        return prDomNodeGetElementById(prDomDocumentAsNode(document()), PrStringView{id, std::strlen(id)});
    }

    // An element's border box in the viewport, CSS px, as laid out now.
    PrDomRect rect(const char* id) { return prCssElementGetBoundingClientRect(byId(id)); }

    // The viewport's height, CSS px: the page's pane below the tab strip.
    double viewportHeight() { return prCssElementClientHeight(prCssDocumentScrollingElement(document())); }

    // A click of the primary button at the centre of the element, as the window passes it to the board, and a frame.
    void click(const char* id) {
        const PrDomRect r = rect(id);
        const PrRect pane = board->pane();
        PrAppEvent e{};
        e.kind = PrAppEventKind_PointerDown;
        e.pointer.position = PrVec2{pane.left + float(r.x + r.width / 2), pane.top + float(r.y + r.height / 2)};
        e.pointer.primary = true;
        e.pointer.clicks = 1;
        board->event(e);
        e.kind = PrAppEventKind_PointerUp;
        board->event(e);
        frames(3);
    }
};

// The page loads, its first answer paints, and the board's script runs: the digits grouped, the clock in en-US with
// its period, and the zone the system names for the date.
void loadsPollsAndFormats() {
    Harness h;
    h.frames(10);
    CHECK(h.server.pages == 1);
    CHECK(h.board->settled());
    CHECK(h.board->problem().empty());
    std::string t = h.text();
    CHECK(has(t, "landed 1,234,567 cents 12,345,678,901"));
    CHECK(std::regex_search(t, std::regex(R"(\d{1,2}:\d{2}:\d{2} (AM|PM)\|(AM|PM))")));
    if (failures != 0)
        std::fprintf(stderr, "the page's text: %s\n", t.c_str());
    std::time_t wall = std::time_t(Clock::wall(&h.clock) / 1000);
    std::tm local{};
    localtime_r(&wall, &local);
    tzset();
    std::string zone = tzname[local.tm_isdst > 0 ? 1 : 0];
    CHECK(has(t, "first"));
    CHECK(!zone.empty() && has(t, zone));
    CHECK(h.hasStyle());
    // The page is opaque: its canvas is the page's own --bg (#0d0f12), which the recorded list fills edge to edge.
    PrColor canvas = h.board->canvas();
    CHECK(canvas.a == 1.0f);
    CHECK(std::fabs(canvas.r - 0x0d / 255.0f) < 0.01f && std::fabs(canvas.g - 0x0f / 255.0f) < 0.01f &&
        std::fabs(canvas.b - 0x12 / 255.0f) < 0.01f);
    // A second's frames: the 1 s poll runs again.
    int polls = h.server.polls;
    h.frames(22);
    CHECK(h.server.polls > polls);
}

// Another tab, or the overlay hidden: no task runs, so no poll goes out however long it waits; shown again, the poll
// that waited runs in the first frames.
void offScreenRunsNothingAndCatchesUp() {
    Harness h;
    h.frames(10);
    CHECK(h.board->settled());
    CHECK(h.model.select_tab(1));
    h.frame();
    int polls = h.server.polls;
    h.frames(100);      // 5 s
    CHECK(h.server.polls == polls);
    CHECK(h.model.select_tab(0));
    h.frames(3);
    CHECK(h.server.polls > polls);

    h.visible = false;
    h.frame();
    polls = h.server.polls;
    h.frames(100);
    CHECK(h.server.polls == polls);
    h.visible = true;
    h.frames(3);
    CHECK(h.server.polls > polls);
}

// A new build reloads the page, and nexus's script runs again in the new document: the clock and the digits are still
// formatted, and the style is added again.
void aNewBuildReloadsWithThePrelude() {
    Harness h;
    h.frames(10);
    h.server.build = "b2";
    h.frames(40);
    CHECK(h.server.pages == 2);
    std::string t = h.text();
    CHECK(has(t, "reloaded"));
    CHECK(has(t, "landed 1,234,567"));
    CHECK(std::regex_search(t, std::regex(R"(\d{1,2}:\d{2}:\d{2} (AM|PM))")));
    CHECK(h.hasStyle());
}

// A board that cannot be reached says so and what to do, and loads once it can, trying again every 10 s.
void anUnreachableBoardSaysWhyAndRecovers() {
    Harness h;
    h.server.down = true;
    h.frames(10);
    CHECK(has(h.board->problem(), "cannot load the sprint board from http://board.test:8080/"));
    CHECK(has(h.board->problem(), "nova-sprint up") && has(h.board->problem(), "NEXUS_SPRINT_URL"));
    h.server.down = false;
    h.frames(100);      // 5 s: not yet
    CHECK(!h.board->problem().empty() && h.server.pages == 0);
    h.frames(120);      // past 10 s
    CHECK(h.server.pages == 1);
    CHECK(h.board->problem().empty() && has(h.text(), "landed 1,234,567"));
}

// The address lookup's failure is the tab's line.
void aFailedLookupIsTheLine() {
    Harness h([] { return SprintBoardAddress{{}, "cannot find the sprint board: nova-config was not found"}; });
    for (int i = 0; i < 200 && h.board->problem().find("cannot find") == std::string::npos; ++i)
        h.frame();
    CHECK(h.board->problem() == "cannot find the sprint board: nova-config was not found");
    CHECK(h.server.pages == 0);
}

// The lookup's address is the page's.
void aFoundAddressLoads() {
    Harness h([] { return SprintBoardAddress{kUrl, {}}; });
    for (int i = 0; i < 200 && !h.board->settled(); ++i)
        h.frame();
    CHECK(h.board->settled() && h.server.pages == 1);
}

// The Cost breakdown reaches one gap above the bottom of the first screen at a tall window and a short one, the gap the
// page leaves between the tiles and the panel, and the next block starts at or below the bottom; the pie keeps its
// size; scrolling does not move it; folded, the panel takes its own height again.
void theCostBreakdownFillsTheFirstScreen() {
    // At 900 px the panel's own content is some 60 px short of the screen's bottom; at 1600, some 770.
    for (float height : {1600.0f, 900.0f}) {
        Harness h;
        h.server.page = kSpendPage;
        h.extent = PrVec2{1200, height};
        h.frames(10);
        CHECK(h.board->settled());
        double viewport = h.viewportHeight();
        PrDomRect spend = h.rect("spend");
        PrDomRect pie = h.rect("pie");
        PrDomRect hero = h.rect("hero");
        double gap = spend.y - (hero.y + hero.height);
        CHECK(viewport > 300 && viewport < height);
        CHECK(std::fabs(gap - 32) < 1);     // the fixture's 2rem between the tiles and the panel
        CHECK(std::fabs(spend.y + spend.height - (viewport - gap)) < 1);
        CHECK(h.rect("next").y >= viewport);
        CHECK(std::fabs(pie.width - 192) < 1 && std::fabs(pie.height - pie.width) < 1);
        if (failures != 0)
            std::fprintf(stderr, "window %.0f: viewport %.1f, spend %.1f+%.1f, pie %.1fx%.1f\n", double(height),
                viewport, spend.y, spend.height, pie.width, pie.height);

        // Scrolled down and back: the panel's height stays.
        prCssElementSetScrollTop(prCssDocumentScrollingElement(h.document()), 400);
        h.frames(3);
        CHECK(std::fabs(h.rect("spend").height - spend.height) < 1);
        prCssElementSetScrollTop(prCssDocumentScrollingElement(h.document()), 0);
        h.frames(3);
        CHECK(std::fabs(h.rect("spend").y + h.rect("spend").height - (viewport - gap)) < 1);

        // Folded: the head alone.
        prDomElementSetAttribute(h.byId("spend"), PrStringView{"class", 5}, PrStringView{"panel spend folded", 18});
        h.frames(3);
        CHECK(h.rect("spend").height < 200);
    }
}

// Keys and the wheel through Prax's view: a tap moves a line, a held Down scrolls while held (its repeats ignored) and
// coasts to rest, Page Down a page less a line, the ends bounce and spring back, a notch three lines; a trackpad's
// pixels scroll at once and stop any motion.
void keysAndTheWheelScrollSmoothly() {
    Harness h;
    h.server.page = kSpendPage;
    h.extent = PrVec2{1200, 900};
    h.frames(10);
    CHECK(h.board->settled());
    PrDomElement* s = prCssDocumentScrollingElement(h.document());
    auto top = [&] { return prCssElementScrollTop(s); };
    auto send = [&](PrAppEvent e) { CHECK(h.board->event(e)); };
    auto key = [](PrAppEventKind kind, PrAppKey k, PrAppModifierFlags modifiers, bool repeat) {
        PrAppEvent e{};
        e.kind = kind;
        e.modifiers = modifiers;
        e.keyboard.key = k;
        e.keyboard.repeat = repeat;
        return e;
    };
    auto down = [&](PrAppKey k, PrAppModifierFlags m = 0, bool repeat = false) {
        send(key(PrAppEventKind_KeyDown, k, m, repeat));
    };
    auto up = [&](PrAppKey k) { send(key(PrAppEventKind_KeyUp, k, 0, false)); };
    const double frame = 1.0 / 120;
    auto run = [&](double seconds) { for (double t = 0; t < seconds; t += frame) h.frame(frame); };
    const double bottom = prCssElementScrollHeight(s) - prCssElementClientHeight(s);

    // A tap of Down: no jump at the press, moving at the next frames, at rest exactly a line on.
    CHECK(top() == 0);
    down(PrAppKey_Down);
    CHECK(top() == 0);
    h.frame(frame);
    h.frame(frame);
    CHECK(top() > 0 && top() < 40);
    run(0.05);
    up(PrAppKey_Down);
    run(2);
    CHECK(top() == 40);

    // Page Down: a page less a line of overlap, with momentum.
    const double page = h.viewportHeight() - 40;
    down(PrAppKey_PageDown);
    run(0.03);
    CHECK(top() > 40 && top() < 40 + page);
    up(PrAppKey_PageDown);
    run(2);
    CHECK(std::fabs(top() - (40 + page)) < 1);

    // Down held for a second, the OS's repeats ignored: a line, then 1000 px/s from 0.15 s on, every frame onward; then it coasts to rest.
    const double from = top();
    down(PrAppKey_Down);
    double last = top();
    for (int i = 0; i < 120; ++i) {
        h.frame(frame);
        if (i % 4 == 3)
            down(PrAppKey_Down, 0, true);
        CHECK(top() >= last);
        last = top();
    }
    CHECK(top() - from > 720 && top() - from < 880);
    up(PrAppKey_Down);
    const double released = top();
    run(2);
    CHECK(top() > released + 60 && top() < released + 140);

    // Command-Up: to the top. End: to the bottom, where it bounces past the end and comes back.
    down(PrAppKey_Up, PrAppModifierFlag_Super);
    run(2);
    CHECK(top() == 0);
    down(PrAppKey_End);
    run(2);
    CHECK(std::fabs(top() - bottom) < 1);
    down(PrAppKey_Down);
    up(PrAppKey_Down);
    bool bounced = false;
    for (double t = 0; t < 2; t += frame) {
        h.frame(frame);
        CHECK(std::fabs(top() - bottom) < 1);
        bounced = bounced || h.board->overscroll() > 1;
    }
    CHECK(bounced && h.board->overscroll() == 0);
    down(PrAppKey_Home);
    run(2);
    CHECK(top() == 0);

    // A wheel's notch over the page: three lines of Prax's 40 px, with momentum.
    PrAppEvent wheel{};
    wheel.kind = PrAppEventKind_Scroll;
    wheel.scroll = PrAppScrollEvent{.position = PrVec2{600, 450}, .delta = PrVec2{0, 3}, .unit = PrAppScrollUnit_Line};
    send(wheel);
    CHECK(top() == 0);
    h.frame(frame);
    h.frame(frame);
    CHECK(top() > 0 && top() < 120);
    run(2);
    CHECK(top() == 120);

    // A trackpad's pixels: at once, and they stop any motion where it shows.
    send(wheel);
    h.frame(frame);
    h.frame(frame);
    const double mid = top();
    CHECK(mid > 120 && mid < 240);
    PrAppEvent pixels = wheel;
    pixels.scroll.delta = PrVec2{0, 10};
    pixels.scroll.unit = PrAppScrollUnit_Pixel;
    send(pixels);
    CHECK(std::fabs(top() - (mid + 10)) < 1);
    run(1);
    CHECK(std::fabs(top() - (mid + 10)) < 1);

    // Scaled text: a trackpad's pixels are the window's, so the page moves by them over the scale.
    h.board->setTextScale(2);
    h.frames(3);
    const double scaled = top();
    send(pixels);
    CHECK(std::fabs(top() - (scaled + 5)) < 1);
}

// The tab always shows the board. A click on a link that would leave it opens the link through the opener (the
// system's browser in the app; the test's records it) and the page stays on the board: a link to another site, to
// another page of the board's server, and one with target _blank. A link to a place on the board scrolls there in the
// tab, a click inside it included; a javascript: link and a link the page cancels itself open nothing. Whatever else
// takes the page off the board, choosing the sprint tab (Board::home) brings it back; on the board, home loads nothing.
void linksThatLeaveTheBoardOpenInTheBrowser() {
    Harness h;
    h.server.page = kLinksPage;
    h.frames(10);
    CHECK(h.board->settled());
    CHECK(h.board->documentUrl() == kUrl);
    PrDomElement* s = prCssDocumentScrollingElement(h.document());

    h.click("repo");
    CHECK((h.opened == std::vector<std::string>{"https://github.test/nova-sprint"}));
    CHECK(h.board->documentUrl() == kUrl && h.server.pages == 1);

    h.click("card");
    CHECK(h.opened.size() == 2 && h.opened.back() == "http://board.test:8080/cards/12");
    CHECK(h.board->documentUrl() == kUrl && h.server.cards == 0);

    h.click("blank");
    CHECK(h.opened.size() == 3 && h.opened.back() == "http://board.test:8080/#later");
    CHECK(h.board->documentUrl() == kUrl && prCssElementScrollTop(s) == 0);

    h.click("script");
    CHECK(h.opened.size() == 3 && h.board->documentUrl() == kUrl);
    h.click("kept");
    CHECK(h.opened.size() == 3 && h.board->documentUrl() == kUrl);

    h.click("inner");
    CHECK(h.opened.size() == 3);
    CHECK(h.board->documentUrl() == std::string(kUrl) + "#later" && prCssElementScrollTop(s) > 0);
    CHECK(h.server.pages == 1);

    // On the board, at one of its places: home loads nothing.
    h.board->home();
    h.frames(5);
    CHECK(h.server.pages == 1 && h.board->documentUrl() == std::string(kUrl) + "#later");

    // Off the board by another way (here the page navigated from outside, as a script setting location would): home
    // brings it back.
    CHECK(prHtmlPageNavigate(h.board->page(), PrStringView{"http://board.test:8080/cards/7", 30}) == PrStatus_Ok);
    h.frames(10);
    CHECK(h.server.cards == 1 && h.board->documentUrl() == "http://board.test:8080/cards/7");
    h.board->home();
    h.frames(10);
    CHECK(h.server.pages == 2 && h.board->documentUrl() == kUrl && h.board->settled());
}

// The board's header shows as served: its logo and its "nova-sprint" title. The footer's link to nova-sprint's
// repository is not rendered, and a footer that held only it is not either; in a footer with more, the rest stays.
// Another link still renders, and still opens in the browser.
void theRepositoryLinkIsHidden() {
    Harness h;
    h.server.page = kBrandPage;
    h.frames(10);
    CHECK(h.board->settled());
    std::string word;
    {
        PrRawString t = prDomNodeTextContent(prDomElementAsNode(h.byId("wordmark")));
        PrStringView v = prRawStringView(&t);
        word.assign(v.data != nullptr ? v.data : "", v.size);
        prRawStringReset(&t);
    }
    CHECK(word == "nova-sprint");
    const PrDomRect mark = h.rect("wordmark");
    const PrDomRect logo = h.rect("logo");
    CHECK(mark.width > 50 && mark.height > 80);
    CHECK(logo.width > 90 && logo.height > 90);                     // the 6rem tile
    CHECK(mark.x > logo.x + logo.width);                            // the title beside it
    const PrDomRect repo = h.rect("repo");
    CHECK(repo.width == 0 && repo.height == 0);                     // not rendered
    const PrDomRect foot = h.rect("foot");
    CHECK(foot.width == 0 && foot.height == 0);                     // its footer held nothing else: not rendered
    const PrDomRect repo2 = h.rect("repo2");
    CHECK(repo2.width == 0 && repo2.height == 0);                   // in a footer with more: the link alone goes
    CHECK(h.rect("more").height > 0 && h.rect("note").width > 0);
    const PrDomRect other = h.rect("other");
    CHECK(other.width > 0 && other.height > 0);
    h.click("other");
    CHECK((h.opened == std::vector<std::string>{"https://github.com/mas-bandwidth/nova-tools"}));
}

} // namespace

int main() {
    loadsPollsAndFormats();
    offScreenRunsNothingAndCatchesUp();
    aNewBuildReloadsWithThePrelude();
    anUnreachableBoardSaysWhyAndRecovers();
    aFailedLookupIsTheLine();
    aFoundAddressLoads();
    theCostBreakdownFillsTheFirstScreen();
    keysAndTheWheelScrollSmoothly();
    linksThatLeaveTheBoardOpenInTheBrowser();
    theRepositoryLinkIsHidden();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("board tests passed\n");
    return 0;
}
