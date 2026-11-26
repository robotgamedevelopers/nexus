#include "pr/compress_select.h"
#include "pr/image_select.h"
#include "board.h"

#include <algorithm>
#include <cmath>
#include <charconv>
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

// How long a step of the page's thread runs the page's due tasks, in milliseconds, before it paints: a long load
// renders as it goes. As Prax's browser runs its pages, on a thread of their own.
constexpr double kTaskBudget = 8;

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


// ---------------------------------------------------------------------------------------------------------------------
// The window's thread

PraxBoard::PraxBoard(const Desc& desc) : desc_(desc), kit_(desc.kit), app_(desc.app) {
    extent_ = desc.initialExtent;
    prewarming_ = extent_.x > 0 && extent_.y > 0;
    posted_ = extent_.x > 0 && extent_.y > 0 ? 1 : 0;
    const ZoneNames zones = localZoneNames();
    prelude_ = sprintPrelude(zones.standard, zones.daylight);
    thread_ = std::thread([this] { run(); });
}

PraxBoard::~PraxBoard() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ending_ = true;
        if (waiting_ != nullptr) {
            prDomAgentStop(waiting_);
        }
    }
    wake_.notify_all();
    thread_.join();     // the page's thread frees the page as it ends (closePage)
    prDrawListRelease(showing_);
    prDrawListRelease(out_.recorded);
}

void PraxBoard::post(Message message) {
    message.time = windowTime_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.push_back(std::move(message));
        ++posted_;
        if (waiting_ != nullptr) {
            prDomAgentWake(waiting_);
        }
    }
    wake_.notify_one();
}

void PraxBoard::frame(double seconds, const PrAppDisplay& display) {
    windowTime_ = seconds;
    windowDisplay_ = display;
    const bool onScreen = shown_ && (!desc_.visible || desc_.visible());
    // The frame, for the page's thread: while the page is on screen, and once as it leaves.
    std::vector<std::string> opens;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool tick = onScreen || onScreen_ || (refreshStep_ && !out_.settled && !out_.failed);
        time_ = seconds;
        display_ = display;
        onScreen_ = onScreen;

        opens.swap(out_.opens);
        if (tick) {
            ++posted_;
            if (waiting_ != nullptr) {
                prDomAgentWake(waiting_);
            }
        }
    }
    wake_.notify_one();
    for (const std::string& url : opens) {
        openLink(url);
    }
}

void PraxBoard::openLink(const std::string& url) {
    if (desc_.open) {
        desc_.open(url);
        return;
    }
    if (app_ == nullptr) {
        return;
    }
    const PrStatus status = prAppOpenUrl(app_, sv(url));
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: the system could not open %s (status %d)\n", url.c_str(), int(status));
    }
}

bool PraxBoard::show(PrLayoutNode parent, PrVec2 extent, std::string* problem) {
    said_.clear();
    bool page = showPage(parent, extent, &said_);
    if (!page) {
        *problem = said_;
    }
    return page;
}

