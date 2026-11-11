#include "board.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <thread>

#include "pr/app_shell.h"
#include "pr/compress.h"
#include "pr/css.h"
#include "pr/css_view.h"
#include "pr/dom.h"
#include "pr/fetch.h"
#include "pr/html.h"
#include "pr/html_storage.h"
#include "pr/http.h"
#include "pr/image.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/net.h"
#include "pr/net_null.h"
#include "pr/svg_quickjs.h"
#include "pr/tls.h"
#include "pr/ui.h"
#include "pr/url_quickjs.h"

#include "style.h"

namespace nexus {

namespace {

// How long a frame runs the page's due tasks, in milliseconds: a long load renders as it goes, and the window's own
// rounds keep their time. Prax's browser runs its pages for 8 ms at a time on a thread of their own; this runs on the
// window's thread, so half that.
constexpr double kTaskBudget = 4;

// How long after a failed load, or a failed lookup of the address, the board tries again, while it shows.
constexpr double kRetrySeconds = 10;

// The names the board's style sheets ask for that are not the names of families: the system's own faces under their
// marketing and engine names. Each goes to the family the font set's system gives the generic.
struct Alias {
    const char* name;
    PrFontGeneric generic;
};
constexpr Alias kAliases[] = {
    {"-apple-system", PrFontGeneric_SystemUi},
    {"BlinkMacSystemFont", PrFontGeneric_SystemUi},
    {"SF Pro Text", PrFontGeneric_SystemUi},
    {"SF Pro Display", PrFontGeneric_SystemUi},
    {"SF Pro Rounded", PrFontGeneric_UiRounded},
    {"SF Mono", PrFontGeneric_UiMonospace},
    {"SFMono-Regular", PrFontGeneric_UiMonospace},
};

PrStringView sv(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

std::string taken(PrRawString s) {
    PrStringView v = prRawStringView(&s);
    std::string copy = v.data != nullptr ? std::string(v.data, v.size) : std::string();
    prRawStringReset(&s);
    return copy;
}

// The page as last recorded, embedded in the window's list, clipped to the pane (the incremental example's paintPage).
void paintPage(void* context, PrVec2 extent, PrDrawList* list) {
    prDrawListPushClipRect(list, PrRect{0, 0, extent.x, extent.y}, PrDrawEdges_Aliased);
    prDrawListEmbed(list, static_cast<PrDrawList*>(context), PrDVec2{0, 0});
    prDrawListPopClip(list);
}

void retainPage(void* context) {
    prDrawListRetain(static_cast<PrDrawList*>(context));
}

void releasePage(void* context) {
    prDrawListRelease(static_cast<PrDrawList*>(context));
}

// The fetcher, HTTP and the null network's own fetcher are freed on the network's loop, after the cancels of the
// page's fetches (Prax's browser, closeShared).
struct NetParts {
    PrFetchNetwork* fetcher;
    PrFetchNetwork* network;
    PrHttp* http;
};

void destroyOnLoop(void* context) {
    NetParts& parts = *static_cast<NetParts*>(context);
    prFetchNetworkDestroy(parts.fetcher);
    prFetchNetworkDestroy(parts.network);
    prHttpDestroy(parts.http);
}

// Every request passes to the network.
void passAll(void*, PrFetchInterceptRequest* request) {
    prFetchInterceptRequestPass(request);
}

std::string_view withoutFragment(std::string_view url) {
    return url.substr(0, url.find('#'));
}

// The identity of the board's click listener on each window (PrDomEventListener::identity): the same each time, as the
// listener is.
constexpr uint64_t kLinkListener = 0x6e657875736c6e6bull;

// How many targets of a click's path the board looks through for its link.
constexpr uint32_t kPathCapacity = 64;

std::string statusName(PrStatus status) {
    switch (status) {
        case PrStatus_Io: return "the server could not be reached or did not answer";
        case PrStatus_NotFound: return "the server has no such page";
        default: return "status " + std::to_string(int(status));
    }
}

// The page's own scroll (its document's scrolling element), as the scroller sees it. Only the page's own scroll moves
// smoothly: a box of the page that scrolls by itself moves at once, as Prax moves it.
class PageScroll final : public GlidePage {
public:
    explicit PageScroll(PrDomElement* element) : element_(element) {}
    double top() override { return prCssElementScrollTop(element_); }
    void setTop(double y) override { prCssElementSetScrollTop(element_, y); }
    double max() override {
        return std::max(0, prCssElementScrollHeight(element_) - prCssElementClientHeight(element_));
    }
    double height() override { return prCssElementClientHeight(element_); }

private:
    PrDomElement* element_;
};

} // namespace

PraxBoard::PraxBoard(const Desc& desc) : desc_(desc), kit_(desc.kit), app_(desc.app) {
    if (!desc_.url.empty()) {
        url_ = desc_.url;
    } else {
        std::function<SprintBoardAddress()> find = desc_.find ? desc_.find : [] { return findSprintBoard(); };
        PrApp* app = app_;
        finding_ = std::async(std::launch::async, [find, app] {
            SprintBoardAddress address = find();
            if (app != nullptr)
                prAppWake(app);
            return address;
        });
    }
    tzset();
    prelude_ = sprintPrelude(tzname[0] != nullptr ? tzname[0] : "", tzname[1] != nullptr ? tzname[1] : "");
}

PraxBoard::~PraxBoard() {
    // The page's view, then the engine (its objects' finalizers reach the page's window), then the page, the agent and
    // the network, as Prax's browser closes (tab.cpp TaskKind_Shutdown, main.cpp closeShared).
    prHtmlViewDestroy(view_);
    prJsHtmlEngineDestroy(engine_);
    prHtmlPageDestroy(page_);
    prDomAgentDestroy(agent_);
    if (net_ != nullptr) {
        NetParts parts{fetcher_, network_, http_};
        if (nullNet_) {
            prNetNullRun(net_);
            destroyOnLoop(&parts);
        } else {
            prNetPostBlocking(net_, destroyOnLoop, &parts);
        }
        prTlsDestroy(tls_);
        prNetDestroy(net_);
    }
    prHtmlStorageProviderDestroy(localStorage_);
    prHtmlStorageProviderDestroy(sessionStorage_);
    prImageCodecDestroy(images_);
    prCompressCodecDestroy(compress_);
    prDrawListRelease(recorded_);
    prTextFontRelease(font_);
    prTextShaperDestroy(shaper_);
    prFontSetDestroy(fonts_);
    if (finding_.valid())
        finding_.wait();
}

void PraxBoard::takeAddress() {
    if (!url_.empty() || !finding_.valid())
        return;
    if (finding_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    SprintBoardAddress address = finding_.get();
    if (!address.url.empty()) {
        url_ = address.url;
        problem_.clear();
        return;
    }
    problem_ = address.error;
    findAgainAt_ = frameTime_ + kRetrySeconds;
}

void PraxBoard::openPage() {
    // The page's fonts: the window's, with the system's faces under the names the board asks for.
    fonts_ = prFontSetCreate(desc_.fonts);
    for (const Alias& a : kAliases) {
        PrStringView family = prFontSetGenericFamily(desc_.fonts, a.generic);
        if (family.size != 0)
            prFontSetAlias(fonts_, sv(a.name), family);
    }
    // The page's text is shaped on the agent's layout thread as well as this one: a shaper of its own (Prax's browser).
    shaper_ = prTextShaperCreate(PrTextShaperBackend_Default);
    PrTextFontDesc font{.set = fonts_, .style = prFontStyleDefault()};
    font_ = prTextFontCreate(&font);
    compress_ = prCompressCodecCreate(PrCompressCodecBackend_Default);
    images_ = compress_ != nullptr ? prImageCodecCreate(PrImageCodecBackend_Default, compress_) : nullptr;

    if (desc_.answer != nullptr) {
        net_ = prNetNullCreate();
        nullNet_ = true;
        PrFetchInterceptDesc intercept{.net = net_, .request = desc_.answer, .context = desc_.answerContext};
        fetcher_ = prFetchInterceptCreate(&intercept);
    } else {
        net_ = prNetCreate(PrNetBackend_Default);
        tls_ = net_ != nullptr ? prTlsCreate(PrTlsBackend_Default, net_) : nullptr;
        if (net_ != nullptr) {
            PrHttpDesc http{.net = net_, .tls = tls_};
            http_ = prHttpCreate(PrHttpBackend_Default, &http);
            PrFetchNetworkDesc fetch{.net = net_, .http = http_, .compress = compress_};
            network_ = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &fetch);
            // An intercepting fetcher in front, passing everything on: the same shape as the test's, so the page's
            // requests take one path whichever network answers them.
            PrFetchInterceptDesc intercept{.net = net_, .request = passAll, .next = network_};
            fetcher_ = network_ != nullptr ? prFetchInterceptCreate(&intercept) : nullptr;
        }
    }
    // Storage in memory only: the board keeps its scroll across a reload in session storage.
    PrHtmlStorageProviderDesc storage{.quotaBytes = kPrHtmlStorageQuotaBytes};
    localStorage_ = prHtmlStorageProviderCreate(&storage);
    sessionStorage_ = prHtmlStorageProviderCreate(&storage);

    agent_ = prDomAgentCreate(nullptr);
    prDomAgentSetWake(agent_, wake, this);
    // The board draws its charts with SVG made by createElementNS, and reads URLs.
    static PrJsHtmlBind* const kBinds[] = {prJsSvgBind, prJsUrlBind};
    PrJsHtmlEngineDesc engine{.binds = kBinds, .bindCount = 2, .console = nullptr, .context = this,
        .realmOpened = realmOpened, .wait = nullptr};
    engine_ = prJsHtmlEngineCreate(&engine);

    PrHtmlHost host = prHtmlHostDefault();
    host.context = this;
    host.fetcher = fetcher_;
    host.loaded = loaded;
    host.windowOpened = windowOpened;
    host.openWindow = openWindow;
    host.agent = agent_;
    host.font = font_;
    host.shaper = shaper_;
    host.images = images_;
    host.compress = compress_;
    host.localStorage = localStorage_;
    host.sessionStorage = sessionStorage_;
    host.now = desc_.now != nullptr ? clockNow : nullptr;
    host.wallNow = desc_.wallNow != nullptr ? clockWallNow : nullptr;
    PrHtmlHost scripted = prJsHtmlEngineHost(engine_, &host, PrJsHtmlScripting_Enabled);
    page_ = prHtmlPageCreate(&scripted);
    // The view records the page's canvas fill (the board's own --bg), so the page is opaque over the window's
    // see-through background (the owner 2026-11-10: a see-through page is hard to read).
    PrHtmlViewDesc view{.page = page_, .compositor = nullptr, .jobBudget = 0, .canvasFilled = false,
        .updated = nullptr, .updatedContext = nullptr};
    view_ = prHtmlViewCreate(&view);
    if (desc_.window != nullptr)
        prHtmlViewSetWindow(view_, desc_.window);
}

void PraxBoard::navigate() {
    load_ = Load::Loading;
    ownLoad_ = true;
    loadError_.clear();
    if (fetcher_ == nullptr) {
        load_ = Load::Failed;
        loadError_ = "cannot show the sprint board: this build has no network to fetch " + url_ + " with";
        retryAt_ = 1e300;
        return;
    }
    if (prHtmlPageNavigate(page_, sv(url_)) != PrStatus_Ok) {
        load_ = Load::Failed;
        loadError_ = "cannot show the sprint board: \"" + url_ + "\" is not an address; set NEXUS_SPRINT_URL to the "
            "board's address (http://<host>:<port>/), or unset it to read the sprint's record";
        retryAt_ = 1e300;
    }
}

// The page's clocks are the desc's, with its context.
double PraxBoard::clockNow(void* context) {
    const Desc& d = static_cast<const PraxBoard*>(context)->desc_;
    return d.now(d.clockContext);
}

double PraxBoard::clockWallNow(void* context) {
    const Desc& d = static_cast<const PraxBoard*>(context)->desc_;
    return d.wallNow(d.clockContext);
}

void PraxBoard::loaded(void* context, PrHtmlPage*, PrStatus status) {
    PraxBoard& b = *static_cast<PraxBoard*>(context);
    b.dirty_ = true;
    const bool own = b.ownLoad_;
    b.ownLoad_ = false;
    if (status == PrStatus_Ok) {
        b.load_ = Load::Loaded;
        b.loadError_.clear();
        if (own)
            b.home_ = std::string(withoutFragment(b.documentUrl()));
        return;
    }
    b.load_ = Load::Failed;
    b.loadError_ = "cannot load the sprint board from " + b.url_ + ": " + statusName(status) + "; check that the "
        "sprint runs (`nova-sprint up`), or set NEXUS_SPRINT_URL to the board's address; trying again every " +
        std::to_string(int(kRetrySeconds)) + " seconds";
    b.retryAt_ = b.frameTime_ + kRetrySeconds;
}

std::string PraxBoard::documentUrl() const {
    if (page_ == nullptr)
        return {};
    PrHtmlWindow* window = prHtmlPageWindow(page_);
    PrDomDocument* document = window != nullptr ? prHtmlWindowDocument(window) : nullptr;
    return document != nullptr ? taken(prDomDocumentUrl(document)) : std::string();
}

void PraxBoard::home() {
    if (page_ == nullptr || url_.empty() || home_.empty())
        return;
    const bool away = withoutFragment(documentUrl()) != home_ || (prHtmlPageIsNavigating(page_) && !ownLoad_);
    if (away) {
        endScroll();
        navigate();
    }
}

void PraxBoard::openLink(const std::string& url) {
    if (desc_.open) {
        desc_.open(url);
        return;
    }
    if (app_ == nullptr)
        return;
    const PrStatus status = prAppOpenUrl(app_, sv(url));
    if (status != PrStatus_Ok)
        std::fprintf(stderr, "nexus: the system could not open %s (status %d)\n", url.c_str(), int(status));
}

// Each window the page opens (one per document) gets the board's click listener, in the bubbling phase: after the
// page's own listeners, which may cancel the click themselves, and before the link's activation follows it.
void PraxBoard::windowOpened(void* context, PrHtmlPage* page) {
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomEventTarget* target = window != nullptr ? prHtmlWindowAsEventTarget(window) : nullptr;
    if (target == nullptr)
        return;
    const PrStringView click{"click", 5};
    PrDomEventListener listener{.invoke = linkClicked, .context = context, .identity = kLinkListener,
        .retain = nullptr, .release = nullptr};
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(target, click);
    prDomEventTargetAddEventListener(target, click, &listener, &options);
}

void PraxBoard::openWindow(void* context, PrHtmlPage*, PrStringView url) {
    static_cast<PraxBoard*>(context)->openLink(std::string(url.data != nullptr ? url.data : "", url.size));
}

// A click the page has not canceled, on a link (the nearest a or area with an href on its path, which its activation
// follows) that would leave the board: canceled, and the link opens in the browser.
void PraxBoard::linkClicked(void* context, PrDomEvent* event) {
    PraxBoard& b = *static_cast<PraxBoard*>(context);
    if (prDomEventDefaultPrevented(event))
        return;
    PrDomEventTarget* path[kPathCapacity];
    const uint32_t count = std::min(prDomEventComposedPath(event, path, kPathCapacity), kPathCapacity);
    for (uint32_t i = 0; i < count; ++i) {
        if (prHtmlAnchorElementQuery(path[i]) == nullptr && prHtmlAreaElementQuery(path[i]) == nullptr)
            continue;
        const PrDomElement* link = prDomElementQuery(path[i]);
        if (prDomElementGetAttribute(link, PrStringView{"href", 4}).data == nullptr)
            continue;
        const std::string href = taken(prHtmlHyperlinkComponent(link, PrUrlComponent_Href));
        const PrStringView target = prDomElementGetAttribute(link, PrStringView{"target", 6});
        const std::string_view targetName = target.data != nullptr ? std::string_view(target.data, target.size) :
            std::string_view();
        if (linkLeavesBoard(href, b.documentUrl(), targetName)) {
            prDomEventPreventDefault(event);
            b.openLink(href);
        }
        return;
    }
}

void PraxBoard::realmOpened(void* context, JSContext* ctx, PrHtmlPage* page) {
    if (page == nullptr)
        return;
    const PraxBoard& b = *static_cast<const PraxBoard*>(context);
    prJsHtmlEngineRun(ctx, sv(b.prelude_), PrStringView{"nexus:prelude", 13}, 1, 1);
}

void PraxBoard::wake(void* context) {
    PraxBoard& b = *static_cast<PraxBoard*>(context);
    if (b.app_ != nullptr)
        prAppWake(b.app_);
}

void PraxBoard::frame(double seconds, const PrAppDisplay& display) {
    frameTime_ = seconds;
    display_ = display;
    const bool onScreen = shown_ && (!desc_.visible || desc_.visible());
    if (page_ != nullptr && hiddenSet_ == onScreen) {
        prHtmlPageSetVisibility(page_, onScreen ? PrHtmlVisibility_Visible : PrHtmlVisibility_Hidden);
        hiddenSet_ = !onScreen;
    }
    if (!onScreen) {
        endScroll();
        return;
    }
    takeAddress();
    if (url_.empty()) {
        if (!finding_.valid() && frameTime_ >= findAgainAt_) {
            std::function<SprintBoardAddress()> find = desc_.find ? desc_.find : [] { return findSprintBoard(); };
            PrApp* app = app_;
            finding_ = std::async(std::launch::async, [find, app] {
                SprintBoardAddress address = find();
                if (app != nullptr)
                    prAppWake(app);
                return address;
            });
        }
        return;
    }
    if (page_ == nullptr) {
        openPage();
        navigate();
    } else if (load_ == Load::Failed && frameTime_ >= retryAt_) {
        navigate();
    }
    if (PrDomElement* s = scroller(); s != nullptr && scroller_.busy()) {
        PageScroll page(s);
        if (scroller_.frame(page, frameTime_))
            dirty_ = true;
    }
    runTasks();
}

void PraxBoard::runTasks() {
    if (nullNet_)
        prNetNullRun(net_);
    if (prDomAgentRun(agent_, kTaskBudget))
        dirty_ = true;
    if (nullNet_ && prNetNullRun(net_))
        dirty_ = true;
}

void PraxBoard::addPageStyle() {
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    PrDomElement* head = document != nullptr ? prDomDocumentHead(document) : nullptr;
    if (head == nullptr || prDomNodeGetElementById(prDomDocumentAsNode(document), sv(kSprintStyleId)) != nullptr)
        return;
    PrDomElement* style = nullptr;
    if (prDomElementCreate(document, PrStringView{"style", 5}, nullptr, &style) != PrStatus_Ok)
        return;
    prDomElementSetAttribute(style, PrStringView{"id", 2}, sv(kSprintStyleId));
    prDomNodeSetTextContent(prDomElementAsNode(style), sv(sprintPageStyle(spend_)));
    prDomNodeAppendChild(prDomElementAsNode(head), prDomElementAsNode(style));
    prDomNodeRelease(prDomElementAsNode(style));
}

SpendFit PraxBoard::measureSpend() {
    SpendFit fit;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    if (document == nullptr)
        return fit;
    PrDomElement* spend = nullptr;
    if (prCssNodeQuerySelector(prDomDocumentAsNode(document), sv(kSpendSelector), &spend) != PrStatus_Ok ||
        spend == nullptr)
        return fit;
    // The top in the viewport, plus the page's scroll: the top in the document, wherever the page is scrolled.
    PrDomElement* scrolling = prCssDocumentScrollingElement(document);
    double scrolled = scrolling != nullptr ? prCssElementScrollTop(scrolling) : 0;
    PrDomRect box = prCssElementGetBoundingClientRect(spend);
    fit.top = box.y + scrolled;
    // The gaps to the blocks before and after it, as the page lays them out.
    if (PrDomElement* before = prDomNodePreviousElementSibling(prDomElementAsNode(spend))) {
        PrDomRect r = prCssElementGetBoundingClientRect(before);
        fit.gap = box.y - (r.y + r.height);
    }
    if (PrDomElement* after = prDomNodeNextElementSibling(prDomElementAsNode(spend)))
        fit.below = prCssElementGetBoundingClientRect(after).y - (box.y + box.height);
    return fit;
}

void PraxBoard::fitSpend() {
    SpendFit fit = measureSpend();
    // Half a pixel either way is the same screen: no restyle, so a rounding never repaints in a loop. None of the
    // measures depends on the panel's height, and the gap below is at least the margin the style gives, so one restyle
    // settles them.
    if (std::fabs(fit.top - spend_.top) < 0.5 && std::fabs(fit.gap - spend_.gap) < 0.5 &&
        std::fabs(fit.below - spend_.below) < 0.5)
        return;
    spend_ = fit;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    PrDomElement* style = document != nullptr
        ? prDomNodeGetElementById(prDomDocumentAsNode(document), sv(kSprintStyleId)) : nullptr;
    if (style == nullptr)
        return;
    prDomNodeSetTextContent(prDomElementAsNode(style), sv(sprintPageStyle(spend_)));
    dirty_ = true;
}

void PraxBoard::paint(PrVec2 extent) {
    PrVec2 viewport{extent.x / zoom_, extent.y / zoom_};
    if (viewport.x != viewport_.x || viewport.y != viewport_.y) {
        prHtmlViewSetViewport(view_, viewport);
        viewport_ = viewport;
        dirty_ = true;
    }
    PrAppDisplay display = display_;
    display.pixelRatio *= zoom_;
    prHtmlViewSetDisplay(view_, &display);
    if (!dirty_ || prHtmlPageIsRenderBlocked(page_))
        return;
    addPageStyle();
    PrDrawList* list = prDrawListCreate();
    // Past an end, the page is drawn rubber-banded (scroll_glide.h): moved by how far past, over its own background.
    const float over = float(scroller_.overscroll());
    if (over != 0)
        prDrawListRect(list, PrRect{0, 0, extent.x, extent.y}, PrDrawPaint{.color = prColor16FromColor(canvas_)});
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Translate(PrVec2{0, -over}),
        prAffine2Scale(PrVec2{zoom_, zoom_})));
    canvas_ = prHtmlViewPaint(view_, list);
    prDrawListPopTransform(list);
    prDrawListFreeze(list);
    prDrawListRelease(recorded_);
    recorded_ = list;
    dirty_ = false;
    // The Cost breakdown's place as laid out in this paint: if it moved, the style is set again and the next frame paints
    // the panel down to one gap above the bottom of the screen.
    fitSpend();
}

