// Unit tests of the terminal tabs whose shells run on the server (src/remote_terminals.h, docs/terminals.md), through a
// fake link that records the actions sent: a new tab asks for a terminal named by the window's token and takes it when
// the state holds it; a terminal no tab holds gets a tab of its own, the selection staying; a closed tab's terminal is
// closed and never taken back; one another window closed leaves; one lost to a restart, or refused, says so; a server
// from before terminals offers none. No OS, no clock, no window, no process, no network: the screens (which need a
// window's kit) are the iOS shots'. And what the keys send, in the order typed, through a screen of Prax's own.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pr/vterm.h"

#include "frame_host.h"
#include "model.h"
#include "remote_terminals.h"
#include "sync/sessions.h"
#include "sync/terminal_state.h"

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

struct FakePort : LinkPort {
    std::vector<sync::Action> sent;
    bool poll(LinkState*) override { return false; }
    std::uint64_t send(sync::Action action) override {
        action.num = sent.size() + 1;
        sent.push_back(std::move(action));
        return sent.back().num;
    }
};

constexpr std::uint64_t kEpoch = 7;

sync::TerminalId idOf(std::uint64_t epoch, std::uint32_t n) {
    return (epoch << 32) | n;
}

sync::TerminalRef term(sync::TerminalId id, std::string opener, std::string title = {}, std::string ended = {}) {
    auto v = std::make_shared<sync::TerminalView>();
    v->id = id;
    v->opener = std::move(opener);
    v->title = std::move(title);
    v->columns = 80;
    v->rows = 24;
    v->ended = std::move(ended);
    v->lines.assign(24, std::make_shared<const std::string>());
    return v;
}

sync::Sessions state(std::vector<sync::TerminalRef> terminals, std::uint64_t next, std::uint64_t epoch = kEpoch,
                     bool served = true) {
    sync::Sessions s;
    s.setTerminals(served, epoch, std::move(terminals));
    s.setNextAction(next);
    return s;
}

std::size_t terminalTabs(const Model& m) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < m.tab_count(); ++i) n += m.tab(i).kind == TabKind::Terminal;
    return n;
}

void opensAndTakesItsTerminal() {
    FakePort port;
    Model model({});
    RemoteTerminals t(nullptr, nullptr, "tok");
    t.setLink(&port);
    CHECK(!t.available());                  // nothing heard yet
    CHECK(!t.open(1));
    CHECK(port.sent.empty());
    t.received(state({}, 1), true, model);
    CHECK(t.available());
    const std::size_t tab = model.add_terminal();
    CHECK(t.open(tab));
    CHECK(port.sent.size() == 1 && port.sent[0].kind == sync::ActionKind::TerminalCreate);
    CHECK(port.sent[0].text == "tok/" + std::to_string(tab));
    CHECK(t.problem(tab).empty() && t.title(tab) == "Terminal" && !t.ended(tab));
    // The state holds it: taken by its tab, not given another.
    t.received(state({term(idOf(kEpoch, 1), "tok/" + std::to_string(tab), "vim")}, 2), true, model);
    CHECK(terminalTabs(model) == 1);
    CHECK(t.title(tab) == "vim");
    CHECK(!t.ended(tab) && t.problem(tab).empty());
    // Command-W: its close goes to the server, and the tab's terminal is never taken back while the state still holds
    // it.
    t.close(tab);
    model.close_tab(model.active());
    CHECK(port.sent.size() == 2 && port.sent[1].kind == sync::ActionKind::TerminalClose);
    CHECK(port.sent[1].terminal == idOf(kEpoch, 1));
    t.received(state({term(idOf(kEpoch, 1), "tok/" + std::to_string(tab))}, 2), true, model);
    CHECK(terminalTabs(model) == 0);
    t.received(state({}, 3), true, model);
    CHECK(terminalTabs(model) == 0);
}

