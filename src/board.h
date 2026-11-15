// The sprint tab's page: nova-sprint's served board in Prax's HTML stack (view.h Board), as Prax's browser and its
// incremental example host a page. One DOM agent, one QuickJS engine and one page, run on a thread of the board's own,
// the agent's thread, as Prax's browser runs each tab's page on its own thread: the window never waits on the page
// (AGENTS.md, the frame budget). That thread makes the page (its network, TLS, script engine and fonts) when the tab
// first shows, runs the page's tasks as they come due, moves its scroll, and records the page into a frozen draw list
// in CSS pixels (prHtmlViewPaint) whenever it changed; the window's tree shows the latest list in a replaced node whose
// painter embeds it, as the incremental example's pane does. The list fills the page's canvas with its own background,
// so the page is opaque edge to edge over the window's see-through fill. After each paint the Cost breakdown panel's
// place is measured, so that nexus's style (sprint.h sprintPageStyle) stretches the panel to one gap above the bottom of
// the first screen.
//
// The window's thread hands the page's thread what it needs as messages: each frame's time, display and visibility,
// the pane's extent, the text scale, the events routed to the page, and home. The page's thread hands back what the
// window shows, under a lock: the latest list, the load's state, when it next has work, and the links to open, which
// the window's thread opens. Each new list wakes the app for a frame.
//
// The address comes from the sprint's record (sprint.h findSprintBoard), looked up on a thread of its own so that the
// window never waits on it. Until there is a page to show, show() gives one line that says what is happening or what
// failed and what to do.
//
// The tab always shows the board. A click on a link that would leave it (sprint.h linkLeavesBoard) is canceled, and the
// link opens in the system's browser (Desc::open); a link to a place on the board scrolls there in the tab. Prax gives a
// host no say over a same-tab navigation (html_window.cpp htmlWindowFollowHyperlink, 903-921, goes to
// prHtmlWindowSetLocation with no host hook), so the board listens for click on each window the page opens, and cancels
// it after the page's own listeners, before the link's activation follows it. A page's script that opens a window asks
// the host (PrHtmlHost::openWindow), which goes to the browser too. Whatever else moves the page off the board, choosing
// the sprint tab brings it back (home).
//
// Off screen (another tab shows, or the overlay is hidden) the page runs nothing: no tasks, no timers, no fetches, no
// frames asked for. Its visibility is hidden (visibilitychange fires), and the board's 1 s poll waits. When it shows
// again, the overdue poll runs in the first frame, and its answer paints as soon as it arrives.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pr/app.h"
#include "pr/draw_list.h"
#include "pr/fetch_intercept.h"
#include "pr/font_set.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/html_view.h"
#include "pr/kit.h"
#include "pr/text.h"

#include "scroll_glide.h"
#include "sprint.h"
#include "view.h"

namespace nexus {

class PraxBoard final : public Board {
public:
    struct Desc {
        PrKit* kit = nullptr;                   // the window's kit, whose tree shows the page
        PrFontSet* fonts = nullptr;             // the window's fonts; the page's set is a child with aliases
        PrApp* app = nullptr;                   // woken when the page has work, and when the address is found; may be null
        PrAppWindow* window = nullptr;          // whose clipboard the page's text controls use; may be null
        // The board's address. Empty: `find` on a thread of its own (findSprintBoard if unset).
        std::string url;
        std::function<SprintBoardAddress()> find;
        // A test's network: every request goes to `answer`, over Prax's null network, whose loop each frame runs. No
        // socket is opened. Unset: the platform's sockets and TLS.
        PrFetchInterceptRequestFn answer = nullptr;
        void* answerContext = nullptr;
        // The page's clocks (PrHtmlHost now and wallNow), in milliseconds; null for the system's.
        double (*now)(void* context) = nullptr;
        double (*wallNow)(void* context) = nullptr;
        void* clockContext = nullptr;
        // True while the window is on screen (the overlay is shown); unset, always.
        std::function<bool()> visible;
        // Shows a link that would take the tab off the board (sprint.h linkLeavesBoard): the window gives its
        // UrlOpener's (url_opener.h), which opens it off the frame thread; unset, the system's handler for it on this
        // thread (pr/app_shell.h prAppOpenUrl over `app`). A test's opener records it.
        std::function<void(const std::string& url)> open;
    };