bool PraxBoard::show(PrLayoutNode parent, PrVec2 extent, std::string* problem) {
    said_.clear();
    bool page = showPage(parent, extent, &said_);
    if (!page)
        *problem = said_;
    return page;
}

bool PraxBoard::showPage(PrLayoutNode parent, PrVec2 extent, std::string* problem) {
    shown_ = true;
    PrUi* ui = kit_->ui;
    PrLayoutTree* tree = kit_->tree;
    const bool onScreen = !desc_.visible || desc_.visible();
    if (url_.empty()) {
        *problem = problem_.empty() ? "looking up the sprint board's address (nova-config service show sprint-run)"
                                    : problem_;
        if (onScreen && !finding_.valid())
            prUiRequestFrameAt(ui, std::max(frameTime_, findAgainAt_));
        return false;
    }
    if (page_ == nullptr) {
        *problem = "opening the sprint board at " + url_;
        if (onScreen)
            prUiRequestFrame(ui);
        return false;
    }
    if (load_ == Load::Failed) {
        *problem = loadError_;
        if (onScreen)
            prUiRequestFrameAt(ui, retryAt_);
        return false;
    }
    paint(extent);
    if (recorded_ == nullptr) {
        *problem = "loading the sprint board from " + url_;
        if (onScreen)
            prUiRequestFrame(ui);
        return false;
    }
    // The pane: a replaced element of the extent given, whose painter embeds the page (incremental.cpp 729-737).
    PrLayoutNode pane = prLayoutTreeFrameReplaced(tree, parent, 0, 0);
    PrKitPlace place{};
    place.width = prKitPx(extent.x);
    place.height = prKitPx(extent.y);
    place.flex = PrKitFlex_Fixed;
    prKitPlace(kit_, pane, &place);
    PrLayoutContent content{.kind = PrLayoutContentKind_Painter};
    content.painter = PrLayoutPainter{.context = recorded_, .paint = paintPage, .measure = nullptr,
        .retain = retainPage, .release = releasePage};
    prLayoutTreeFrameSetContent(tree, pane, &content);
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(tree, pane, PrLayoutCoordinates_Viewport));
    if (bounds.right - bounds.left >= 1 && bounds.bottom - bounds.top >= 1)
        pane_ = bounds;
    // The next frame: now if the page has work due, at its next timer, or none.
    if (onScreen && agent_ != nullptr) {
        double delay = prDomAgentNextTaskDelay(agent_);
        if (delay == 0 || dirty_ || scroller_.busy())
            prUiRequestFrame(ui);
        else if (delay > 0)
            prUiRequestFrameAt(ui, frameTime_ + delay / 1000);
    }
    return true;
}