void adoptsWhatNoTabHolds() {
    FakePort port;
    Model model({});
    model.add_terminal();
    const std::size_t first = model.tab(model.active()).id;
    RemoteTerminals t(nullptr, nullptr, "tok");
    t.setLink(&port);
    t.received(state({}, 1), true, model);
    CHECK(t.open(first));
    // Another window's terminal, and this one's: the other gets a tab of its own at the end; the selection stays.
    t.received(state({term(idOf(kEpoch, 1), "tok/" + std::to_string(first)), term(idOf(kEpoch, 2), "ipad/3", "top")},
        2), true, model);
    CHECK(terminalTabs(model) == 2);
    CHECK(model.tab(model.active()).id == first);
    const std::size_t adopted = model.tab(model.tab_count() - 1).id;
    CHECK(adopted != first);
    CHECK(t.title(adopted) == "top");
    CHECK(port.sent.size() == 1);           // taking a tab sends nothing
    // The same state again: no second tab.
    t.received(state({term(idOf(kEpoch, 1), "tok/" + std::to_string(first)), term(idOf(kEpoch, 2), "ipad/3", "top")},
        2), true, model);
    CHECK(terminalTabs(model) == 2);
    // Another window closed it: its tab here goes (ended), and the first stays.
    t.received(state({term(idOf(kEpoch, 1), "tok/" + std::to_string(first))}, 2), true, model);
    CHECK(t.ended(adopted));
    CHECK(!t.ended(first));
    // A state while the server is not heard changes nothing.
    t.received(state({}, 2), false, model);
    CHECK(!t.ended(first));
}

void endedStaysAndRestartSays() {
    FakePort port;
    Model model({});
    RemoteTerminals t(nullptr, nullptr, "tok");
    t.setLink(&port);
    t.received(state({term(idOf(kEpoch, 4), "mac/1")}, 1), true, model);
    CHECK(terminalTabs(model) == 1);
    const std::size_t tab = model.tab(model.tab_count() - 1).id;
    // Its shell ended: the tab stays, with no problem line (the screen says how), and is not ended.
    t.received(state({term(idOf(kEpoch, 4), "mac/1", "", "exited with code 0")}, 1), true, model);
    CHECK(!t.ended(tab) && t.problem(tab).empty());
    // The server restarted: the terminal is lost, and the tab says so until it is closed; closing sends nothing.
    t.received(state({}, 1, kEpoch + 1), true, model);
    CHECK(!t.ended(tab));
    CHECK(t.problem(tab) == sync::kTerminalRestarted);
    t.close(tab);
    CHECK(port.sent.empty());
}

void refusedAndOrphaned() {
    FakePort port;
    Model model({});
    RemoteTerminals t(nullptr, nullptr, "tok");
    t.setLink(&port);
    t.received(state({}, 1), true, model);
    const std::size_t a = model.add_terminal();
    CHECK(t.open(a));                       // action 1
    // The server applied it and made nothing: the tab says so.
    t.received(state({}, 2), true, model);
    CHECK(t.problem(a).find("did not start") != std::string::npos);
    // A tab closed before its terminal came: the terminal is closed when it comes, and gets no tab.
    const std::size_t b = model.add_terminal();
    CHECK(t.open(b));                       // action 2
    t.close(b);
    t.received(state({term(idOf(kEpoch, 9), "tok/" + std::to_string(b))}, 3), true, model);
    CHECK(port.sent.size() == 3 && port.sent[2].kind == sync::ActionKind::TerminalClose);
    CHECK(port.sent[2].terminal == idOf(kEpoch, 9));
    CHECK(terminalTabs(model) == 2);        // a and b's own, which the view would have closed: none added
}

void olderServerOffersNone() {
    FakePort port;
    Model model({});
    RemoteTerminals t(nullptr, nullptr, "tok");
    t.setLink(&port);
    t.received(state({}, 1, 0, false), true, model);
    CHECK(!t.available());
    const std::size_t tab = model.add_terminal();
    CHECK(!t.open(tab));
    CHECK(port.sent.empty());
    // At the most the server runs: none offered either.
    std::vector<sync::TerminalRef> full;
    for (std::uint32_t i = 1; i <= sync::kMaxTerminals; ++i) full.push_back(term(idOf(kEpoch, i), "x/" + std::to_string(i)));
    t.received(state(full, 1), true, model);
    CHECK(!t.available());
}

