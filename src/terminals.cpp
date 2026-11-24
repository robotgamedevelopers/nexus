#include "terminals.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include "pr/file.h"
#include "pr/layout_style.h"
#include "pr/raw_string.h"
#include "pr/vterm.h"

#include "foreground.h"
#include "command.h"
#include "style.h"
#include "terminal_launch.h"

namespace nexus {

namespace {

// The monospaced family the terminals are set in, as Prax's terminal app sets them (terminal_app.cpp kMono): the
// system's fixed-pitch font.
const PrFontFamily kMono{.name = {}, .generic = PrFontGeneric_Monospace};

// How often the start thread reads each shell's foreground process, and the frame thread each terminal's directory.
constexpr std::chrono::milliseconds kWatchEvery{250};
constexpr double kDirectoriesEvery = 1.0;

uint8_t channel(float v) {
    return uint8_t(std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255));
}

PrStringView view(const std::string& s) {
    return PrStringView{s.data(), s.size()};
}

} // namespace

PraxTerminals::PraxTerminals(PrKit* kit, PrFontSet* fonts, PrApp* app, PrAppWindow* window)
    : kit_(kit), fonts_(fonts), app_(app), window_(window),
      launcher_(prProcessLauncherCreate(PrProcessBackend_Default)), thread_([this] { run(); }) {}

PraxTerminals::~PraxTerminals() {
    closeAll();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ending_ = true;
    }
    wake_.notify_one();
    thread_.join();
    // Shells started after the frame thread last took them: no tab waits for them.
    for (const Started& s : started_) {
        prTerminalDestroy(s.terminal);
    }
    prProcessLauncherDestroy(launcher_);
}

void PraxTerminals::closeAll() {
    take();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, terminal] : terminals_) {
            ends_.push_back(terminal);
        }
        shells_.clear();
        watch_.clear();
        recheck_ = false;
    }
    wake_.notify_one();
    terminals_.clear();
    labels_.clear();
    pids_.clear();
    seen_.clear();
    titles_.clear();
    asked_.clear();
    failed_.clear();
    problems_.clear();
    dirs_.clear();
    scales_.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// The start thread

void PraxTerminals::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto watchAt = std::chrono::steady_clock::now();
    for (;;) {
        wake_.wait_until(lock, watchAt, [this] { return ending_ || !starts_.empty() || !ends_.empty() || recheck_; });
        std::vector<PrTerminal*> ends;
        ends.swap(ends_);
        std::deque<Start> starts;
        starts.swap(starts_);
        const bool ending = ending_;
        lock.unlock();
        for (PrTerminal* terminal : ends) {
            prTerminalDestroy(terminal);
        }
        for (const Start& s : starts) {
            Started done = ending ? Started{s.id, nullptr, s.scale, s.dir, {}} : start(s);
            lock.lock();
            started_.push_back(std::move(done));
            lock.unlock();
            if (app_ != nullptr) {
                prAppWake(app_);   // a frame takes it up (pr/studio_app.h)
            }
        }
        lock.lock();
        if (ending_ && starts_.empty() && ends_.empty()) {
            return;
        }
        if (!ending_ && (recheck_ || std::chrono::steady_clock::now() >= watchAt)) {
            recheck_ = false;
            const std::map<std::size_t, int> shells = shells_;
            const std::vector<Machine> machines = watched_;
            const std::map<std::size_t, ForegroundSeen> snapshot = watch_;
            lock.unlock();
            struct Polled {
                std::size_t id;
                ForegroundSeen snap;
                int group;
                std::string remote;
            };
            std::vector<Polled> polled;
            polled.reserve(shells.size());
            for (const auto& [id, pid] : shells) {
                auto it = snapshot.find(id);
                const ForegroundSeen snap = it != snapshot.end() ? it->second : ForegroundSeen{};
                const int group = foregroundGroup(pid);
                std::string remote = pollNeedsArguments(snap, group)
                    ? remoteLabel(sshDestination(processArguments(group)), machines)
                    : snap.remote;
                polled.push_back(Polled{id, snap, group, std::move(remote)});
            }
            lock.lock();
            bool changed = false;
            for (Polled& p : polled) {
                if (shells_.count(p.id) == 0) {
                    continue;
                }
                const PollCommit c = commitPollForeground(watch_[p.id], p.snap, p.group, std::move(p.remote));
                changed = changed || c.changed;
                recheck_ = recheck_ || c.recheck;
            }
            watchAt = std::chrono::steady_clock::now() + kWatchEvery;
            if (changed && !ending_ && app_ != nullptr) {
                lock.unlock();
                prAppWake(app_);
                lock.lock();
            }
        }
    }
}