bool PraxBoard::showPage(PrLayoutNode parent, PrVec2 extent, std::string* problem) {
    shown_ = true;
    PrUi* ui = kit_->ui;
    PrLayoutTree* tree = kit_->tree;
    const bool onScreen = !desc_.visible || desc_.visible();
    bool opened = false;
    bool failed = false;
    double retryAt = 0;
    bool busy = false;
    double nextTask = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (extent.x != extent_.x || extent.y != extent_.y) {
            extent_ = extent;
            ++posted_;
            if (waiting_ != nullptr) {
                prDomAgentWake(waiting_);
            }
        }
        *problem = lookupError_;
        opened = out_.opened;
        failed = out_.failed;
        if (failed) {
            *problem = out_.error;
        }
        retryAt = out_.retryAt;
        busy = out_.busy;
        nextTask = out_.nextTask;
        if (out_.recorded != showing_) {
            prDrawListRelease(showing_);
            showing_ = out_.recorded;
            if (showing_ != nullptr) {
                prDrawListRetain(showing_);
            }
        }
    }
    wake_.notify_one();
    if (!opened) {
        if (onScreen) {
            prUiRequestFrame(ui);
        }
        return false;
    }
    if (failed) {
        if (onScreen) {
            prUiRequestFrameAt(ui, retryAt);
        }
        return false;
    }
    if (showing_ == nullptr) {
        if (onScreen) {
            prUiRequestFrame(ui);
        }
        return false;
    }
    // The pane: a replaced element of the extent given, whose painter embeds the page (incremental.cpp 729-737).
    PrLayoutNode pane = prLayoutTreeFrameReplaced(tree, parent, 0, 0);
    PrKitPlace place{};
    place.width = prKitPx(extent.x);
    place.height = prKitPx(extent.y);
    place.flex = PrKitFlex_Fixed;
    prKitPlace(kit_, pane, place);
    PrLayoutContent content{.kind = PrLayoutContentKind_Painter};
    content.painter = PrLayoutPainter{.context = showing_, .paint = paintPage, .measure = nullptr,
        .retain = retainPage, .release = releasePage};
    prLayoutTreeFrameSetContent(tree, pane, &content);
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(tree, pane, PrLayoutCoordinates_Viewport));
    if (bounds.right - bounds.left >= 1 && bounds.bottom - bounds.top >= 1) {
        pane_ = bounds;
    }
    // The next frame: now if the page has work due (its thread wakes the app when a new paint is ready), at its next
    // timer, or none.
    if (onScreen) {
        if (busy) {
            prUiRequestFrame(ui);
        } else if (nextTask > 0) {
            prUiRequestFrameAt(ui, windowTime_ + nextTask / 1000);
        }
    }
    return true;
}

void PraxBoard::hidden() {
    if (shown_) {
        post(Message{Message::Kind::Hidden, {}, {}, {}});
    }
    shown_ = false;
    pressInPage_ = false;
}

void PraxBoard::setTextScale(float k) {
    if (k <= 0 || k == windowZoom_) {
        return;
    }
    windowZoom_ = k;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        zoom_ = k;
        ++posted_;
        if (waiting_ != nullptr) {
            prDomAgentWake(waiting_);
        }
    }
    wake_.notify_one();
}

void PraxBoard::prepare(PrVec2 extent) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (extent.x > 0 && extent.y > 0) extent_ = extent;
        refreshStep_ = posted_ + 1;
    }
    post(Message{Message::Kind::Refresh, {}, {}, {}});
}

void PraxBoard::cancelPreparation() {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshStep_ = 0;
    if (waiting_) prDomAgentWake(waiting_);
}

bool PraxBoard::prepared() {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ >= refreshStep_ && (out_.settled || out_.failed || !lookupError_.empty());
}

void PraxBoard::home() {
    post(Message{Message::Kind::Home, {}, {}, {}});
}

bool PraxBoard::event(const PrAppEvent& e) {
    if (!shown_ || showing_ == nullptr) {
        return false;
    }
    PrVec2 at{};
    const bool positioned = prAppEventPosition(&e, &at);
    const bool inPage = positioned && at.x >= pane_.left && at.x < pane_.right && at.y >= pane_.top &&
        at.y < pane_.bottom;
    BoardRoute route = routeBoardEvent(e, inPage, pressInPage_);
    if (e.kind == PrAppEventKind_PointerDown) {
        pressInPage_ = route == BoardRoute::Page;
    }
    if (route == BoardRoute::Window) {
        // A key of the window's (a tab's Command-digit, say) ends a held arrow's scrolling.
        if (e.kind == PrAppEventKind_KeyDown) {
            post(Message{Message::Kind::WindowKey, {}, {}, {}});
        }
        return false;
    }
    // A drag's data and a trackpad's touches are the platform's, valid only while the event is delivered: they stay on
    // this thread, and the page does not see them.
    const bool platformData = e.kind == PrAppEventKind_TrackpadTouches || e.kind == PrAppEventKind_DragEnter ||
        e.kind == PrAppEventKind_DragOver || e.kind == PrAppEventKind_DragLeave || e.kind == PrAppEventKind_Drop ||
        e.kind == PrAppEventKind_DragPromise;
    if (!platformData) {
        // In the page's CSS px: its position from the pane's corner, and a trackpad's pixel deltas, over the text scale.
        Message m{Message::Kind::Event, prAppEventPlaced(&e, PrVec2{pane_.left, pane_.top}, windowZoom_), {}, {}};
        const PrStringView text = prAppEventText(&m.event);
        m.text.assign(text.data != nullptr ? text.data : "", text.size);
        post(std::move(m));
    }
    if (e.kind == PrAppEventKind_PointerUp) {
        pressInPage_ = false;
    }
    return route == BoardRoute::Page;
}