    explicit PraxBoard(const Desc& desc);
    ~PraxBoard() override;
    PraxBoard(const PraxBoard&) = delete;
    PraxBoard& operator=(const PraxBoard&) = delete;

    // At each frame's start, before its rounds: the frame's time in seconds and its display. Takes the address once it
    // is found, and, while the board shows and the window is on screen, runs the page's due tasks.
    void frame(double seconds, const PrAppDisplay& display);

    // An event while the sprint tab shows (sprint.h routeBoardEvent). Returns true if the page took it and the window
    // should not see it. Keys and the wheel move the page's scroll over the next frames, with momentum, a held arrow
    // scrolls while it is held, and the page bounces at its ends (scroll_glide.h); a trackpad's pixels scroll it at
    // once.
    bool event(const PrAppEvent& event);

    bool show(PrLayoutNode parent, PrVec2 extent, std::string* problem) override;
    void hidden() override;
    void setTextScale(float k) override;
    // Loads the board again if the page shows another document, or is on its way to one.
    void home() override;

    // For tests and the screenshot: true once the document has loaded and its first answers have painted, with nothing
    // due now (as of the last step of the page's thread); the line show() gives while there is no page.
    bool settled();
    std::string problem() const { return said_; }
    // The page's canvas colour at the last paint, which the recorded list fills edge to edge.
    PrColor canvas();
    // How far past an end the page is drawn now, CSS px (scroll_glide.h GlideScroller::overscroll).
    double overscroll();
    // The page's rectangle in the window, px, at the last layout.
    PrRect pane() const { return pane_; }

    // For tests: waits until the page's thread has taken up everything handed to it so far and said what came of it.
    void wait();
    // For tests: runs `fn` on the page's thread with the page (null before it is made), and waits for it.
    void onPage(const std::function<void(PrHtmlPage* page)>& fn);
    // For tests, on the page's thread: the text of the page's body, and the URL of the document the page shows (empty
    // before it has one).
    std::string bodyText();
    std::string documentUrl();

private:
    // What the window's thread hands the page's thread.
    struct Message {
        enum class Kind { Event, WindowKey, Hidden, Home, Run } kind;
        PrAppEvent event{};                     // Event: placed in the page's CSS px; its text is in `text`
        std::string text;
        std::function<void(PrHtmlPage*)> run;  // Run
        double time = 0;                        // the window's frame when it was handed over, s
    };
    // What the page's thread hands back, as of its last step.
    struct Shown {
        bool opened = false;                    // the page is made
        bool failed = false;                    // its load failed (`error` says why) and it tries again at `retryAt`
        std::string error;
        double retryAt = 0;
        PrDrawList* recorded = nullptr;         // the page as last painted, frozen, one reference held here
        PrColor canvas{};
        double overscroll = 0;
        bool settled = false;
        bool busy = false;                      // work is due now: tasks, a paint, the scroll moving
        double nextTask = -1;                   // ms until the page's next timer, or -1 for none
        std::vector<std::string> opens;         // links to open in the system's browser
    };

    void takeAddress();
    bool showPage(PrLayoutNode parent, PrVec2 extent, std::string* problem);
    void post(Message message);
    void openLink(const std::string& url);

    // The page's thread.
    void run();
    void step(std::vector<Message>& inbox);
    void publish();
    void openPage();
    void closePage();
    void navigate();
    void runTasks();
    void paint();
    void addPageStyle();
    SpendFit measureSpend();
    void fitSpend();
    PrDomElement* scroller();
    bool scrollEvent(const PrAppEvent& event);
    void endScroll();
    std::string pageUrl() const;

    static double clockNow(void* context);
    static double clockWallNow(void* context);

    static void loaded(void* context, PrHtmlPage* page, PrStatus status);
    static void windowOpened(void* context, PrHtmlPage* page);
    static void openWindow(void* context, PrHtmlPage* page, PrStringView url);
    static void linkClicked(void* context, PrDomEvent* event);
    static void realmOpened(void* context, JSContext* ctx, PrHtmlPage* page);