PraxTerminals::Started PraxTerminals::start(const Start& s) {
    Started done{s.id, nullptr, s.scale, s.dir, {}};
    std::string where = s.dir;
    if (kShellHost == ShellHost::Wsl2 && s.program.empty()) {
        // WSL2 is there, and the saved directory still a directory inside it (terminal_launch.h).
        const Command probe = wsl2Probe(where);
        const Wsl2Verdict verdict = wsl2Verdict(probe, runCommand(probe, kWsl2ProbeTimeoutSeconds), where,
            kWsl2ProbeTimeoutSeconds);
        if (!verdict.problem.empty()) {
            std::fprintf(stderr, "nexus: a terminal tab cannot start: %s\n", verdict.problem.c_str());
            done.problem = verdict.problem;
            return done;
        }
        if (verdict.home) {
            std::fprintf(stderr, "nexus: %s is no longer a directory inside WSL2; the terminal starts in the WSL2 home "
                "directory\n", where.c_str());
            where.clear();
        }
    } else {
        PrFileInfo info{};
        if (!where.empty() && (!prFileInfo(view(where), &info) || info.kind != PrFileKind_Directory)) {
            std::fprintf(stderr, "nexus: %s is no longer a directory; the terminal starts in the home directory\n",
                where.c_str());
            where.clear();
        }
    }
    const TerminalLaunch launch = terminalLaunch(kShellHost, s.program, where);
    std::vector<PrStringView> arguments;
    for (const std::string& a : launch.arguments) {
        arguments.push_back(view(a));
    }
    PrRawString home = prRawStringNull();
    if (launch.directory.empty()) {
        home = prFileUserDirectory(PrFileUserDirectory_Home);
    }
    PrTerminalDesc desc{};
    desc.font = font(s.scale);
    desc.app = app_;
    desc.launcher = launcher_;
    desc.program = view(launch.program);
    desc.arguments = arguments.empty() ? nullptr : arguments.data();
    desc.argumentCount = uint32_t(arguments.size());
    desc.directory = launch.directory.empty() ? prRawStringView(&home) : view(launch.directory);
    desc.scrollback = kPrTerminalScrollbackDefault;
    desc.notificationTag = s.id + 1;
    done.terminal = prTerminalCreate(&desc);
    prRawStringReset(&home);
    return done;
}

// ---------------------------------------------------------------------------------------------------------------------
// The frame thread

void PraxTerminals::take() {
    std::vector<Started> started;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started.swap(started_);
        watched_ = machines;
        for (const Started& s : started) {
            const bool wanted = asked_.count(s.id) != 0 && s.terminal != nullptr;
            if (!wanted && s.terminal != nullptr) {
                ends_.push_back(s.terminal);     // its tab closed while it started
            }
        }
        for (const auto& [id, shared] : watch_) {
            takeForegroundWatch(shared, seen_[id]);
        }
    }
    bool ends = false;
    for (const Started& s : started) {
        if (asked_.erase(s.id) == 0) {
            ends = ends || s.terminal != nullptr;
            continue;
        }
        if (s.terminal == nullptr) {
            if (!s.problem.empty()) {
                problems_[s.id] = s.problem;    // the tab stays, showing the line
            } else {
                failed_.insert(s.id);
            }
            held_.erase(s.id);
            continue;
        }
        terminals_[s.id] = s.terminal;
        labels_[s.id] = TabLabel();
        dirs_[s.id] = s.dir;
        scales_[s.id] = s.scale;
        applyBackground(s.terminal);
        prTerminalFocus(s.terminal);
    }
    if (ends) {
        wake_.notify_one();
    }
}

void PraxTerminals::observe(double time) {
    take();
    struct Observed {
        std::size_t id;
        int group;
        std::string remote;
    };
    std::vector<Observed> updates;
    for (auto& [id, terminal] : terminals_) {
        ForegroundSeen& seen = seen_[id];
        // A new title: the label must see the foreground as of it, so the group is read now, and the arguments of a
        // group not seen before.
        const std::string title = programTitle(id);
        auto last = titles_.find(id);
        if (last == titles_.end() || last->second != title) {
            auto pid = pids_.find(id);
            const int group = pid != pids_.end() ? foregroundGroup(pid->second) : -1;
            std::string remote = foregroundNeedsArguments(seen, group)
                ? remoteLabel(sshDestination(processArguments(group)), machines)
                : seen.remote;
            updates.push_back(Observed{id, group, std::move(remote)});
            titles_[id] = title;
        }
    }
    if (!updates.empty()) {
        bool recheck = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Observed& u : updates) {
                ForegroundSeen& seen = seen_[u.id];
                if (shells_.count(u.id) != 0) {
                    recheck = commitFrameForeground(watch_[u.id], seen, u.group, std::move(u.remote)) || recheck;
                } else {
                    seen = ForegroundSeen{seen.seq + 1, u.group, std::move(u.remote)};
                }
            }
            if (recheck) {
                recheck_ = true;
            }
        }
        if (recheck) {
            wake_.notify_one();
        }
    }
    for (auto& [id, terminal] : terminals_) {
        labels_[id].observe(seen_[id].remote, titles_[id], time);
    }
    // One terminal's directory a frame, each terminal once a second.
    if (time - dirsAt_ >= kDirectoriesEvery || dirNext_ != 0) {
        if (dirNext_ == 0) {
            dirsAt_ = time;
        }
        auto it = terminals_.lower_bound(dirNext_);
        if (it == terminals_.end()) {
            dirNext_ = 0;
        } else {
            readDirectory(it->first, it->second);
            dirNext_ = it->first + 1;
        }
    }
}