std::uint64_t PraxBoard::postedStep() {
    std::lock_guard<std::mutex> lock(mutex_);
    return posted_;
}

bool PraxBoard::readyToReveal(std::uint64_t step) {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ >= step && (out_.settled || out_.failed || !lookupError_.empty());
}

bool PraxBoard::settled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return out_.settled;
}

PrColor PraxBoard::canvas() {
    std::lock_guard<std::mutex> lock(mutex_);
    return out_.canvas;
}

double PraxBoard::overscroll() {
    std::lock_guard<std::mutex> lock(mutex_);
    return out_.overscroll;
}

void PraxBoard::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t posted = posted_;
    wake_.notify_one();
    stepped_.wait(lock, [&] { return done_ >= posted || ending_; });
}

void PraxBoard::onPage(const std::function<void(PrHtmlPage* page)>& fn) {
    post(Message{Message::Kind::Run, {}, {}, fn});
    wait();
}

std::string PraxBoard::bodyText() {
    std::string text;
    onPage([&](PrHtmlPage* page) {
        if (page == nullptr) {
            return;
        }
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
        PrDomElement* body = document != nullptr ? prDomDocumentBody(document) : nullptr;
        text = body != nullptr ? taken(prDomNodeTextContent(prDomElementAsNode(body))) : std::string();
    });
    return text;
}

std::string PraxBoard::documentUrl() {
    std::string url;
    onPage([&](PrHtmlPage*) { url = pageUrl(); });
    return url;
}

// ---------------------------------------------------------------------------------------------------------------------
// The page's thread

void PraxBoard::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    pageUrl_ = desc_.url;
    while (pageUrl_.empty() && !ending_) {
        lock.unlock();
        auto found = desc_.find ? desc_.find() : findSprintBoard();
        lock.lock();
        lookupError_ = found.error;
        pageUrl_ = std::move(found.url);
        if (app_) prAppWake(app_);
        if (pageUrl_.empty()) {
            done_ = posted_;
            stepped_.notify_all();
            // Process frame acknowledgments while the service is unavailable.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(int(kRetrySeconds));
            while (!ending_ && std::chrono::steady_clock::now() < deadline) {
                wake_.wait_until(lock, deadline);
                done_ = posted_;
                stepped_.notify_all();
            }
        }
    }
    for (;;) {
        if (ending_) {
            break;
        }
        if (inbox_.empty() && taken_ == posted_) {
            if (agent_ != nullptr && (onScreen_ || (refreshStep_ && (done_ < refreshStep_ || (!out_.settled && !out_.failed))) || prewarming_)) {
                // Until a task can run, a message comes, the layout thread finishes, or the window wakes it.
                waiting_ = agent_;
                lock.unlock();
                prDomAgentWait(agent_, -1);
                lock.lock();
                waiting_ = nullptr;
            } else {
                wake_.wait(lock, [this] { return ending_ || !inbox_.empty() || taken_ != posted_; });
            }
            if (ending_) {
                break;
            }
        }
        std::vector<Message> inbox;
        inbox.swap(inbox_);
        const std::uint64_t taken = posted_;
        taken_ = taken;
        frameTime_ = time_;
        pageDisplay_ = display_;
        visible_ = onScreen_ || (refreshStep_ && (done_ < refreshStep_ || (!out_.settled && !out_.failed))) || prewarming_;

        pageExtent_ = extent_;
        pageZoom_ = zoom_;
        lock.unlock();
        step(inbox);
        publish();
        lock.lock();
        done_ = taken;
        stepped_.notify_all();
    }
    done_ = posted_;
    stepped_.notify_all();
    lock.unlock();
    closePage();
}

