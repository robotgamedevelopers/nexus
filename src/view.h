// The window, declared each round as a Prax layout tree over lib/ui and Studio Kit: the tab strip (kit tabs), the
// transcript and the prompt (a kit text area). Prax lays it out, paints it, routes input to it, edits the prompt's text
// and keeps the transcript at its newest entry. The view keeps only what Prax does not: each friend's transcript scroll
// while another tab shows. Under a friend's transcript come the open entry as it grows, then the state cursor on its
// own line (cursor.h), coloured from the server's state and breathing while a turn is open.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pr/font_set.h"
#include "pr/text_font.h"
#include "pr/kit.h"
#include "pr/kit_choice.h"
#include "pr/kit_theme.h"
#include "pr/layout_tree.h"
#include "pr/ui.h"

#include "account_menu.h"
#include "account_picker.h"
#include "chat_markdown.h"
#include "chat_selection.h"
#include "cursor.h"
#include "hitch.h"
#include "model.h"
#include "window_app.h"
#include "native_tabs.h"

namespace nexus {

// How the window's own records are drawn. Its address owns the window's records (pr/ui_records.h).
struct Look {
    bool overlay = false;           // the window background and tab strip are see-through (style::kOverlayOpacity)
    int textSteps = 0;              // the text scale it is drawn at, in steps from 1 (style::textScale)
    TabStyle tabs{};
    PromptStyle prompt{};
    MessageCardStyle messageCard{};
    TranscriptStyle transcriptStyle{};
    float overlayOpacity = style::kOverlayOpacity;
};

// The kit theme the window is drawn in: Studio's dark theme with the window's colours and type sizes (style.h) at the
// text scale `steps` steps from 1.
PrKitTheme windowTheme(PrFontSet* fonts, bool overlay, int steps = 0, float opacity = style::kOverlayOpacity,
                       const MessageCardStyle& card = {}, const TranscriptStyle& transcript = {}, const TabStyle& tabs = {},
                       const PromptStyle& prompt = {});

// The shells of the terminal tabs, by Tab::id. Our own seam (Prax's terminal cannot run without a process), so that the
// tab logic runs in tests without one; terminals.h is the one over Prax's terminal (pr/terminal.h).
struct Terminals {
    virtual ~Terminals() = default;
    virtual bool open(std::size_t id) = 0;      // starts a new tab's shell; false, having printed why, if it cannot
    virtual void close(std::size_t id) = 0;     // ends its shell and frees it
    // Declares the one that shows this round under `parent`, filling the room the parent's other children leave, and
    // gives it focus.
    virtual void startPending(PrVec2) {}
    virtual void show(std::size_t id, PrLayoutNode parent) = 0;
    virtual void update(std::size_t id) = 0;    // services one that does not show this round
    virtual bool ended(std::size_t id) = 0;     // its shell exited and all of its output was read
    // Empty, or the one plain line its tab shows in place of a shell that cannot run on this machine at all, saying
    // what is missing and what to do (on Windows: WSL2, terminal_launch.h). Such a tab stays until Command-W closes it.
    virtual std::string problem(std::size_t id) {
        (void)id;
        return {};
    }
    // Its tab's label, or empty: the title its shell set, or while ssh runs in it the machine it reaches (remote.h).
    virtual std::string title(std::size_t id) = 0;
    virtual void setTextScale(float k) = 0;     // every terminal's font, at the text scale k
};

// The sprint tab's page: nova-sprint's served board. Our own seam, as Terminals is, so that the tab logic runs in tests
// without a page; board.h is the one over Prax's HTML view (pr/html_view.h).
struct Board {
    virtual ~Board() = default;
    // Declares the page under `parent`, `extent` px, if there is one to show, and returns true. Otherwise declares
    // nothing, writes to `problem` one line that says what is happening or what failed and what to do, and returns false.
    virtual bool show(PrLayoutNode parent, PrVec2 extent, std::string* problem) = 0;
    virtual void hidden() = 0;                  // the sprint tab does not show this round
    virtual void setTextScale(float k) = 0;     // the page's zoom, at the text scale k
    // The sprint tab was chosen, by its Command key or a click on its tab, whether or not it showed already: a page
    // that has left the board goes back to it.
    virtual void home() = 0;
    virtual void prepare(PrVec2) { home(); }
    virtual void cancelPreparation() {}
    virtual bool prepared() { return true; }
};

// The request a secret dialog shows (docs/secrets.md): never a value.
struct SecretAsk {
    std::uint64_t id = 0;       // the request's id: a new one opens the dialog afresh
    std::string account;
    std::string what;           // the file asked for, such as oauth-token
    std::string asker;          // who asks: a friend, or empty for nexus itself
    std::string why;            // the asker's one-line reason, shown as quoted text
};

// The secret dialog's side of the window, with a server (frame_host.h LinkSecrets over sync/secrets.h SecretPrompt).
// Our own seam, so that the dialog runs in the headless window tests without a link. The value lives behind it, never in
// the view.
struct Secrets {
    virtual ~Secrets() = default;
    virtual const SecretAsk* showing() = 0;         // the request the dialog shows, or null for none
    virtual std::string_view typed() = 0;           // the field's text, drawn masked, valid for the round
    virtual void type(std::string_view text) = 0;   // the field's text changed
    virtual void submit() = 0;                      // Enter: the value goes to the server, and the dialog closes
    virtual void cancel() = 0;                      // Escape, or the dialog closed: the request is cancelled
};

// The window's question for its server where nothing names one (server_choice.h): on iOS, at first launch and when
// the person asks to change it. Our own seam, so that the dialog draws in a state shot without a link.
struct ServerChoice {
    virtual ~ServerChoice() = default;
    virtual bool asking() = 0;                      // the dialog shows
    virtual std::string_view current() = 0;         // the server kept now, or empty for none
    virtual std::string_view typed() = 0;           // the field's text, valid for the round
    virtual std::string_view problem() = 0;         // why the last Enter was not taken, or empty
    virtual void type(std::string_view text) = 0;   // the field's text changed
    virtual void submit() = 0;                      // Enter: checked, then kept and used, or a problem
    virtual void change() = 0;                      // the person asks to change the server: the dialog opens
    virtual void cancel() = 0;                      // closed: with a server kept, the dialog closes; else it stays
};

// The kit's record of one tab strip's tabs (pr/kit_choice.h PrKitTabs), freed with the view.
struct KitTabsFree {
    void operator()(PrKitTabs* tabs) const { prKitTabsDestroy(tabs); }
};
using KitTabs = std::unique_ptr<PrKitTabs, KitTabsFree>;

// A friend's transcript as the view lays it out (view.cpp transcript). Only the items in sight, and a screen's height
// either side, are declared each round; spacers stand for the rest, so the scroll covers the whole transcript and a
// round costs the same with fifty entries or fifty thousand. An item is an entry, or a run of tool lines, which stand
// together. Each item's height is the one it took at its last layout, or until then an estimate from its text.
struct TranscriptItems {
    std::uint64_t generation = 0;
    std::size_t entries = 0;                    // the transcript's entries grouped into items so far
    std::vector<std::uint32_t> first;           // each item's first entry
    std::vector<std::uint32_t> lines;           // its lines (line feeds and one, summed over a run of tool lines)
    std::vector<std::uint32_t> chars;           // its bytes of text
    std::vector<std::uint32_t> count;           // its entries (more than one for a run of tool lines)
    std::vector<float> height;                  // px with the gap after it, as last laid out; 0 until it is
    std::vector<std::pair<std::uint32_t, PrLayoutNode>> declared;  // the items the last round declared, and nodes
    PrLayoutNode column = kPrLayoutNodeNone;    // the transcript's column, as the last round built it
    float width = 0;                            // the column's width at the last layout, px
    int steps = 0;                              // the text scale the heights were laid out at
};

// The account menu that drops from the active AI's tab, and the /account, /login and /logout dialogs (account_menu.h,
// docs/accounts.md "Switching accounts"). The menu's gesture is fed the pointer by routeAccountMenu before the tab strip
// sees it; the menu draws below the tab's highlight and changes nothing about the tab.
struct AccountMenuView {
    enum class Dialog : std::uint8_t { None, Account, Login, Logout };
    AccountMenuGesture gesture;
    std::size_t tab = ~std::size_t(0);          // the Tab::id the menu drops from, while it is open
    std::vector<AccountMenuRow> rows;           // the rows it opened with
    AccountMenuLayout layout;                   // as the last round laid it out, for the finger's lift
    MenuBox anchor;                             // the highlight it drops from, as the press found it
    float textInset = -1;                       // the tab label's left from the highlight's left; -1: unknown
    PrLayoutNode node = kPrLayoutNodeNone;      // the menu, as the last round built it
    std::vector<PrLayoutNode> rowNodes;         // its rows
    std::vector<PrLayoutNode> rowTexts;         // each row's text
    Dialog dialog = Dialog::None;               // the dialog that shows
    bool dialogShown = false;                   // a dialog showed last round
    std::size_t dialogTab = ~std::size_t(0);    // the Tab::id it was opened on
    std::vector<AccountMenuRow> dialogRows;     // its rows
    std::string logoutAccount, logoutShown, logoutNext;   // /logout: the account, its email, where the AI goes
};

struct View {
    std::function<void(const Chirp&)> playChirp;
    AccountMenuView accounts;
    // What the menu shows, decided from the server's state (frame_host.cpp). Unset, there is no menu and the commands
    // say why.
    // The active AI's menu rows (its signed-in accounts and the sign-in row); none while it has no accounts, the link is
    // down, or a switch is on its way.
    std::function<std::optional<std::vector<AccountMenuRow>>()> accountMenuRows;
    std::function<std::vector<AccountMenuRow>()> accountLoginRows;  // the active AI's accounts that need signing in
    std::function<void(const std::string&)> switchAccount;          // the active AI's session goes to this account
    std::function<void(const std::string&)> loginAccount;           // sign this account in; switch once it is
    // /logout: the active AI's current account, its email, and where the AI goes after (empty: the sign-in prompt).
    std::function<bool(std::string&, std::string&, std::string&)> logoutTarget;
    std::function<bool()> logout;                                   // signs out and moves the AI, as the dialog says
    // The node a press opens the menu from this round: the active AI's tab (its highlight), or on the iPhone the AI's
    // name at the top of its page. kPrLayoutNodeNone: none.
    PrLayoutNode menuAnchor = kPrLayoutNodeNone;
    PrLayoutNode menuAnchorText = kPrLayoutNodeNone;                // the anchor's label, for the rows' text to line up
    struct GlyphTrail {
        struct Range { size_t start, end; double birth; };
        size_t tab = SIZE_MAX, entry = SIZE_MAX;
        uint64_t generation = 0;
        size_t bytes = 0;
        std::vector<Range> ranges;
    };
    GlyphTrail replyTrail;
    struct SentFlash {
        std::size_t tab, first;
        uint64_t generation;
        std::string text;
        double since;
    };
    std::vector<SentFlash> sentFlashes;
    struct OutgoingBubble { size_t tab; uint64_t action; std::string text; };
    std::vector<OutgoingBubble> outgoing;
    std::map<std::pair<size_t, std::string>, std::string> attachmentLabels;
    bool dialogVisible = false, savingPower = false;
    double lastInput = -100;
    std::function<void(bool)> setQuiet;
    ChatMarkdownCache markdown;
    std::unique_ptr<PrTextFont, decltype(&prTextFontRelease)> inlineFont{nullptr, prTextFontRelease};
    std::unique_ptr<PrTextFont, decltype(&prTextFontRelease)> promptFont{nullptr, prTextFontRelease};
    ChatSelection selection;
    bool promptPointerDown = false;
    std::optional<uint32_t> marginAnchor;
    std::function<bool(const std::string&)> copy;
    std::function<void(const std::string&)> openLink;
    Model* model = nullptr;
    Look* look = nullptr;
    Terminals* terminals = nullptr;             // the terminal tabs' shells; unset, Command-T adds none
    Board* board = nullptr;                     // the sprint tab's page; unset, the tab says it has none
    PrVec2 extent{};                            // the window's extent in px, which the host sets before each frame
    std::shared_ptr<policy::Host> application = std::make_shared<policy::Host>("client");
    WindowApplication windowApp;
    NativeTabs nativeTabs;
    std::map<std::string, KitTabs> groupStrips;
    struct TabWidth { std::string title; float advance = 0; };
    PrTextShaper* windowShaper = nullptr; // borrowed from the window for measuring tab titles
    std::vector<TabWidth> tabWidths;
    std::vector<PrLayoutNode> tabNodes;
    PrLayoutNode accountAnchor = kPrLayoutNodeNone;
    PrLayoutNode accountMenu = kPrLayoutNodeNone;
    std::vector<PrLayoutNode> accountRows;
    std::string lastHighlightedAccount;
    uint64_t tabGroupGeneration = 0;
    uint64_t draggingTab = 0;
    std::function<std::string(const std::string& service, const std::string& text)> requestService;
    std::string extensionProblem;
    int askedTextSteps = 0;                     // the text scale the keys asked for, applied at the next frame's start
    // The share of the policy's prompt width the prompt box and the transcript's column take, applied at the next
    // frame's start: 1 on a desktop, style::kRegularWidthColumn on an iPad (main_ios.cpp).
    float columnScale = 1;
    // The AI's replies take the column's whole width rather than the policy's reply_width, applied at the next frame's
    // start: false on a desktop and an iPad, true on a compact width (an iPhone, main_ios.cpp), where the policy's share
    // wraps a reply at about half the screen.
    bool fullWidthReplies = false;
    // The dialogs typed into (the server, a secret) stand at the window's top rather than its centre: true on iOS and
    // iPadOS (main_ios.cpp), where the on-screen keyboard covers the window's lower part and a modal is placed over
    // the whole window.
    bool dialogsAtTop = false;
    std::string notice;                         // shown in place of the transcript when there are no tabs
    std::function<void()> hide;                 // Escape: hides the overlay (overlay.h escape); unset when
                                                // windowed, where Escape does nothing
    std::function<void()> stop;                 // Command-Escape on a friend's tab: stops the friend's work (with a
                                                // server, a stop action for the server to decide); unset, nothing
    // With a server: the friend's text still growing in the session a friend's tab (by Tab::id) shows, the open entry
    // (sync/mirror.h Mirror::open), shown after the transcript as it grows; null or empty for none. Unset: none.
    std::function<const std::string*(std::size_t id)> growing;
    // With a server: the state cursor of a friend's tab (by Tab::id), under its last text (cursor.h). Unset: none.
    std::function<CursorState(std::size_t id)> cursor;
    std::function<unsigned(std::size_t)> activeAgents;
    std::string choiceKind, choiceValue;
    std::size_t choiceTab = SIZE_MAX;
    unsigned scannerCount = 1;
    bool scannerBusy = false;
    double scannerChanged = 0, scannerStarted = 0;
    double time = 0;                            // the frame's time in seconds, for the busy cursor's breathe
    size_t scannerTab = SIZE_MAX, energyTab = SIZE_MAX;
    std::string energyAccount;
    double scannerTime = 0, tabEnergyTime = -100, accountEnergyTime = -100;
    float scannerSpread = 0;
    bool energyAnimating = false;
    unsigned pendingDashStep = 0;
    bool breathing = false;                     // the last round drew a busy cursor: frames go on while it shows
    std::optional<std::size_t> helpTab;
    std::string helpText;
    std::string problem;                        // why the last prompt was not sent, until one is: a dim line at the
                                                // end of the transcript, never beside the prompt
    // With a server (main.cpp --server): Enter and Command-Enter on a friend's tab call this with the tab's position
    // and whether it queues, in place of the model's send and queue; the server's state comes back into the model.
    std::function<void(std::size_t tab, bool queue)> submit;
    std::function<void(std::size_t tabId)> clearOpen;
    std::function<bool()> signOut; // true when the selected account accepted the explicit composer command
    std::function<LimitOffer()> limitOffer;
    std::function<void(const LimitOffer&, const std::string&)> resolveLimit;
    LimitOffer displayedLimit;
    std::optional<AccountPicker> accountPicker;
    std::optional<std::pair<std::size_t, std::string>> dismissedSignIn;
    std::function<std::optional<AccountPicker>(int)> previewAccounts;
    std::function<void(const std::string&)> accountAction;
    std::function<void(const std::string&)> pickerCommand;
    std::function<void(const std::string&)> chooseAccount;
    std::function<void(const std::string&)> accountCommand;
    std::function<std::string()> accountCaption;
    std::function<void(std::string&, std::string&, std::string&)> sessionCapabilities;
    std::function<SignInAccount()> signInAccount; // selected account while its sign-in is available
    std::function<bool()> activateSignIn;        // true when sign-in consumed Enter or a card click
    // Command-V in the prompt: reads the clipboard, whose text comes back as a Paste event through the host, which cuts
    // it to the room the prompt has left (frame_host.h); unset, the text area pastes on its own, uncut.
    std::function<void()> paste;
    // Text cut to fit the prompt (kPromptBytes): one dim line at the end of the transcript of the friend's tab it was
    // cut on (`cutTab`, by Tab::id), saying how much was left out (cutLine), until that draft changes. The transcript
    // goes to its end once to show it (`cutFollow`). While the cut text is on its way into the prompt (`cutComing`, a
    // Paste event for the next frame), the change it makes is not a change of the draft; once it is in, `cutDraft` is
    // the draft the line stands for.
    std::string cut;
    std::size_t cutTab = ~std::size_t(0);
    std::string cutDraft;
    bool cutComing = false;
    bool cutFollow = false;
    bool promptOpen = true;                     // false: no prompt on a friend's tab (before the server is reached)
    std::string status;                         // one plain line where the prompt stands (the link's state); empty,
                                                // none
    std::function<void(const Tab&)> dismissFriend; // local close; the server still owns the friend and session
    std::function<void()> restoreTabLayout;      // after the effective group policy is installed
    std::function<void()> moved;                // after a drag moved a tab: saves the order (tab_order.h)
    std::function<void()> selected;             // after a round that changed the selected tab (by Tab::id), the
                                                // first round included: saves the selection (tab_order.h)
    std::optional<std::size_t> pendingBoardId;
    std::size_t selectedId = ~std::size_t(0);   // the Tab::id selected at the end of the last round
    std::function<void()> scaled;               // after a frame's start applied a new text scale (look->textSteps),
                                                // the first one included: saves it (tab_order.h)
    Secrets* secrets = nullptr;                 // the secret dialog's requests and field; unset, none ever shows
    ServerChoice* serverChoice = nullptr;       // the server dialog (iOS); unset, it never shows
    bool serverShown = false;                   // the server dialog showed last round
    std::uint64_t secretShown = 0;              // the request the dialog showed last round (0: none)
    PrLayoutNode secretField = kPrLayoutNodeNone;   // its field, as the last round built it
    // Each friend's transcript scroll from the top in px, by Tab::id, kept while another shows; kAtEnd while it showed
    // its newest entry, where it follows what arrives.
    static constexpr float kAtEnd = -1;
    std::vector<float> scrolls;
    std::vector<TranscriptItems> items;         // each friend's transcript as laid out, by Tab::id
    std::size_t shown = ~std::size_t(0);        // the friend (Tab::id) the transcript showed last round
    std::size_t prompted = ~std::size_t(0);     // the friend (Tab::id) whose draft the prompt edited last round
    uint32_t promptRows = 1;                    // lines of text the prompt showed at the last layout
    PrLayoutNode prompt = kPrLayoutNodeNone;    // the prompt's text area, as the last round built it
    PrLayoutNode promptBox = kPrLayoutNodeNone; // the rounded box that holds the plus and the text area
    PrLayoutNode conversation = kPrLayoutNodeNone;
    PrLayoutNode transcript = kPrLayoutNodeNone; // the transcript's scroller, as the last round built it
    PrLayoutNode boardBox = kPrLayoutNodeNone;  // the box that holds the sprint tab's page, as the last round built it
    HitchDetector* hitch = nullptr;             // names each terminal tab's update and show as a phase; null, none
};

// The most a prompt holds, in bytes: the server's text limit (sync/hub.h Limits::text), past which the server refuses a
// prompt anyway. A text area relays out the whole of its text at each edit, so a prompt of a megabyte took 100 ms a
// keystroke; within this it stays well inside a frame.
constexpr std::size_t kPromptBytes = 32 * 1024;

// The line the transcript ends with after text of `given` bytes was cut to `kept` to fit the prompt, e.g. "1,015,808 of
// 1,048,576 bytes were left out: the prompt holds 32 KB."
std::string cutLine(std::size_t given, std::size_t kept);

// The bytes the prompt of the friend's tab that shows can still take: kPromptBytes less its draft, plus the bytes
// selected in it (`ui`'s selection of view.prompt), which what comes in replaces. Zero on any other tab.
std::size_t promptRoom(const View& view, PrUi* ui);

// Notes that text of `given` bytes was cut to `kept` on the friend's tab that shows (View::cut): `coming`, the kept text
// is still on its way into the prompt.
void promptCut(View& view, std::size_t given, std::size_t kept, bool coming);

// The longest prefix of `text` of at most `room` bytes that ends on a whole UTF-8 character.
PrStringView cutText(PrStringView text, std::size_t room);

// Called at the start of each frame, before its rounds: applies a text scale the keys asked for in the last frame to the
// look and the kit's theme (which changes only between rounds). Returns true if it changed the scale.
bool beginWindowFrame(View& view, PrKit* kit, PrFontSet* fonts);

// True if a text scale is asked for and not yet applied, a busy cursor breathes, or cut text is on its way into the
// prompt: another frame would show something different.
bool windowWantsFrame(const View& view);

// Declares the whole window for one round under `parent`, or as the root if `parent` is kPrLayoutNodeNone, and applies
// the round's input: tab clicks and drags (reordering), Command-1 to Command-0 (by position), the text scale's keys,
// Command-T (a terminal tab) and Command-W (closes a terminal tab), on a friend's or the sprint tab Escape (hide), and
// on a friend's tab Command-Escape (stop), the prompt's edits, and its Enter and Command-Enter. The sprint tab stands
// first in a strip of its own, which nothing is dragged into or out of, and shows its page (or the line that says why
// there is none) with no prompt. A terminal tab shows its terminal in place of the transcript and the prompt; terminals
// that do not show are serviced, and one whose shell ended closes.
bool routePromptPointer(View& view, PrUi* ui, const PrAppEvent& event);
// The account menu's gesture (account_menu.h), ahead of the tree: true when the event is the menu's. A press on the
// active AI's tab opens the menu; a sideways drag past the threshold becomes the tab's move, given to the tree as a press
// at the origin and the movement; a lift on a row chooses it.
bool routeAccountMenu(View& view, PrUi* ui, const PrAppEvent& event);
// Opens a dialog on the active AI's tab: Account (/account), Login (/login) or Logout (/logout). False, with
// view.problem saying why, when there is nothing to show.
bool openAccountDialog(View& view, AccountMenuView::Dialog dialog);
void buildWindow(View& view, PrKit* kit, PrLayoutNode parent);

} // namespace nexus