void PraxTerminals::readDirectory(std::size_t id, PrTerminal* terminal) {
    PrRawString dir = prTerminalWorkingDirectory(terminal);
    std::string result;
    if (!prRawStringIsNull(&dir)) {
        PrStringView v = prRawStringView(&dir);
        result.assign(v.data != nullptr ? v.data : "", v.size);
    }
    prRawStringReset(&dir);
    dirs_[id] = result;
}

namespace {

bool carriesText(const PrAppEvent& e) {
    return e.kind == PrAppEventKind_Text || e.kind == PrAppEventKind_Paste;
}

} // namespace

bool PraxTerminals::hold(std::size_t id, const PrAppEvent& e) {
    if (e.kind != PrAppEventKind_KeyDown && e.kind != PrAppEventKind_KeyUp && !carriesText(e)) {
        return false;
    }
    // nexus's own chords (Command-W, Command-T, the tab keys) act at once, whatever the shell.
    if ((e.modifiers & style::kCommand) != 0 || failed_.count(id) != 0 || problems_.count(id) != 0) {
        return false;
    }
    auto queued = held_.find(id);
    if (queued == held_.end() || queued->second.empty()) {
        PrTerminal* terminal = find(id);
        const bool starting = asked_.count(id) != 0;
        if (!starting && (terminal == nullptr || (prTerminalLaunched(terminal) && prTerminalHasFocus(terminal)))) {
            return false;
        }
    }
    Held h{e, carriesText(e) ? std::string(e.input.text.data, e.input.text.size) : std::string()};
    held_[id].push_back(std::move(h));
    return true;
}

void PraxTerminals::deliver(PrUi* ui) {
    for (auto it = held_.begin(); it != held_.end();) {
        PrTerminal* terminal = find(it->first);
        if (terminal == nullptr || !prTerminalLaunched(terminal) || !prTerminalHasFocus(terminal)) {
            ++it;
            continue;
        }
        for (Held& h : it->second) {
            PrAppEvent e = h.event;
            if (carriesText(e)) {
                e.input.text = PrStringView{h.text.data(), h.text.size()};
            }
            prUiPushEvent(ui, kPrUiViewportMain, &e);
        }
        it = held_.erase(it);
    }
}

std::size_t PraxTerminals::held(std::size_t id) const {
    auto it = held_.find(id);
    return it != held_.end() ? it->second.size() : 0;
}

void PraxTerminals::readDirectories() {
    take();
    for (auto& [id, terminal] : terminals_) {
        readDirectory(id, terminal);
    }
}

PrTerminal* PraxTerminals::find(std::size_t id) {
    auto it = terminals_.find(id);
    return it != terminals_.end() ? it->second : nullptr;
}

PrTextFontDesc PraxTerminals::font(float scale) const {
    PrFontStyle s = prFontStyleDefault();
    s.size = style::kTerminalSize * scale;
    return PrTextFontDesc{.set = fonts_, .families = &kMono, .familyCount = 1, .style = s, .locale = {}};
}

bool PraxTerminals::open(std::size_t id) {
    return openIn(id, directory);
}

bool PraxTerminals::openIn(std::size_t id, const std::string& dir) {
    if (launcher_ == nullptr) {
        std::fprintf(stderr, "nexus: no process launcher on this platform, so no terminal can start\n");
        return false;
    }
    asked_[id] = dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        starts_.push_back(Start{id, program, dir, scale_});
    }
    wake_.notify_one();
    return true;
}