void PraxBoard::step(std::vector<Message>& inbox) {
    if (page_ != nullptr && hiddenSet_ == visible_) {
        prHtmlPageSetVisibility(page_, PrHtmlVisibility(visible_ ? PrHtmlVisibility_Visible : PrHtmlVisibility_Hidden));
        hiddenSet_ = !visible_;
    }
    // Each message at the frame it came in, as the window's thread would have taken it then; then this frame.
    const double now = frameTime_;
    for (Message& m : inbox) {
        frameTime_ = std::min(m.time, now);
        switch (m.kind) {
            case Message::Kind::Run:
                m.run(page_);
                break;
            case Message::Kind::Hidden:
                endScroll();
                break;
            case Message::Kind::WindowKey:
                if (scroller_.held()) {
                    scroller_.release(frameTime_);
                }
                break;
            case Message::Kind::Refresh:
                if (page_) {
                    endScroll();
                    // A live Sprint board refreshes its existing rows before reveal. Other pages reload.
                    PrDomEventInit init = prDomEventInitDefault();
                    init.cancelable = true;
                    auto* event = prDomEventCreate(sv("nexus-dashboard-refresh"), &init);
                    bool reload = true;
                    prDomEventTargetDispatchEvent(prHtmlWindowAsEventTarget(prHtmlPageWindow(page_)), event, &reload);
                    prDomEventRelease(event);
                    if (reload) navigate();
                }
                break;
            case Message::Kind::Home:
                if (page_ != nullptr && !pageUrl_.empty() && !home_.empty()) {
                    const bool away = withoutFragment(pageUrl()) != home_ ||
                        (prHtmlPageIsNavigating(page_) && !ownLoad_);
                    if (away) {
                        endScroll();
                        navigate();
                    }
                }
                break;
            case Message::Kind::Event:
                if (view_ != nullptr && recorded_ != nullptr) {
                    prAppEventSetText(&m.event, PrStringView{m.text.data(), m.text.size()});
                    scrollEvent(m.event);
                    dirty_ = true;
                }
                break;
        }
    }
    frameTime_ = now;
    if (!visible_) {
        endScroll();
        return;
    }
    if (pageUrl_.empty()) {
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
        if (scroller_.frame(page, frameTime_)) {
            dirty_ = true;
        }
    }
    runTasks();
    if (load_ != Load::Failed) {
        paint();
    }
}