void PraxBoard::hidden() {
    endScroll();
    shown_ = false;
    pressInPage_ = false;
}

void PraxBoard::setTextScale(float k) {
    if (k > 0 && k != zoom_) {
        zoom_ = k;
        dirty_ = true;
    }
}

bool PraxBoard::event(const PrAppEvent& e) {
    if (!shown_ || view_ == nullptr || recorded_ == nullptr)
        return false;
    PrVec2 at{};
    const bool positioned = prAppEventPosition(&e, &at);
    const bool inPage = positioned && at.x >= pane_.left && at.x < pane_.right && at.y >= pane_.top &&
        at.y < pane_.bottom;
    BoardRoute route = routeBoardEvent(e, inPage, pressInPage_);
    if (e.kind == PrAppEventKind_PointerDown)
        pressInPage_ = route == BoardRoute::Page;
    if (route == BoardRoute::Window) {
        // A key of the window's (a tab's Command-digit, say) ends a held arrow's scrolling.
        if (e.kind == PrAppEventKind_KeyDown && scroller_.held())
            scroller_.release(frameTime_);
        return false;
    }
    // In the page's CSS px: its position from the pane's corner, and a trackpad's pixel deltas, over the text scale.
    PrAppEvent copy = prAppEventPlaced(&e, PrVec2{pane_.left, pane_.top}, zoom_);
    scrollEvent(copy);
    if (e.kind == PrAppEventKind_PointerUp)
        pressInPage_ = false;
    dirty_ = true;
    return route == BoardRoute::Page;
}

