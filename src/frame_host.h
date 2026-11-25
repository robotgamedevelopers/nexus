// What the window does around the view each frame, apart from the overlay: shared by the app (main.cpp, as a Prax
// Studio window's callbacks) and the frame-budget test (tests/window/test_frames.cpp), so that the test drives the code
// the app runs.
//
// At a frame's start: the terminals' labels from what their poller last read, the window's kept preferences handed to
// their writer when they changed, the server's latest state into the model, a new text scale, and the sprint tab's
// page. Then the rounds build the view (view.h). Events go to the sprint tab's page when it shows. Each of these is a
// phase of the frame for the hitch detector (hitch.h).
//
// Nothing here waits: the link has a thread of its own (link/client.h), the terminals start, stop and are read on
// threads of theirs (terminals.h), the board's address is found on one, and the preferences are written by a writer
// thread (tab_order.h asyncStore). The client runs at 120 Hz with an 8.3 ms budget (AGENTS.md).

#pragma once
#include <future>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pr/app.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/ui.h"

#include "hitch.h"
#include "link_port.h"
#include "model.h"
#include "sync/actions.h"
#include "sync/client.h"
#include "sync/ai_strip.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#include "signin_gate.h"
#include "sync/sessions.h"
#include "sync/signin.h"
#include "tab_order.h"
#include "view.h"

namespace nexus {

class PraxTerminals;
class PraxBoard;

// Text from outside (a name the server sent that is not an AI's, the host given), as a log line can hold it: every
// byte below 0x20, 0x7f and above, and the backslash as \xNN, the rest as it is; at most 80 bytes, cut between two
// characters, never inside an escape, then "...".
std::string loggable(std::string_view text);

// The secret dialog over the link (view.h Secrets, docs/secrets.md): the requests come in the server's state, and the
// dialog shows the oldest the window has not answered (sync/secrets.h SecretPrompt). The value is held in the prompt
// alone while it is typed, then in the one action that carries it to the server, which the link's thread hands to mosh
// and which is wiped once the server has acknowledged it. Nothing here waits: submit and cancel hand the action to the
// port.
class LinkSecrets : public Secrets {
public:
    LinkPort* port = nullptr;
    sync::SecretPrompt prompt;

    const SecretAsk* showing() override;
    std::string_view typed() override { return prompt.typed().bytes(); }
    void type(std::string_view text) override { prompt.type(text); }
    void submit() override;
    void cancel() override;

private:
    SecretAsk shown_;
};

// In-app sign-in over the link (docs/secrets.md "Sign-in"): the server offers each account that needs signing in, and a
// window that is shown claims one (sync/signin.h SignInOpener); the server then hands that window alone the process's
// URL, which it opens in the browser (`openUrl`, main.cpp: Prax's prAppOpenUrl). When the server runs on another
// machine, the window first opens its gate (signin_gate.h) on the redirect's port, and sends up the one callback it
// takes. The URL and the callback's query pass only through here, the gate and the actions that carry them: nothing is
// logged, drawn or kept. One step a frame; nothing waits.
class LinkSignIn {
public:
    LinkPort* port = nullptr;
    sync::SignInOpener opener;          // its claim token, random per launch (connect)
    std::string serverArg;              // where the window reaches the server (--server)
    std::string local;                  // this machine's host name, for sync::sameMachine
    bool loopback = false;              // established by the transport, independent of names in server offers
    std::optional<std::string> activeAccount; // roster-aware windows claim only on explicit input
    std::function<void(const std::string& url)> openUrl;    // empty: nothing is opened (the frame test)
    // Where the gate comes from: a test seam; empty: a SignInGate.
    std::function<std::unique_ptr<SignInGate>(const sync::SignInTarget&)> openGate;
    std::string problem;                // the last gate's failure, for the view's problem line; cleared when shown