void PraxBoard::publish() {
    const double delay = agent_ != nullptr ? prDomAgentNextTaskDelay(agent_) : -1;
    auto* doc = page_ ? prHtmlWindowDocument(prHtmlPageWindow(page_)) : nullptr;
    auto* pie = doc ? prDomNodeGetElementById(prDomDocumentAsNode(doc), sv("pie")) : nullptr;
    // Sprint inserts the first slice (or its empty-state circle) after accepting its snapshot.
    // Other documents, including static charts, do not use that dashboard contract.
    const bool chartReady = !pie || !prDomNodeGetElementById(prDomDocumentAsNode(doc), sv("landed")) ||
        prDomNodeFirstElementChild(prDomElementAsNode(pie));
    const bool settled = chartReady && delay != 0 && page_ != nullptr && load_ == Load::Loaded && recorded_ != nullptr && !dirty_ &&
        !prHtmlPageIsFetching(page_);
    if (settled && !home_.empty() && withoutFragment(pageUrl()) == home_) {
        auto* window = prHtmlPageWindow(page_);
        auto* storage = prHtmlWindowStorage(window, PrHtmlStorageKind_Local);
        auto* scroll = scroller();
        if (storage && scroll) {
            const auto key = sv("nexus-dashboard-scroll");
            if (restoreScroll_) {
                restoreScroll_ = false;
                const auto stored = taken(prHtmlStorageGetItem(storage, key));
                long top = 0;
                auto parsed = std::from_chars(stored.data(), stored.data() + stored.size(), top);
                if (parsed.ec == std::errc{} && top > 0) {
                    prCssElementSetScrollTop(scroll, top);
                    dirty_ = true;
                    paint();
                }
            }
            const long top = std::lround(prCssElementScrollTop(scroll));
            if (top != savedScroll_) {
                prHtmlStorageSetItem(storage, key, sv(std::to_string(top)));
                savedScroll_ = top;
            }
        }
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settled && out_.recorded != recorded_) {
            prDrawListRelease(out_.recorded);
            out_.recorded = recorded_;
            if (recorded_ != nullptr) {
                prDrawListRetain(recorded_);
            }
            changed = true;
        }
        const bool failed = load_ == Load::Failed;
        changed = changed || out_.opened != (page_ != nullptr) || out_.failed != failed || !opens_.empty();
        out_.opened = page_ != nullptr;
        out_.failed = failed;
        out_.error = loadError_;
        out_.retryAt = retryAt_;
        out_.canvas = canvas_;
        out_.overscroll = scroller_.overscroll();
        changed = changed || out_.settled != settled;
        out_.settled = settled;
        if (settled || failed) prewarming_ = false;
        out_.busy = dirty_ || scroller_.busy() || delay == 0;
        out_.nextTask = delay;
        for (std::string& url : opens_) {
            out_.opens.push_back(std::move(url));
        }
        opens_.clear();
    }
    if (changed && app_ != nullptr) {
        prAppWake(app_);   // a frame shows it (pr/studio_app.h)
    }
}

void PraxBoard::openPage() {
    // The page's fonts: the window's, with the system's faces under the names the board asks for.
    fonts_ = prFontSetCreate(desc_.fonts);
    for (const Alias& a : kAliases) {
        PrStringView family = prFontSetGenericFamily(desc_.fonts, a.generic);
        if (family.size != 0) {
            prFontSetAlias(fonts_, sv(a.name), family);
        }
    }
    // The page's text is shaped on the agent's layout thread as well as this one: a shaper of its own (Prax's browser).
    shaper_ = prTextShaperCreate(PrTextShaperBackend_Default);
    PrTextFontDesc font{.set = fonts_, .style = prFontStyleDefault()};
    font_ = prTextFontCreate(&font);
    compress_ = prCompressCodecSetCreateDefault();
    images_ = compress_ != nullptr ? prImageCodecSetCreateDefault(compress_) : nullptr;

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
    // The page owns its fold preferences in origin-scoped local storage.
    PrHtmlStorageProviderDesc storage{.quotaBytes = kPrHtmlStorageQuotaBytes};
    sessionStorage_ = prHtmlStorageProviderCreate(&storage);
    if (!desc_.storageDirectory.empty()) {
        storagePool_ = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
        if (storagePool_) {
            storage.directory = sv(desc_.storageDirectory);
            storage.pool = storagePool_;
        }
    }
    localStorage_ = prHtmlStorageProviderCreate(&storage);

    // A task posted to the agent (a fetch's answer, say) ends the page's thread's wait by itself (prDomAgentWait).
    agent_ = prDomAgentCreate(nullptr);
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
    if (desc_.window != nullptr) {
        prHtmlViewSetWindow(view_, desc_.window);
    }
}

