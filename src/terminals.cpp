#include "terminals.h"

#include <cmath>
#include <cstdio>

#include "pr/file.h"
#include "pr/layout_style.h"
#include "pr/raw_string.h"
#include "pr/vterm.h"

#include "style.h"

namespace nexus {

namespace {

// The monospaced family the terminals are set in, as Prax's terminal app sets them (terminal_app.cpp kMono): the
// system's fixed-pitch font.
const PrFontFamily kMono{.name = {}, .generic = PrFontGeneric_Monospace};

uint8_t channel(float v) {
    return uint8_t(std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255));
}

PrStringView view(const std::string& s) {
    return PrStringView{s.data(), s.size()};
}

} // namespace

PraxTerminals::PraxTerminals(PrKit* kit, PrFontSet* fonts, PrApp* app, bool overlay)
    : kit_(kit), fonts_(fonts), app_(app),
      background_{channel(style::kBackground.r), channel(style::kBackground.g), channel(style::kBackground.b),
          channel(overlay ? style::kOverlayOpacity : 1.0f)},
      launcher_(prProcessLauncherCreate(PrProcessBackend_Default)) {}

PraxTerminals::~PraxTerminals() {
    closeAll();
    prProcessLauncherDestroy(launcher_);
}

void PraxTerminals::closeAll() {
    for (auto& [id, terminal] : terminals_)
        prTerminalDestroy(terminal);
    terminals_.clear();
}

PrTerminal* PraxTerminals::find(std::size_t id) const {
    auto it = terminals_.find(id);
    return it != terminals_.end() ? it->second : nullptr;
}

PrTextFontDesc PraxTerminals::font() const {
    PrFontStyle s = prFontStyleDefault();
    s.size = style::kTerminalSize * scale_;
    return PrTextFontDesc{.set = fonts_, .families = &kMono, .familyCount = 1, .style = s, .locale = {}};
}

bool PraxTerminals::open(std::size_t id) {
    if (launcher_ == nullptr) {
        std::fprintf(stderr, "nexus: no process launcher on this platform, so no terminal can start\n");
        return false;
    }
    PrRawString home = prRawStringNull();
    if (directory.empty())
        home = prFileUserDirectory(PrFileUserDirectory_Home);
    PrTerminalDesc desc{};
    desc.kit = kit_;
    desc.font = font();
    desc.app = app_;
    desc.launcher = launcher_;
    desc.program = view(program);
    desc.directory = directory.empty() ? prRawStringView(&home) : view(directory);
    desc.scrollback = kPrTerminalScrollbackDefault;
    desc.notificationTag = id + 1;
    PrTerminal* terminal = nullptr;
    PrStatus status = prTerminalCreate(&desc, &terminal);
    prRawStringReset(&home);
    if (status != PrStatus_Ok || terminal == nullptr) {
        std::fprintf(stderr, "nexus: could not start %s for a terminal tab (status %d): check that SHELL names a "
            "shell that runs\n", program.empty() ? "the login shell" : program.c_str(), int(status));
        return false;
    }
    terminals_[id] = terminal;
    applyBackground(terminal);
    prTerminalFocus(terminal);
    return true;
}

void PraxTerminals::close(std::size_t id) {
    auto it = terminals_.find(id);
    if (it == terminals_.end())
        return;
    prTerminalDestroy(it->second);
    terminals_.erase(it);
}

// Sets the screen's background colour to the window's if it is not, under the screen's lock. Returns true if it was
// not: Prax set it from the kit's theme (the terminal's first frame, or a palette change).
bool PraxTerminals::applyBackground(PrTerminal* terminal) const {
    PrVterm* vterm = prTerminalVtermLock(terminal);
    PrColor8 now = prVtermRoleColor(vterm, PrVtermColorRole_Background);
    bool changed = now.r != background_.r || now.g != background_.g || now.b != background_.b ||
        now.a != background_.a;
    if (changed)
        prVtermRoleSetColor(vterm, PrVtermColorRole_Background, background_);
    prTerminalVtermUnlock(terminal);
    return changed;
}

void PraxTerminals::show(std::size_t id, PrLayoutNode parent, PrVec2 extent) {
    PrTerminal* terminal = find(id);
    if (terminal == nullptr)
        return;
    if (!prTerminalHasFocus(terminal))
        prTerminalFocus(terminal);
    applyBackground(terminal);
    PrLayoutTree* tree = kit_->tree;
    const PrLayoutStyleIndex before = prLayoutTreeFrameStyle(tree, parent);
    prTerminalFrame(terminal, parent, extent, PrTerminalBackground_Parent);
    // A frame in which Prax reset the colour drew with the theme's opaque one: the window keeps its own background
    // this frame, and the next frame draws with the window's colour.
    if (applyBackground(terminal))
        prLayoutTreeFrameSetStyle(tree, parent, before);
}

void PraxTerminals::update(std::size_t id) {
    if (PrTerminal* terminal = find(id))
        prTerminalUpdate(terminal);
}

bool PraxTerminals::ended(std::size_t id) {
    PrTerminal* terminal = find(id);
    if (terminal == nullptr)
        return false;
    PrProcessInfo info = prTerminalProcessInfo(terminal);
    return info.exited && info.outputEnded;
}

std::string PraxTerminals::title(std::size_t id) {
    PrTerminal* terminal = find(id);
    if (terminal == nullptr)
        return {};
    PrStringView t = prTerminalTitle(terminal);
    return std::string(t.data != nullptr ? t.data : "", t.size);
}

void PraxTerminals::setTextScale(float k) {
    if (k == scale_)
        return;
    scale_ = k;
    PrTextFontDesc f = font();
    for (auto& [id, terminal] : terminals_)
        prTerminalSetFont(terminal, &f);
}

std::string PraxTerminals::text(std::size_t id) {
    PrTerminal* terminal = find(id);
    if (terminal == nullptr)
        return {};
    PrVterm* vterm = prTerminalVtermLock(terminal);
    const uint32_t lines = prVtermScrollbackCount(vterm) + prVtermExtent(vterm).y;
    PrRawString out = prRawStringNull();
    for (uint32_t i = 0; i < lines; ++i) {
        prVtermLineAppendText(vterm, i, 0, ~uint32_t(0), &out);
        prRawStringAppend(&out, PrStringView{"\n", 1});
    }
    prTerminalVtermUnlock(terminal);
    PrStringView v = prRawStringView(&out);
    std::string result(v.data != nullptr ? v.data : "", v.size);
    prRawStringReset(&out);
    return result;
}

} // namespace nexus