    // The state's offers as the link last brought them, or the link lost.
    void received(const sync::Sessions& state) { opener.received(state.offers()); }
    void lost();
    // The overlay was shown: the window may claim one flow.
    void shown() { opener.shown(); }
    // The client lock locked the window (client_lock.h): the claim a show allowed is taken back, and no step runs (no
    // claim, no URL opened, no callback sent up) until it is unlocked; the owner's next show allows one again.
    void lock() { locked = true; opener.hidden(); }
    void unlock() { locked = false; }
    bool locked = false;
    // One step: the gate's callback up, a gate gone with its flow closed, then the opener's next claim or open. None
    // while locked.
    void step();
    bool gateOpen() const { return gate_ != nullptr; }

private:
    std::unique_ptr<SignInGate> gate_;
    std::uint64_t gateFlow_ = 0;
};

// The client lock's input gate (FrameHost::event, View::lockScreen). isInputEvent: the person's input (pointer, scroll,
// keys, text, paste, composition, drag and drop), as against the window's and the app's own events. lockScreenTakes:
// the input a locked window lets through to its lock screen: the pointer, and Return and Escape with no modifier.
// resetsIdle: the person's deliberate input, which starts the lock's idle time again (a key, text, a press, a scroll, a
// paste, a drop); a pointer passing over the window, a drag passing over it, a gesture or a touch on the trackpad does
// not, so that nothing but the person's act keeps the window unlocked.
bool isInputEvent(const PrAppEvent& e);
bool lockScreenTakes(const PrAppEvent& e);
bool resetsIdle(const PrAppEvent& e);

inline constexpr size_t kMaxAttachmentBytes = 32 * 1024 * 1024;
struct StagedFile {
    size_t tab;
    unsigned frame;
    std::string marker, name;
    std::shared_ptr<const std::string> bytes;
};

struct FrameHost {
    void finishUpload(); // also runs on wake when the overlay is hidden
    bool stageFile(std::string name, std::string bytes);
    std::function<void()> uploadWake;
    size_t pastePreviewBytes = 4096;

    Model* model = nullptr;
    View* view = nullptr;
    PraxTerminals* terminals = nullptr;     // while the window is open
    PraxBoard* board = nullptr;             // while the window is open
    HitchDetector* hitch = nullptr;         // the window's; null measures nothing

    // Kept across launches (tab_order.h): with `keep`, the terminal tabs and the selected tab are saved when they
    // change. The stores' writes never wait (tab_order.h asyncStore).
    bool keep = false;
    const TabOrderStore* terminalStore = nullptr;
    const TabOrderStore* selectionStore = nullptr;
    std::string savedTerminals;         // the text last handed to each store, so that only a change is written
    std::string savedSelection;
    const TabOrderStore* layoutStore = nullptr;
    TabLayout layout;
    bool groupPolicyReady = false;
    bool rosterKnown = false;
    void restoreLayout();
    std::uint64_t savedLayoutRevision = ~std::uint64_t{};
    std::size_t savedLayoutFocus = ~std::size_t{};

    // The AI tabs are the server's (AGENTS.md "AI tabs"): each state the link brings while connected whose
    // AIs changed sets them (sync/ai_strip.h), new ones in the saved order, and a session is ensured with each
    // AI as its tab is added. A saved AI's tab is selected when it arrives, unless the person chose a tab first
    // (personSelected); the selection is not saved while it waits, nor the AIs' order before the AIs arrive.
    // A removed AI's draft is kept by name, and given back when an AI of that name has a tab again; each such
    // change is made in `keptStore` for this window's server, `host` (with `keep`, and unless its file did not read or
    // the host cannot key a draft: tab_order.h KeptChange, keptServer).
    sync::AIStrip strip;
    const KeptDraftsStore* keptStore = nullptr;
    // A tab chosen by the person (view.h selected): the saved selection no longer waits for the AIs.
    void personSelected();

    // With a server (main.cpp --server, or a state file).
    LinkPort* link = nullptr;
    std::string host;
    sync::Mirror mirror;
    sync::LinkPhase phase = sync::LinkPhase::Connecting;
    std::uint64_t heardMsAgo = 0;
    LinkSecrets secrets;                // the secret dialog, from connect()
    LinkSignIn signIn;                  // in-app sign-in, from connect(); main.cpp sets its openUrl

    // Shows the server's sessions through `port`: the prompt waits for the server, Enter and Command-Enter send and
    // queue to the session the tab shows, Command-Escape stops it, the AI tabs follow the server's AIs, a
    // session is ensured with each, and the server's secret requests are asked in the secret dialog.
    void connect(LinkPort* port, const std::string& serverHost);
    // Shows `state` as the window holds it once the server is heard, or with `lost` once it has lost the server.
    void showState(const sync::Sessions& state, bool lost);