// The page's view, then the engine (its objects' finalizers reach the page's window), then the page, the agent and the
// network, as Prax's browser closes (tab.cpp TaskKind_Shutdown, main.cpp closeShared).
void PraxBoard::closePage() {
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
    prTaskPoolDestroy(storagePool_);
    prImageCodecSetDestroy(images_);
    prCompressCodecSetDestroy(compress_);
    prDrawListRelease(recorded_);
    prTextFontRelease(font_);
    prTextShaperDestroy(shaper_);
    prFontSetDestroy(fonts_);
}

void PraxBoard::navigate() {
    load_ = Load::Loading;
    ownLoad_ = true;
    loadError_.clear();
    if (fetcher_ == nullptr) {
        load_ = Load::Failed;
        loadError_ = "cannot show the sprint board: this build has no network to fetch " + pageUrl_ + " with";
        retryAt_ = 1e300;
        return;
    }
    if (prHtmlPageNavigate(page_, sv(pageUrl_)) != PrStatus_Ok) {
        load_ = Load::Failed;
        loadError_ = "cannot show the sprint board: \"" + pageUrl_ + "\" is not an address; give the board's "
            "address in nexus's client config (a line `sprint-url http://<host>:<port>/` in ~/.nexus/client/config), "
            "or remove that line to read the sprint's record";
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
        b.restoreScroll_ = true;
        b.load_ = Load::Loaded;
        b.loadError_.clear();
        if (own) {
            b.home_ = std::string(withoutFragment(b.pageUrl()));
        }
        return;
    }
    b.load_ = Load::Failed;
    b.loadError_ = "cannot load the sprint board from " + b.pageUrl_ + ": " + statusName(status) + "; check that the "
        "sprint runs (`nova-sprint up`), or give the board's address in ~/.nexus/client/config (a line `sprint-url http://<host>:<port>/`); trying again every " +
        std::to_string(int(kRetrySeconds)) + " seconds";
    b.retryAt_ = b.frameTime_ + kRetrySeconds;
}

std::string PraxBoard::pageUrl() const {
    if (page_ == nullptr) {
        return {};
    }
    PrHtmlWindow* window = prHtmlPageWindow(page_);
    PrDomDocument* document = window != nullptr ? prHtmlWindowDocument(window) : nullptr;
    return document != nullptr ? taken(prDomDocumentUrl(document)) : std::string();
}

// Each window the page opens (one per document) gets the board's click listener, in the bubbling phase: after the
// page's own listeners, which may cancel the click themselves, and before the link's activation follows it.
void PraxBoard::windowOpened(void* context, PrHtmlPage* page) {
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomEventTarget* target = window != nullptr ? prHtmlWindowAsEventTarget(window) : nullptr;
    if (target == nullptr) {
        return;
    }
    const PrStringView click{"click", 5};
    PrDomEventListener listener{.invoke = linkClicked, .context = context, .identity = kLinkListener,
        .retain = nullptr, .release = nullptr};
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(target, click);
    prDomEventTargetAddEventListener(target, click, &listener, &options);
}

// The window's thread opens it (frame).
void PraxBoard::openWindow(void* context, PrHtmlPage*, PrStringView url) {
    static_cast<PraxBoard*>(context)->opens_.emplace_back(url.data != nullptr ? url.data : "", url.size);
}