PrDomElement* PraxBoard::scroller() {
    PrDomDocument* document = page_ != nullptr ? prHtmlWindowDocument(prHtmlPageWindow(page_)) : nullptr;
    return document != nullptr ? prCssDocumentScrollingElement(document) : nullptr;
}

namespace {

// Which way a key scrolls the page, as Prax's view scrolls by keys (html_view.cpp scrollByKey): -1 up, 1 down, 0 none.
int keyDirection(const PrAppEvent& e) {
    if ((e.modifiers & style::kOption) != 0)
        return 0;
    switch (e.keyboard.key) {
        case PrAppKey_Up: case PrAppKey_PageUp: case PrAppKey_Home: return -1;
        case PrAppKey_Down: case PrAppKey_PageDown: case PrAppKey_End: return 1;
        case ' ': return (e.modifiers & (style::kCommand | PrAppModifierFlag_Control)) != 0 ? 0 :
            (e.modifiers & PrAppModifierFlag_Shift) != 0 ? -1 : 1;
        default: return 0;
    }
}

// Up or Down, -1 or 1, else 0; with `plain`, only with no Command, Control or Option (which change what it does).
int arrowOf(const PrAppEvent& e, bool plain) {
    const PrAppModifierFlags chord = style::kCommand | PrAppModifierFlag_Control | style::kOption;
    if (plain && (e.modifiers & chord) != 0)
        return 0;
    return e.keyboard.key == PrAppKey_Up ? -1 : e.keyboard.key == PrAppKey_Down ? 1 : 0;
}

} // namespace

