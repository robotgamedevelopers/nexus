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
#include "model.h"
#include "sync/actions.h"
#include "sync/client.h"
#include "sync/friend_strip.h"
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

// Text from outside (a name the server sent that is not a friend's, the host given), as a log line can hold it: every
// byte below 0x20, 0x7f and above, and the backslash as \xNN, the rest as it is; at most 80 bytes, cut between two
// characters, never inside an escape, then "...".
std::string loggable(std::string_view text);

// The link to nexus-server as the window's frame sees it: the latest state, read without waiting, and actions handed
// over without waiting. link/client_port.h is the one over the link's thread (link/client.h).
struct LinkState {
    sync::LinkPhase phase = sync::LinkPhase::Connecting;
    std::string problem;                // with Failed: why
    std::uint64_t heardMsAgo = 0;       // with Connected: since the server was last heard
    sync::Sessions sessions;            // the server's latest state
    bool loopback = false;              // link connected to a numeric loopback address
};

struct LinkPort {
    virtual ~LinkPort() = default;
    // The latest state; true if it changed since the last call that returned true. Never waits.
    virtual bool poll(LinkState* out) = 0;
    // An action for the server, in order. Never waits.
    virtual void send(sync::Action action) = 0;
};

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
    // One step: the gate's callback up, a gate gone with its flow closed, then the opener's next claim or open.
    void step();
    bool gateOpen() const { return gate_ != nullptr; }

private:
    std::unique_ptr<SignInGate> gate_;
    std::uint64_t gateFlow_ = 0;
};

struct FrameHost {
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

    // The friend tabs are the server's (AGENTS.md "Friend tabs"): each state the link brings while connected whose
    // friends changed sets them (sync/friend_strip.h), new ones in the saved order, and a session is ensured with each
    // friend as its tab is added. A saved friend's tab is selected when it arrives, unless the person chose a tab first
    // (personSelected); the selection is not saved while it waits, nor the friends' order before the friends arrive.
    // A removed friend's draft is kept by name, and given back when a friend of that name has a tab again; each such
    // change is made in `keptStore` for this window's server, `host` (with `keep`, and unless its file did not read or
    // the host cannot key a draft: tab_order.h KeptChange, keptServer).
    sync::FriendStrip strip;
    const KeptDraftsStore* keptStore = nullptr;
    // A tab chosen by the person (view.h selected): the saved selection no longer waits for the friends.
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
    // queue to the session the tab shows, Command-Escape stops it, the friend tabs follow the server's friends, a
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

    // Saves the terminal tabs and the selected tab if they changed (and the window keeps them).
    void keepTerminals();
    void keepSelection();

    // The link's latest state into the window, as each frame's start takes it: the friend tabs, the transcripts, the
    // secret requests and the status line. Never waits. main.cpp calls it before the first frame too, while a --tab
    // waits for the server's friends.
    void pollLink();

private:
    sync::Sessions accountState_;
    std::map<std::string, std::uint64_t> pendingClaims_; // until the offer changes or this link ends
    const sync::AccountChoice* activeChoice() const;
    const sync::SignInOffer* offerFor(const std::string& account) const;
    void selectSignInAccount();
    bool activateSignIn();
    const sync::ServerFriend* activeAccountRoster() const;
    void wire();
    // The friend tabs from `state` (FriendStrip::take): when its friends changed, the mirror's record of each removed
    // tab forgotten, a session ensured for each added one when there is a link, each name the server sent that is not
    // a friend's logged, the kept drafts saved when they changed, and the notice for a strip without friends.
    void takeFriends(const sync::Sessions& state);
};

} // namespace nexus
