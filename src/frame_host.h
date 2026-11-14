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
#include <memory>
#include <string>

#include "pr/app.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/ui.h"

#include "hitch.h"
#include "model.h"
#include "sync/actions.h"
#include "sync/client.h"
#include "sync/hub.h"
#include "sync/mirror.h"
#include "sync/sessions.h"
#include "tab_order.h"
#include "view.h"

namespace nexus {

class PraxTerminals;
class PraxBoard;

// The link to nexus-server as the window's frame sees it: the latest state, read without waiting, and actions handed
// over without waiting. link/client_port.h is the one over the link's thread (link/client.h).
struct LinkState {
    sync::LinkPhase phase = sync::LinkPhase::Connecting;
    std::string problem;                // with Failed: why
    std::uint64_t heardMsAgo = 0;       // with Connected: since the server was last heard
    sync::Sessions sessions;            // the server's latest state
};

struct LinkPort {
    virtual ~LinkPort() = default;
    // The latest state; true if it changed since the last call that returned true. Never waits.
    virtual bool poll(LinkState* out) = 0;
    // An action for the server, in order. Never waits.
    virtual void send(sync::Action action) = 0;
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

    // With a server (main.cpp --server, or a state file).
    LinkPort* link = nullptr;
    std::string host;
    sync::Mirror mirror;
    sync::LinkPhase phase = sync::LinkPhase::Connecting;
    std::uint64_t heardMsAgo = 0;

    // Shows the server's sessions through `port`: the prompt waits for the server, Enter and Command-Enter send and
    // queue to the session the tab shows, Command-Escape stops it, and a session is ensured with each friend.
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

private:
    void pollLink();
    void wire();
};

} // namespace nexus