// Keys and the wheel go through the scroller (scroll_glide.h), which gives each to Prax's view and moves the page where
// it went over the next frames; a trackpad's scroll and a press stop that motion where the page shows and go to the
// view as they are. Returns false for an event the page must not see (a held arrow's repeat).
bool PraxBoard::scrollEvent(const PrAppEvent& e) {
    PrDomElement* s = scroller();
    if (s == nullptr) {
        prHtmlViewEvent(view_, &e, 0);
        return true;
    }
    PageScroll page(s);
    auto apply = [&] { prHtmlViewEvent(view_, &e, 0); };
    switch (e.kind) {
        case PrAppEventKind_KeyDown:
            return scroller_.keyDown(page, frameTime_, arrowOf(e, true), keyDirection(e), e.keyboard.repeat, apply);
        case PrAppEventKind_KeyUp:
            scroller_.keyUp(page, frameTime_, arrowOf(e, false));
            break;
        case PrAppEventKind_Scroll:
            if (e.scroll.unit == PrAppScrollUnit_Line) {
                scroller_.wheel(page, frameTime_, e.scroll.delta.y > 0 ? 1 : e.scroll.delta.y < 0 ? -1 : 0, apply);
                return true;
            }
            scroller_.interrupt(page, frameTime_);
            break;
        case PrAppEventKind_PointerDown:
            scroller_.interrupt(page, frameTime_);
            break;
        default:
            break;
    }
    apply();
    return true;
}

// The page leaves the screen: its scroll goes where it was going, and a held arrow stops.
void PraxBoard::endScroll() {
    if (PrDomElement* s = scroller(); s != nullptr && scroller_.busy()) {
        PageScroll page(s);
        scroller_.hide(page, frameTime_);
        dirty_ = true;
    }
}

bool PraxBoard::settled() {
    // The board's 1 s poll and its transitions keep tasks coming: settled is loaded, painted since the last task, and
    // no fetch waiting, the first poll's answer included.
    return page_ != nullptr && load_ == Load::Loaded && recorded_ != nullptr && !dirty_ && !prHtmlPageIsFetching(page_);
}

std::string PraxBoard::bodyText() {
    if (page_ == nullptr)
        return {};
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    PrDomElement* body = document != nullptr ? prDomDocumentBody(document) : nullptr;
    return body != nullptr ? taken(prDomNodeTextContent(prDomElementAsNode(body))) : std::string();
}

} // namespace nexus