// A click the page has not canceled, on a link (the nearest a or area with an href on its path, which its activation
// follows) that would leave the board: canceled, and the link opens in the browser.
void PraxBoard::linkClicked(void* context, PrDomEvent* event) {
    PraxBoard& b = *static_cast<PraxBoard*>(context);
    if (prDomEventDefaultPrevented(event)) {
        return;
    }
    PrDomEventTarget* path[kPathCapacity];
    const uint32_t count = std::min(prDomEventComposedPath(event, path, kPathCapacity), kPathCapacity);
    for (uint32_t i = 0; i < count; ++i) {
        if (prHtmlAnchorElementQuery(path[i]) == nullptr && prHtmlAreaElementQuery(path[i]) == nullptr) {
            continue;
        }
        const PrDomElement* link = prDomElementQuery(path[i]);
        if (prDomElementGetAttribute(link, PrStringView{"href", 4}).data == nullptr) {
            continue;
        }
        const std::string href = taken(prHtmlHyperlinkComponent(link, PrUrlComponent_Href));
        const PrStringView target = prDomElementGetAttribute(link, PrStringView{"target", 6});
        const std::string_view targetName = target.data != nullptr ? std::string_view(target.data, target.size) :
            std::string_view();
        if (linkLeavesBoard(href, b.pageUrl(), targetName)) {
            prDomEventPreventDefault(event);
            b.opens_.push_back(href);
        }
        return;
    }
}

void PraxBoard::realmOpened(void* context, JSContext* ctx, PrHtmlPage* page) {
    if (page == nullptr) {
        return;
    }
    const PraxBoard& b = *static_cast<const PraxBoard*>(context);
    prJsHtmlEngineRun(ctx, sv(b.prelude_), PrStringView{"nexus:prelude", 13}, 1, 1);
}

void PraxBoard::runTasks() {
    if (nullNet_) {
        prNetNullRun(net_);
    }
    if (prDomAgentRun(agent_, kTaskBudget)) {
        dirty_ = true;
    }
    if (nullNet_ && prNetNullRun(net_)) {
        dirty_ = true;
    }
}

void PraxBoard::addPageStyle() {
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    if (document) {
        const auto setLabel = [&](const char* id, const char* value) {
            auto* element = prDomNodeGetElementById(prDomDocumentAsNode(document), sv(id));
            if (element && taken(prDomNodeTextContent(prDomElementAsNode(element))) != value)
                prDomNodeSetTextContent(prDomElementAsNode(element), sv(value));
        };
        // The dashboard owns its IDs and fold keys. Change only its default display labels after load.
        if (load_ == Load::Loaded) {
            const auto rename = [](PrDomElement* element, const char* from, const char* to) {
                if (element && taken(prDomNodeTextContent(prDomElementAsNode(element))) == from)
                    prDomNodeSetTextContent(prDomElementAsNode(element), sv(to));
            };
            auto* root = prDomDocumentAsNode(document);
            // The sprint dashboard (nova-sprint) names its section and column "friends"; those ids and labels are its
            // own, so the word stays here only, and nexus shows them as "AIs" and "AI".
            auto* subtitle = prDomNodeGetElementById(root, sv("friends-sub"));
            if (subtitle) rename(prDomNodePreviousElementSibling(prDomElementAsNode(subtitle)), "Friends", "AIs");
            auto* rows = prDomNodeGetElementById(root, sv("friends"));
            auto* header = rows ? prDomNodeFirstElementChild(prDomElementAsNode(rows)) : nullptr;
            if (header) rename(prDomNodeFirstElementChild(prDomElementAsNode(header)), "friend", "AI");
        }
        setLabel("wordmark", "nexus");
        setLabel("ver", NEXUS_DISPLAY_VERSION);
    }
    PrDomElement* head = document != nullptr ? prDomDocumentHead(document) : nullptr;
    if (head == nullptr || prDomNodeGetElementById(prDomDocumentAsNode(document), sv(kSprintStyleId)) != nullptr) {
        return;
    }
    PrDomElement* style = nullptr;
    if (prDomElementCreate(document, PrStringView{"style", 5}, nullptr, &style) != PrStatus_Ok) {
        return;
    }
    prDomElementSetAttribute(style, PrStringView{"id", 2}, sv(kSprintStyleId));
    prDomNodeSetTextContent(prDomElementAsNode(style), sv(sprintPageStyle(spend_)));
    prDomNodeAppendChild(prDomElementAsNode(head), prDomElementAsNode(style));
    prDomNodeRelease(prDomElementAsNode(style));
}

