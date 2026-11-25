#include "remote_terminals.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

#include "pr/hash.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/terminal.h"
#include "pr/vterm.h"
#include "pr/vterm_input.h"

#include "frame_host.h"
#include "style.h"
#include "sync/secrets.h"
#include "sync/terminal_paint.h"

namespace nexus {

namespace {

// The monospaced family, as the desktop's terminals set theirs (terminals.cpp kMono).
const PrFontFamily kMono{.name = {}, .generic = PrFontGeneric_Monospace};

PrStringView sv(const std::string& s) {
    return {s.data(), s.size()};
}

// The view's own screen copy, under its lock: what the program would be sent, which is the server's to send, is
// dropped (the copy runs no program; its replies to the paint's sequences are not wanted either).
struct Locked {
    explicit Locked(PrTerminal* t) : terminal(t), vterm(prTerminalVtermLock(t)) {}
    ~Locked() { prTerminalVtermUnlock(terminal); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
    std::string take() {
        const PrByteView out = prVtermOutput(vterm);
        std::string bytes(reinterpret_cast<const char*>(out.data), out.size);
        prVtermOutputConsume(vterm, out.size);
        return bytes;
    }
    void drop() { prVtermOutputConsume(vterm, prVtermOutput(vterm).size); }
    PrTerminal* terminal;
    PrVterm* vterm;
};

} // namespace

RemoteTerminals::RemoteTerminals(PrApp* app, PrFontSet* fonts, std::string token)
    : app_(app), fonts_(fonts), token_(std::move(token)) {}

RemoteTerminals::~RemoteTerminals() {
    for (auto& [id, t] : tabs_) {
        prTerminalDestroy(t.view);
    }
}

PrTextFontDesc RemoteTerminals::font() const {
    PrFontStyle s = prFontStyleDefault();
    s.size = style::kTerminalSize * scale_;
    return PrTextFontDesc{.set = fonts_, .families = &kMono, .familyCount = 1, .style = s, .locale = {}};
}

RemoteTerminals::Tab* RemoteTerminals::find(std::size_t id) {
    const auto it = tabs_.find(id);
    return it == tabs_.end() ? nullptr : &it->second;
}

void RemoteTerminals::send(sync::Action action) {
    if (link_ != nullptr) {
        link_->send(std::move(action));
    }
}

bool RemoteTerminals::available() const {
    return link_ != nullptr && served_ && connected_ && count_ < sync::kMaxTerminals;
}

bool RemoteTerminals::open(std::size_t id) {
    if (!available()) {
        std::fprintf(stderr, "nexus: tab %zu: no terminal: %s\n", id,
            link_ == nullptr ? "this window has no server" : !connected_ ? "the server is not reached" :
            !served_ ? "this server runs no terminals (it is older than they are)" :
            "the server runs as many terminals as it can; close one first");
        return false;
    }
    Tab t;
    t.opener = token_ + "/" + std::to_string(id);
    sync::Action a;
    a.kind = sync::ActionKind::TerminalCreate;
    a.text = t.opener;
    // The home directory, as a new terminal tab on the Mac starts where the window's last one was or else there: a
    // phone has no directory of the server's to offer.
    if (link_ != nullptr) {
        t.createAction = link_->send(std::move(a));
    }
    tabs_.emplace(id, std::move(t));
    return true;
}

void RemoteTerminals::received(const sync::Sessions& state, bool connected, Model& model) {
    connected_ = connected;
    if (!connected) {
        return;     // the tabs keep what they show until the server is heard again
    }
    served_ = state.terminalsServed();
    epoch_ = state.terminalEpoch();
    const auto& terminals = state.terminals();
    count_ = terminals.size();
    // Terminals this window closed, or whose tab it closed while it waited, until the state no longer holds them.
    std::set<sync::TerminalId> held;
    for (const auto& [id, t] : tabs_) {
        if (t.terminal != 0) held.insert(t.terminal);
    }
    for (auto& [id, t] : tabs_) {
        if (t.terminal == 0 && !t.opener.empty()) {
            for (const sync::TerminalRef& v : terminals) {
                if (v->opener == t.opener && held.count(v->id) == 0) {
                    t.terminal = v->id;
                    t.epoch = epoch_;
                    held.insert(v->id);
                    break;
                }
            }
            // Its create was applied (the state's next action is past it) and made no terminal: the server refused it.
            if (t.terminal == 0 && t.createAction != 0 && state.nextAction() > t.createAction && t.refused.empty()) {
                t.refused = count_ >= sync::kMaxTerminals ?
                    "The server runs as many terminals as it can (" + std::to_string(sync::kMaxTerminals) +
                        "). Close this tab and one of the others, then open a terminal again." :
                    "The server did not start this terminal; its log says why. Close this tab and open another.";
            }
        }
    }
    std::erase_if(closing_, [&](sync::TerminalId id) { return state.findTerminal(id) == nullptr; });
    for (const sync::TerminalRef& v : terminals) {
        if (held.count(v->id) != 0 || closing_.count(v->id) != 0) continue;
        if (orphans_.erase(v->opener) != 0) {
            // A terminal this window asked for and whose tab it closed before the terminal came: closed in turn.
            sync::Action a;
            a.kind = sync::ActionKind::TerminalClose;
            a.terminal = v->id;
            send(std::move(a));
            closing_.insert(v->id);
            continue;
        }
        // One no tab holds: a tab of its own at the end of the terminals, the selection staying where it was.
        const std::size_t before = model.tab_count() != 0 ? model.tab(model.active()).id : ~std::size_t(0);
        const std::size_t id = model.add_terminal();
        if (before != ~std::size_t(0)) {
            for (std::size_t i = 0; i < model.tab_count(); ++i) {
                if (model.tab(i).id == before) {
                    model.select_tab(i);
                    break;
                }
            }
        }
        Tab t;
        t.terminal = v->id;
        t.epoch = epoch_;
        tabs_.emplace(id, std::move(t));
        held.insert(v->id);
    }
    for (auto& [id, t] : tabs_) {
        if (t.terminal == 0) continue;
        t.fate = sync::terminalFate(t.terminal, t.epoch, epoch_, terminals);
        if (const sync::TerminalView* v = state.findTerminal(t.terminal)) {
            for (const sync::TerminalRef& r : terminals) {
                if (r.get() == v) t.now = r;
            }
        }
        t.closedElsewhere = t.fate == sync::TerminalFate::Closed;
    }
}

void RemoteTerminals::paint(Tab& t) {
    if (t.view == nullptr || !t.now || !t.launched) return;
    Locked screen(t.view);
    const PrUVec2 extent = prVtermExtent(screen.vterm);
    const bool whole = !t.held || extent.x != t.columns || extent.y != t.rows;
    if (whole || t.held != t.now) {
        const std::string bytes = sync::paintTerminal(whole ? nullptr : t.held.get(), *t.now, extent.x, extent.y);
        prVtermWrite(screen.vterm, PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
        t.held = t.now;
        t.columns = extent.x;
        t.rows = extent.y;
    }
    screen.drop();
}

void RemoteTerminals::show(std::size_t id, PrLayoutNode parent) {
    Tab* t = find(id);
    if (t == nullptr || kit_ == nullptr) return;
    if (t->view == nullptr) {
        PrTerminalDesc desc{};
        desc.app = app_;
        desc.font = font();
        desc.launcher = nullptr;    // no program: the server's shell is the program, and its screen is painted here
        desc.scrollback = 0;
        t->view = prTerminalCreate(&desc);
        if (t->view == nullptr) return;
        scales_[id] = scale_;
    }
    if (scales_[id] != scale_) {
        scales_[id] = scale_;
        const PrTextFontDesc f = font();
        prTerminalSetFont(t->view, &f);
    }
    if (!prTerminalHasFocus(t->view)) prTerminalFocus(t->view);
    paint(*t);
    // The screen above the key row, which the phone's keyboard lacks: Escape, Tab, Control and the arrows.
    PrLayoutNode column = prKitBox(kit_, parent, {.key = prHashString(sv("remote-terminal"), id)},
        {.direction = PrKitDirection_Column, .place = {.flex = PrKitFlex_Fill}});
    PrLayoutNode screenBox = prKitBox(kit_, column, {.key = prHashString(sv("remote-terminal-screen"), id)},
        {.place = {.flex = PrKitFlex_Fill}});
    prTerminalFrame(t->view, kit_, screenBox, PrTerminalBackground_Parent);
    if (!t->launched && prTerminalLaunched(t->view)) {
        // Its first frame wrote that no program starts: the whole screen is painted over it next.
        t->launched = true;
        t->held.reset();
        paint(*t);
    }
    PrLayoutNode row = prKitBox(kit_, column, {.key = prHashString(sv("remote-terminal-keys"), id)},
        {.direction = PrKitDirection_Row, .gap = 6, .padding = {6, 6, 6, 6}});
    struct KeyCap { const char* label; PrAppKey key; };
    static constexpr KeyCap kKeys[] = {{"esc", PrAppKey_Escape}, {"tab", PrAppKey_Tab}, {"ctrl", PrAppKey(0)},
        {"\xe2\x86\x90", PrAppKey_Left}, {"\xe2\x86\x93", PrAppKey_Down}, {"\xe2\x86\x91", PrAppKey_Up},
        {"\xe2\x86\x92", PrAppKey_Right}};
    for (const KeyCap& k : kKeys) {
        const std::string label = k.label;
        const bool control = k.key == PrAppKey(0);
        const PrLayoutNode cap = prKitButton(kit_, row,
            {.key = prHashString(sv("remote-terminal-key-" + label), id), .text = sv(label),
             .selected = control && control_, .disabled = t->fate != sync::TerminalFate::Running || t->terminal == 0},
            {.size = PrKitControlSize_Lg});
        if (prKitActivated(kit_, cap)) {
            if (control) toggleControl();
            else key(id, k.key, 0);
        }
    }
    // The extent the screen took goes to the server when it changed: the shell's pseudoterminal follows the tab.
    if (t->terminal != 0 && t->fate == sync::TerminalFate::Running && t->launched) {
        PrUVec2 extent{};
        {
            Locked screen(t->view);
            extent = prVtermExtent(screen.vterm);
        }
        if (extent.x != 0 && extent.y != 0 && (extent.x != t->sentColumns || extent.y != t->sentRows)) {
            sync::Action a;
            a.kind = sync::ActionKind::TerminalResize;
            a.terminal = t->terminal;
            a.columns = extent.x;
            a.rows = extent.y;
            send(std::move(a));
            t->sentColumns = extent.x;
            t->sentRows = extent.y;
        }
    }
}

void RemoteTerminals::update(std::size_t id) {
    if (Tab* t = find(id)) paint(*t);
}

bool RemoteTerminals::ended(std::size_t id) {
    const Tab* t = find(id);
    return t != nullptr && t->closedElsewhere;
}

std::string RemoteTerminals::problem(std::size_t id) {
    const Tab* t = find(id);
    if (t == nullptr) return {};
    if (!t->refused.empty()) return t->refused;
    if (t->fate == sync::TerminalFate::Restarted) return sync::kTerminalRestarted;
    if (t->terminal == 0 && connected_ && !served_) {
        return "This server runs no terminals: it is older than they are. Update nexus-server, then open one again.";
    }
    return {};
}

std::string RemoteTerminals::title(std::size_t id) {
    const Tab* t = find(id);
    return t != nullptr && t->now ? t->now->title : std::string();
}

void RemoteTerminals::setTextScale(float k) {
    scale_ = k;
}

void RemoteTerminals::close(std::size_t id) {
    const auto it = tabs_.find(id);
    if (it == tabs_.end()) return;
    Tab& t = it->second;
    if (t.terminal != 0 && !t.closedElsewhere && t.fate != sync::TerminalFate::Restarted) {
        sync::Action a;
        a.kind = sync::ActionKind::TerminalClose;
        a.terminal = t.terminal;
        send(std::move(a));
        closing_.insert(t.terminal);
    } else if (t.terminal == 0 && !t.opener.empty() && t.refused.empty()) {
        orphans_.insert(t.opener);
    }
    prTerminalDestroy(t.view);
    scales_.erase(id);
    tabs_.erase(it);
}

void RemoteTerminals::sendBytes(Tab& t, std::string bytes) {
    for (std::size_t at = 0; at < bytes.size(); at += sync::kMaxTerminalInput) {
        sync::Action a;
        a.kind = sync::ActionKind::TerminalInput;
        a.terminal = t.terminal;
        a.secret = sync::SecretValue(std::string_view(bytes).substr(at, sync::kMaxTerminalInput));
        send(std::move(a));
    }
    sync::wipeBytes(bytes);
}

bool RemoteTerminals::typed(std::size_t id, const PrAppEvent& e) {
    Tab* t = find(id);
    if (t == nullptr || t->view == nullptr) return false;
    const bool typed = e.kind == PrAppEventKind_KeyDown || e.kind == PrAppEventKind_Text ||
        e.kind == PrAppEventKind_Paste;
    if (!typed) return false;
    if (e.kind == PrAppEventKind_KeyDown && e.keyboard.composing) return false;
    if (t->terminal == 0 || t->fate != sync::TerminalFate::Running) {
        // Nothing to type into (waiting, ended, lost): a key that would type is taken and goes nowhere; the rest, such
        // as Command-C, is the view's.
        return e.kind != PrAppEventKind_KeyDown || e.keyboard.text.size != 0;
    }
    std::string bytes;
    bool taken = false;
    {
        Locked screen(t->view);
        screen.drop();
        PrVec2 cell = prTerminalCellExtent(t->view);
        if (cell.x <= 0 || cell.y <= 0) cell = PrVec2{1, 1};
        const PrVtermInputDesc desc{.cellExtent = cell};
        PrAppEvent ev = e;
        if (control_ && (e.kind == PrAppEventKind_KeyDown || e.kind == PrAppEventKind_Text)) {
            // The key row's Control on the next letter: the letter's control character (C0), as Control would send.
            const PrStringView text = e.kind == PrAppEventKind_Text ? e.input.text : e.keyboard.text;
            if (text.size == 1) {
                const unsigned char c = static_cast<unsigned char>(text.data[0]);
                if ((c >= '@' && c <= '_') || (c >= 'a' && c <= 'z')) {
                    bytes.push_back(static_cast<char>(c & 0x1f));
                    control_ = false;
                    dropText_ = e.kind == PrAppEventKind_KeyDown ? std::string(text.data, text.size) : std::string();
                    taken = true;
                }
            }
        }
        if (!taken) {
            switch (e.kind) {
                case PrAppEventKind_KeyDown:
                    taken = prVtermInputEvent(screen.vterm, &ev, &desc);
                    dropText_ = taken ? std::string(e.keyboard.text.data, e.keyboard.text.size) : std::string();
                    break;
                case PrAppEventKind_Text:
                    if (!dropText_.empty() && std::string_view(e.input.text.data, e.input.text.size) == dropText_) {
                        taken = true;   // the key's own text, which its sequence carried
                    } else {
                        taken = prVtermInputEvent(screen.vterm, &ev, &desc) || e.input.text.size != 0;
                    }
                    dropText_.clear();
                    break;
                case PrAppEventKind_Paste:
                    prVtermInputPaste(screen.vterm, e.input.text);
                    taken = true;
                    break;
                default:
                    break;
            }
            bytes += screen.take();
        }
    }
    if (!bytes.empty()) sendBytes(*t, std::move(bytes));
    return taken;
}

void RemoteTerminals::key(std::size_t id, PrAppKey key, PrAppModifierFlags modifiers) {
    PrAppEvent e{};
    e.kind = PrAppEventKind_KeyDown;
    e.modifiers = modifiers | (control_ ? PrAppModifierFlag_Control : 0);
    e.keyboard.key = key;
    control_ = false;
    Tab* t = find(id);
    if (t == nullptr || t->view == nullptr || t->terminal == 0 || t->fate != sync::TerminalFate::Running) return;
    std::string bytes;
    {
        Locked screen(t->view);
        screen.drop();
        PrVec2 cell = prTerminalCellExtent(t->view);
        if (cell.x <= 0 || cell.y <= 0) cell = PrVec2{1, 1};
        const PrVtermInputDesc desc{.cellExtent = cell};
        prVtermInputEvent(screen.vterm, &e, &desc);
        bytes = screen.take();
    }
    if (!bytes.empty()) sendBytes(*t, std::move(bytes));
}

} // namespace nexus
