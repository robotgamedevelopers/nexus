// The sprint tab's page (src/board.h) over Prax's HTML stack, headless: a fixture board served from memory through the
// board's test network (Prax's null network and an intercepting fetcher: no socket, no live board), shown by the nexus
// view over a Prax UI context with no window and no GPU. The page's clocks are made up and stepped by the test; the
// fixture's script uses what the served board uses: a 1 s poll of /api/sprint with fetch, Intl.DateTimeFormat's parts,
// toLocaleTimeString, toLocaleString on Number and BigInt, session storage and a reload on a new build. Text is set in
// the system's fonts and scripts run in QuickJS: a functional test.

#include <cstdio>
#include <ctime>
#include <regex>
#include <string>
#include <vector>

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

// What the fixture server answers, and what it was asked.
struct Server {
    bool down = false;              // every request fails as a refused connection
    std::string build = "b1";
    int pages = 0;                  // requests for the page
    int polls = 0;                  // requests for /api/sprint
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
            s.body = kPage;
        } else if (has(url, "/api/sprint")) {
            ++s.polls;
            type.value = PrStringView{"application/json", 16};
            s.body = R"({"landed": 1234567, "cents": "12345678901", "build": ")" + s.build + "\"}";
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
    bool visible = true;
    double time = 1000;

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
        board = new PraxBoard(b);
        view.model = &model;
        view.look = &look;
        view.board = board;
        view.extent = PrVec2{1000, 780};
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

    // One frame, 50 ms after the last on both clocks.
    void frame() {
        time += 0.05;
        clock.ms += 50;
        board->frame(time, prAppDisplayDefault());
        PrUiFrameDesc desc{.time = time, .delta = 0.05, .extent = PrVec2{1000, 780}, .maxRounds = 4,
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

} // namespace

int main() {
    loadsPollsAndFormats();
    offScreenRunsNothingAndCatchesUp();
    aNewBuildReloadsWithThePrelude();
    anUnreachableBoardSaysWhyAndRecovers();
    aFailedLookupIsTheLine();
    aFoundAddressLoads();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("board tests passed\n");
    return 0;
}