SpendFit PraxBoard::measureSpend() {
    SpendFit fit;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    if (document == nullptr) {
        return fit;
    }
    PrDomElement* spend = nullptr;
    if (prCssNodeQuerySelector(prDomDocumentAsNode(document), sv(kSpendSelector), &spend) != PrStatus_Ok ||
            spend == nullptr) {
        return fit;
    }
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
    if (PrDomElement* after = prDomNodeNextElementSibling(prDomElementAsNode(spend))) {
        fit.below = prCssElementGetBoundingClientRect(after).y - (box.y + box.height);
    }
    return fit;
}

void PraxBoard::fitSpend() {
    SpendFit fit = measureSpend();
    // Half a pixel either way is the same screen: no restyle, so a rounding never repaints in a loop. None of the
    // measures depends on the panel's height, and the gap below is at least the margin the style gives, so one restyle
    // settles them.
    if (std::fabs(fit.top - spend_.top) < 0.5 && std::fabs(fit.gap - spend_.gap) < 0.5 &&
            std::fabs(fit.below - spend_.below) < 0.5) {
        return;
    }
    spend_ = fit;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page_));
    PrDomElement* style = document != nullptr
        ? prDomNodeGetElementById(prDomDocumentAsNode(document), sv(kSprintStyleId)) : nullptr;
    if (style == nullptr) {
        return;
    }
    prDomNodeSetTextContent(prDomElementAsNode(style), sv(sprintPageStyle(spend_)));
    dirty_ = true;
}

// Records the page at the pane's extent if it changed since the last paint.
void PraxBoard::paint() {
    if (pageExtent_.x <= 0 || pageExtent_.y <= 0) {
        return;     // the pane has not been laid out yet
    }
    PrVec2 viewport{pageExtent_.x / pageZoom_, pageExtent_.y / pageZoom_};
    if (viewport.x != viewport_.x || viewport.y != viewport_.y || pageZoom_ != paintedZoom_) {
        prHtmlViewSetViewport(view_, viewport);
        viewport_ = viewport;
        paintedZoom_ = pageZoom_;
        dirty_ = true;
    }
    PrAppDisplay display = pageDisplay_;
    display.pixelRatio *= pageZoom_;
    prHtmlViewSetDisplay(view_, &display);
    if (!dirty_ || prHtmlPageIsRenderBlocked(page_)) {
        return;
    }
    addPageStyle();
    PrDrawList* list = prDrawListCreate();
    // Past an end, the page is drawn rubber-banded (scroll_glide.h): moved by how far past, over its own background.
    const float over = float(scroller_.overscroll());
    if (over != 0) {
        prDrawListRect(list, PrRect{0, 0, pageExtent_.x, pageExtent_.y},
            PrDrawPaint{.color = prColor16FromColor(canvas_)});
    }
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Translate(PrVec2{0, -over}),
        prAffine2Scale(PrVec2{pageZoom_, pageZoom_})));
    canvas_ = prHtmlViewPaint(view_, list);
    prDrawListPopTransform(list);
    prDrawListFreeze(list);
    prDrawListRelease(recorded_);
    recorded_ = list;
    dirty_ = false;
    // The Cost breakdown's place as laid out in this paint: if it moved, the style is set again and the next step paints
    // the panel down to one gap above the bottom of the screen.
    fitSpend();
}

PrDomElement* PraxBoard::scroller() {
    PrDomDocument* document = page_ != nullptr ? prHtmlWindowDocument(prHtmlPageWindow(page_)) : nullptr;
    return document != nullptr ? prCssDocumentScrollingElement(document) : nullptr;
}

namespace {

// Which way a key scrolls the page, as Prax's view scrolls by keys (html_view.cpp scrollByKey): -1 up, 1 down, 0 none.
int keyDirection(const PrAppEvent& e) {
    if ((e.modifiers & style::kOption) != 0) {
        return 0;
    }
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
    if (plain && (e.modifiers & chord) != 0) {
        return 0;
    }
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

} // namespace nexus