    Desc desc_;
    PrKit* kit_;
    PrApp* app_;

    // The window's thread's.
    std::future<SprintBoardAddress> finding_;
    std::string url_;                       // the address, once found (found on a thread, or given)
    double findAgainAt_ = 0;
    std::string problem_;                   // why the address lookup failed
    std::string said_;                      // the line show() gave last, or empty if it showed the page
    PrDrawList* showing_ = nullptr;         // the list the pane embeds, one reference held here
    double windowTime_ = 0;
    PrAppDisplay windowDisplay_ = prAppDisplayDefault();
    PrRect pane_{};                         // the page's rectangle in the window, px, at the last layout
    bool pressInPage_ = false;
    bool shown_ = false;                    // the sprint tab showed at the last round
    float windowZoom_ = 1;

    // Between the threads, under `mutex_`: the inbox, the frame's state for the page's thread, and what it shows.
    std::mutex mutex_;
    std::condition_variable wake_;          // the page's thread, while it has no page or the page is off screen
    std::condition_variable stepped_;       // a step ended (wait)
    bool ending_ = false;
    std::vector<Message> inbox_;
    std::uint64_t posted_ = 0;              // what was handed over, counted, and what the page's thread took up
    std::uint64_t taken_ = 0;
    std::uint64_t done_ = 0;
    double time_ = 0;                       // the window's last frame
    PrAppDisplay display_ = prAppDisplayDefault();
    bool onScreen_ = false;
    std::string address_;                   // url_, for the page's thread
    PrVec2 extent_{};                       // the pane's, px
    float zoom_ = 1;
    PrDomAgent* waiting_ = nullptr;         // the agent the page's thread waits on, to wake it
    Shown out_;

    // The page's thread's own. Prax: fonts, network, agent, engine, page and view.
    PrFontSet* fonts_ = nullptr;
    PrTextShaper* shaper_ = nullptr;
    PrTextFont* font_ = nullptr;
    PrCompressCodec* compress_ = nullptr;
    PrImageCodec* images_ = nullptr;
    PrNet* net_ = nullptr;
    bool nullNet_ = false;
    PrTls* tls_ = nullptr;
    PrHttp* http_ = nullptr;
    PrFetchNetwork* network_ = nullptr;
    PrFetchNetwork* fetcher_ = nullptr;
    PrHtmlStorageProvider* localStorage_ = nullptr;
    PrHtmlStorageProvider* sessionStorage_ = nullptr;
    PrDomAgent* agent_ = nullptr;
    PrJsHtmlEngine* engine_ = nullptr;
    PrHtmlPage* page_ = nullptr;
    PrHtmlView* view_ = nullptr;
    std::string prelude_;
    std::string pageUrl_;               // the address, as handed over
    double frameTime_ = 0;              // the window's last frame, s
    PrAppDisplay pageDisplay_ = prAppDisplayDefault();
    bool visible_ = false;              // on screen: the tab shows and the window is visible
    PrVec2 pageExtent_{};               // the pane's, px
    float pageZoom_ = 1;
    enum class Load { None, Loading, Loaded, Failed };
    Load load_ = Load::None;
    bool ownLoad_ = false;              // the load in flight is the board's own (navigate)
    std::string home_;                  // the board's document's URL, without its fragment, as its own load ended
    std::string loadError_;
    double retryAt_ = 0;
    PrDrawList* recorded_ = nullptr;    // the page as last painted, frozen
    PrColor canvas_{};                  // the page's canvas colour at the last paint
    bool dirty_ = true;
    PrVec2 viewport_{};                 // the extent the page was last painted at, in CSS px
    SpendFit spend_;                    // the Cost breakdown as last styled; its top -1 for none
    float paintedZoom_ = 0;
    bool hiddenSet_ = false;            // the page's visibility is hidden
    std::vector<std::string> opens_;    // links to open, until published
    // The page's scroll as keys and the wheel move it (scroll_glide.h).
    GlideScroller scroller_;

    std::thread thread_;                // last: it starts once everything above is made
};

} // namespace nexus