void PraxTerminals::close(std::size_t id) {
    asked_.erase(id);
    failed_.erase(id);
    problems_.erase(id);
    dirs_.erase(id);
    scales_.erase(id);
    labels_.erase(id);
    pids_.erase(id);
    seen_.erase(id);
    titles_.erase(id);
    held_.erase(id);
    auto it = terminals_.find(id);
    if (it == terminals_.end()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ends_.push_back(it->second);
        shells_.erase(id);
        watch_.erase(id);
    }
    wake_.notify_one();
    terminals_.erase(it);
}

// Sets the screen's background colour to the window's if it is not, under the screen's lock. Returns true if it was
// not: Prax set it from the kit's theme (the terminal's first frame, or a palette change).
bool PraxTerminals::applyBackground(PrTerminal* terminal) const {
    const auto canvas = kit_->theme.palette.bg0;
    const PrColor8 background{channel(canvas.r), channel(canvas.g), channel(canvas.b), channel(canvas.a)};
    PrVterm* vterm = prTerminalVtermLock(terminal);
    PrColor8 now = prVtermRoleColor(vterm, PrVtermColorRole_Background);
    bool changed = now.r != background.r || now.g != background.g || now.b != background.b ||
        now.a != background.a;
    if (changed) {
        prVtermRoleSetColor(vterm, PrVtermColorRole_Background, background);
    }
    prTerminalVtermUnlock(terminal);
    return changed;
}

void PraxTerminals::show(std::size_t id, PrLayoutNode parent) {
    take();
    PrTerminal* terminal = find(id);
    if (terminal == nullptr) {
        return;     // starting: the tab shows the window's background until its shell is there; what is typed meanwhile
                    // is held (hold) and reaches the shell once its screen has the focus
    }
    if (scales_[id] != scale_) {
        scales_[id] = scale_;
        PrTextFontDesc f = font(scale_);
        prTerminalSetFont(terminal, &f);
    }
    if (!prTerminalHasFocus(terminal)) {
        prTerminalFocus(terminal);
    }
    applyBackground(terminal);
    PrLayoutTree* tree = kit_->tree;
    const PrLayoutStyleIndex before = prLayoutTreeFrameStyle(tree, parent);
    const bool starting = !prTerminalLaunched(terminal);
    const auto beforeChildren = starting ? childProcesses() : std::vector<int>{};
    prTerminalFrame(terminal, kit_, parent, PrTerminalBackground_Parent);
    if (starting && prTerminalLaunched(terminal)) {
        if (prTerminalLaunchStatus(terminal) != PrStatus_Ok) {
            failed_.insert(id);
        } else if (const int pid = startedShell(beforeChildren, childProcesses()); pid > 0) {
            pids_[id] = pid;
            std::lock_guard lock(mutex_);
            shells_[id] = pid;
            watch_[id] = ForegroundSeen{};
        }
    }
    // A frame in which Prax reset the colour drew with the theme's opaque one: the window keeps its own background
    // this frame, and the next frame draws with the window's colour.
    if (applyBackground(terminal)) {
        prLayoutTreeFrameSetStyle(tree, parent, before);
    }
}

void PraxTerminals::update(std::size_t id) {
    if (PrTerminal* terminal = find(id)) {
        prTerminalService(terminal, window_);
    }
}

bool PraxTerminals::ended(std::size_t id) {
    take();
    if (failed_.count(id) != 0) {
        return true;
    }
    PrTerminal* terminal = find(id);
    if (terminal == nullptr) {
        return false;
    }
    PrProcessState info = prTerminalProcessState(terminal);
    return info.exited && info.outputEnded;
}

std::string PraxTerminals::problem(std::size_t id) {
    take();
    auto it = problems_.find(id);
    return it != problems_.end() ? it->second : std::string();
}

std::string PraxTerminals::programTitle(std::size_t id) {
    PrTerminal* terminal = find(id);
    if (terminal == nullptr) {
        return {};
    }
    PrStringView t = prTerminalTitle(terminal);
    return std::string(t.data != nullptr ? t.data : "", t.size);
}

std::string PraxTerminals::title(std::size_t id) {
    const std::string title = programTitle(id);
    auto label = labels_.find(id);
    return label != labels_.end() ? label->second.label(title) : title;
}

// The font of the terminal that shows changes now; each other one's when it next shows (show).
void PraxTerminals::setTextScale(float k) {
    scale_ = k;
}

std::string PraxTerminals::text(std::size_t id) {
    PrTerminal* terminal = find(id);
    if (terminal == nullptr) {
        return {};
    }
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

std::string PraxTerminals::workingDirectory(std::size_t id) {
    auto asked = asked_.find(id);
    if (asked != asked_.end()) {
        return asked->second;
    }
    auto dir = dirs_.find(id);
    return dir != dirs_.end() ? dir->second : std::string();
}

} // namespace nexus