// A key typed at time t, as the iOS hardware keyboard reports it: KeyDown with the key's text.
PrAppEvent keyDown(PrAppKey key, const char* text, double t, PrAppModifierFlags modifiers = 0) {
    PrAppEvent e{};
    e.kind = PrAppEventKind_KeyDown;
    e.time = t;
    e.modifiers = modifiers;
    e.keyboard.key = key;
    e.keyboard.text = PrStringView{text, std::strlen(text)};
    return e;
}

PrAppEvent textEvent(const char* text, double t) {
    PrAppEvent e{};
    e.kind = PrAppEventKind_Text;
    e.time = t;
    e.input.text = PrStringView{text, std::strlen(text)};
    return e;
}

struct Screen {
    PrVterm* vterm;
    Screen() {
        PrVtermDesc d{};
        d.extent = PrUVec2{80, 24};
        vterm = prVtermCreate(&d);
    }
    ~Screen() { prVtermDestroy(vterm); }
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
};

constexpr PrVec2 kCell{8, 16};

// iOS's text system delivers a hardware key's text after its KeyDown, here after the Enter typed next: the shell still
// gets the letters before the Enter, and each once.
void keysKeepTheirOrder() {
    Screen screen;
    TerminalKeys keys;
    std::string sent;
    CHECK(keys.type(screen.vterm, kCell, keyDown('L', "l", 1.0), &sent));
    CHECK(keys.type(screen.vterm, kCell, keyDown('S', "s", 1.01), &sent));
    CHECK(keys.type(screen.vterm, kCell, keyDown(PrAppKey_Enter, "", 1.02), &sent));
    CHECK(sent == "ls\r");
    CHECK(keys.type(screen.vterm, kCell, textEvent("l", 1.05), &sent));
    CHECK(keys.type(screen.vterm, kCell, textEvent("s", 1.06), &sent));
    CHECK(sent == "ls\r");
    // The on-screen keyboard's text comes with no KeyDown: typed as it comes.
    CHECK(keys.type(screen.vterm, kCell, textEvent("x", 2.0), &sent));
    CHECK(sent == "ls\rx");
    // A key whose text never came is forgotten after a second: the same letter typed later on screen is typed.
    CHECK(keys.type(screen.vterm, kCell, keyDown('Q', "q", 3.0), &sent));
    CHECK(keys.type(screen.vterm, kCell, textEvent("q", 4.5), &sent));
    CHECK(sent == "ls\rxqq");
    // Text that is not the waiting key's (the text system changed it): typed, and nothing left waiting.
    CHECK(keys.type(screen.vterm, kCell, keyDown('A', "a", 5.0), &sent));
    CHECK(keys.type(screen.vterm, kCell, textEvent("b", 5.1), &sent));
    CHECK(keys.type(screen.vterm, kCell, textEvent("a", 5.2), &sent));
    CHECK(sent == "ls\rxqqaba");
}

// Control+C from a keyboard, and the key row's Control on the next letter, send ETX; Control is held for one key.
void controlSendsItsCharacter() {
    Screen screen;
    TerminalKeys keys;
    std::string sent;
    CHECK(keys.type(screen.vterm, kCell, keyDown('C', "c", 1.0, PrAppModifierFlag_Control), &sent));
    CHECK(keys.type(screen.vterm, kCell, textEvent("c", 1.01), &sent));   // its own text, dropped
    CHECK(sent == "\x03");
    keys.toggleControl();
    CHECK(keys.controlHeld());
    CHECK(keys.type(screen.vterm, kCell, textEvent("d", 2.0), &sent));
    CHECK(sent == "\x03\x04");
    CHECK(!keys.controlHeld());
    keys.key(screen.vterm, kCell, PrAppKey_Left, 0, &sent);
    CHECK(sent == "\x03\x04\x1b[D");
}

} // namespace

int main() {
    opensAndTakesItsTerminal();
    adoptsWhatNoTabHolds();
    endedStaysAndRestartSays();
    refusedAndOrphaned();
    olderServerOffersNone();
    keysKeepTheirOrder();
    controlSendsItsCharacter();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("remote terminals: ok\n");
    return 0;
}