    // The frame's start, before its rounds.
    void beginFrame(PrKit* kit, PrFontSet* fonts, double time, const PrAppDisplay& display);
    // An event before the window's tree sees it: true if the host took it. The sprint tab's page takes the events it is
    // routed. Text typed or pasted into the prompt past its room (view.h promptRoom: kPromptBytes less the draft, plus
    // the selection it replaces) is cut to fit and pushed to `ui` in one event in its place, the transcript saying how
    // much was left out (View::cut).
    bool event(const PrAppEvent& e);
    PrUi* ui = nullptr;                     // the window's tree's context, while the window is open; null cuts nothing
    // One round of the window under `root`.
    void build(PrKit* kit, PrLayoutNode root);
    // One round of the window under `root`, inside a box of its own, `box` px, in every frame (iOS, main_ios.cpp): the
    // viewport less its safe area, and only the room above the on-screen keyboard while one covers its foot
    // (keyboard_room.h contentBox). view->extent is that box. The box is there with the keyboard and without it, so the
    // window's nodes stay the same nodes as it comes and goes: built straight into the root while no keyboard showed,
    // they were new each time it rose or fell, the prompt took the focus anew, and the keyboard went down and up
    // every frame (tests/window/test_keyboard_frames.cpp).
    void buildAbove(PrKit* kit, PrLayoutNode root, PrVec2 box);

    // Saves the terminal tabs and the selected tab if they changed (and the window keeps them).
    void keepTerminals();
    void keepSelection();

    // The link's latest state into the window, as each frame's start takes it: the AI tabs, the transcripts, the
    // secret requests and the status line. Never waits. main.cpp calls it before the first frame too, while a --tab
    // waits for the server's AIs.
    void pollLink();

private:
    // Actions the link lost before the server acknowledged them (LinkState::cut): a message among them goes back to its
    // tab as a pending message, and the problem line says what happened. Never silent.
    void showCut(const std::vector<std::uint64_t>& cut);
    // A press, followed until it is a tap (released within kTapSlop of where it went down) or not (View::tapEndsEditing).
    void watchTap(const PrAppEvent& e);
    bool tapping_ = false;
    PrVec2 tapDown_{};
    std::size_t tapTab_ = ~std::size_t(0);      // the active tab's id when the prompt was dismissed
    std::vector<StagedFile> stagedFiles_;
    unsigned stagedCount_ = 0, pasteFrame_ = 0;
    std::future<void> uploadWorker_;
    struct UploadResult {
        sync::Action action;
        size_t tab;
        std::string original, error;
    };
    std::future<UploadResult> upload_;
    sync::Sessions accountState_;
    std::map<sync::SessionId, std::uint64_t> accountSelections_; // wait for the exact server result
    struct LimitSelection { uint64_t request, action; };
    std::map<sync::SessionId, LimitSelection> limitSelections_;
    LimitOffer currentLimit() const;
    std::optional<std::pair<sync::SessionId, std::string>> signInSelection_;
    std::map<std::string, std::uint64_t> pendingClaims_; // until the offer changes or this link ends
    // /login: the session and the account signing in while the session stays on its own; it switches once signed in.
    std::optional<std::pair<sync::SessionId, std::string>> pendingLogin_;
    std::string logoutNext_;    // /logout: the account the AI moves to after, chosen as the dialog opened
    const sync::ServerFriend* rosterOf(sync::SessionId session) const;
    std::optional<std::string> signInTarget() const;
    void startLogin(const std::string& account);
    void settleLogin();
    void wireAccountMenu();
    const sync::AccountChoice* activeChoice() const;
    const sync::SignInOffer* offerFor(const std::string& account) const;
    void selectSignInAccount();
    void refreshPicker();
    std::optional<AccountPicker> previewAccounts(int direction) const;
    void pickerCommand(const std::string& command);
    void chooseAccount(std::string account);
    void setAccount(sync::SessionId session, const std::string& target);
    bool activateSignIn();
    const sync::ServerAI* activeAccountRoster() const;
    void wire();
    // The AI tabs from `state` (AIStrip::take): when its AIs changed, the mirror's record of each removed
    // tab forgotten, a session ensured for each added one when there is a link, each name the server sent that is not
    // an AI's logged, the kept drafts saved when they changed, and the notice for a strip without AIs.
    void takeAIs(const sync::Sessions& state);
};

} // namespace nexus
