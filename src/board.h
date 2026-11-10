// The sprint tab's page: nova-sprint's served board in Prax's HTML stack (view.h Board), as Prax's browser and its
// incremental example host a page. One DOM agent, one QuickJS engine and one page, run on the window's thread: each
// frame runs the page's due tasks for a few milliseconds, records the page into a frozen draw list in CSS pixels
// (prHtmlViewPaint), and the window's tree shows it in a replaced node whose painter embeds the list, as the incremental
// example's pane does. The list fills the page's canvas with its own background, so the page is opaque edge to edge over
// the window's see-through fill. After each paint the Cost breakdown panel's place is measured, so that nexus's style
// (sprint.h sprintPageStyle) stretches the panel to one gap above the bottom of the first screen.
//
// The address comes from the sprint's record (sprint.h findSprintBoard), looked up on a thread of its own so that the
// window never waits on it. Until there is a page to show, show() gives one line that says what is happening or what
// failed and what to do.
//
// Off screen (another tab shows, or the overlay is hidden) the page runs nothing: no tasks, no timers, no fetches, no
// frames asked for. Its visibility is hidden (visibilitychange fires), and the board's 1 s poll waits. When it shows
// again, the overdue poll runs in the first frame, and its answer paints as soon as it arrives.

#pragma once

#include <functional>
#include <future>
#include <string>

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
#include "scroll_hold.h"
#include "sprint.h"
#include "view.h"

namespace nexus {

class PraxBoard final : public Board {
public:
    struct Desc {
        PrKit* kit = nullptr;                   // the window's kit, whose tree shows the page
        PrFontSet* fonts = nullptr;             // the window's fonts; the page's set is a child with aliases
        PrApp* app = nullptr;                   // woken when the page has work, and when the address is found; may be null
        // Whose clipboard the page's text controls use, and whose keyboard a held key needs; may be null.
        PrAppWindow* window = nullptr;
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
    };

    explicit PraxBoard(const Desc& desc);
    ~PraxBoard() override;
    PraxBoard(const PraxBoard&) = delete;
    PraxBoard& operator=(const PraxBoard&) = delete;

    // At each frame's start, before its rounds: the frame's time in seconds and its display. Takes the address once it
    // is found, and, while the board shows and the window is on screen, runs the page's due tasks.
    void frame(double seconds, const PrAppDisplay& display);

    // An event while the sprint tab shows (sprint.h routeBoardEvent). Returns true if the page took it and the window
    // should not see it. A key or a wheel's notch that scrolls the page glides its scroll there over the next frames
    // (scroll_glide.h); a trackpad's pixels scroll it at once. Up and Down (without Command, Control or Option) scroll
    // the page every frame while they are down, from the frame after the press to the release, and their repeats are
    // dropped (scroll_hold.h).
    bool event(const PrAppEvent& event);

    bool show(PrLayoutNode parent, PrVec2 extent, std::string* problem) override;
    void hidden() override;
    void setTextScale(float k) override;

    // For tests and the screenshot: true once the document has loaded and its first answers have painted, with nothing
    // due now; the text of the page's body; the line show() gives while there is no page.
    bool settled();
    std::string bodyText();
    std::string problem() const { return said_; }
    // The page's canvas colour at the last paint, which the recorded list fills edge to edge.
    PrColor canvas() const { return canvas_; }
    PrHtmlPage* page() const { return page_; }

private:
    void takeAddress();
    void openPage();
    void navigate();
    void runTasks();
    void paint(PrVec2 extent);
    void addPageStyle();
    SpendFit measureSpend();
    void fitSpend();
    bool showPage(PrLayoutNode parent, PrVec2 extent, std::string* problem);
    PrDomElement* scroller();
    void glideThrough(const PrAppEvent& event);
    void stepGlide();
    void endGlide(bool atTarget);
    static bool arrowKey(const PrAppEvent& event, HoldKey* key);
    static bool isHeld(const Hold& hold, HoldKey key);
    void pressHeld(const PrAppEvent& event, HoldKey key);
    void stepHeld();

    static double clockNow(void* context);
    static double clockWallNow(void* context);
    static void loaded(void* context, PrHtmlPage* page, PrStatus status);
    static void realmOpened(void* context, JSContext* ctx, PrHtmlPage* page);
    static void wake(void* context);

    Desc desc_;
    PrKit* kit_;
    PrApp* app_;

    // The address: found on a thread, or given.
    std::future<SprintBoardAddress> finding_;
    std::string url_;
    double findAgainAt_ = 0;

    // Prax: fonts, network, agent, engine, page and view.
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

    // The page's state on the window's side.
    enum class Load { None, Loading, Loaded, Failed };
    Load load_ = Load::None;
    std::string loadError_;
    double retryAt_ = 0;
    std::string problem_;               // why the address lookup failed
    std::string said_;                  // the line show() gave last, or empty if it showed the page
    PrDrawList* recorded_ = nullptr;    // the page as last painted, frozen; the painter's context
    PrColor canvas_{};                  // the page's canvas colour at the last paint
    bool dirty_ = true;
    PrVec2 viewport_{};                 // the extent the page was last painted at, in CSS px
    SpendFit spend_;                    // the Cost breakdown as last styled; its top -1 for none
    float zoom_ = 1;
    PrAppDisplay display_ = prAppDisplayDefault();
    PrRect pane_{};                     // the page's rectangle in the window, px, at the last layout
    bool pressInPage_ = false;
    bool shown_ = false;                // the sprint tab showed at the last round
    bool hiddenSet_ = false;            // the page's visibility is hidden
    // The page's scroll gliding to where a wheel's notch or a key sent it (scroll_glide.h), and the scroll it was set
    // to last, which tells whether the page scrolled itself since.
    Glide glide_;
    double applied_ = 0;
    // Up or Down held (scroll_hold.h), and the place the hold has moved the page's scroll to, to a fraction of a pixel.
    Hold hold_;
    double heldAt_ = 0;
    double frameTime_ = 0;
};

} // namespace nexus
