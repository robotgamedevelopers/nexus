#include "terminals.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>

#include "pr/file.h"
#include "pr/layout_style.h"
#include "pr/raw_string.h"
#include "pr/vterm.h"

#include "foreground.h"
#include "style.h"

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

PraxTerminals::PraxTerminals(PrKit* kit, PrFontSet* fonts, PrApp* app, PrAppWindow* window, bool overlay)
    : kit_(kit), fonts_(fonts), app_(app), window_(window),
      background_{channel(style::kBackground.r), channel(style::kBackground.g), channel(style::kBackground.b),
          channel(overlay ? style::kOverlayOpacity : 1.0f)},
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
    }
    wake_.notify_one();
    terminals_.clear();
    labels_.clear();
    pids_.clear();
    seen_.clear();
    titles_.clear();
    asked_.clear();
    failed_.clear();
    dirs_.clear();
    scales_.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// The start thread

void PraxTerminals::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto watchAt = std::chrono::steady_clock::now();
    for (;;) {
        wake_.wait_until(lock, watchAt, [this] { return ending_ || !starts_.empty() || !ends_.empty(); });
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
            Started done = ending ? Started{s.id, nullptr, -1, s.scale, s.dir} : start(s);
            lock.lock();
            started_.push_back(std::move(done));
            lock.unlock();
            if (app_ != nullptr) {
                prAppWake(app_);   // a frame takes it up (pr/studio_app.h)
            }
        }
        if (ending) {
            lock.lock();
            if (starts_.empty() && ends_.empty()) {
                return;
            }
            continue;
        }
        if (std::chrono::steady_clock::now() >= watchAt) {
            lock.lock();
            const std::map<std::size_t, int> shells = shells_;
            const std::vector<Machine> machines = watched_;
            std::map<std::size_t, Seen> watch = watch_;
            lock.unlock();
            bool changed = false;
            for (const auto& [id, pid] : shells) {
                const int group = foregroundGroup(pid);
                Seen& seen = watch[id];
                if (group != seen.group || seen.at == std::chrono::steady_clock::time_point{}) {
                    seen = Seen{group, remoteLabel(sshDestination(processArguments(group)), machines),
                        std::chrono::steady_clock::now()};
                    changed = true;
                }
            }
            lock.lock();
            for (auto it = watch.begin(); it != watch.end();) {
                it = shells_.count(it->first) != 0 ? std::next(it) : watch.erase(it);
            }
            watch_ = std::move(watch);
            watchAt = std::chrono::steady_clock::now() + kWatchEvery;
            if (changed && app_ != nullptr) {
                prAppWake(app_);
            }
            continue;
        }
        lock.lock();
    }
}

PraxTerminals::Started PraxTerminals::start(const Start& s) {
    Started done{s.id, nullptr, -1, s.scale, s.dir};
    std::string where = s.dir;
    PrFileInfo info{};
    if (!where.empty() && (!prFileInfo(view(where), &info) || info.kind != PrFileKind_Directory)) {
        std::fprintf(stderr, "nexus: %s is no longer a directory; the terminal starts in the home directory\n",
            where.c_str());
        where.clear();
    }
    PrRawString home = prRawStringNull();
    if (where.empty()) {
        home = prFileUserDirectory(PrFileUserDirectory_Home);
    }
    PrTerminalDesc desc{};
    desc.font = font(s.scale);
    desc.app = app_;
    desc.launcher = launcher_;
    desc.program = view(s.program);
    desc.directory = where.empty() ? prRawStringView(&home) : view(where);
    desc.scrollback = kPrTerminalScrollbackDefault;
    desc.notificationTag = s.id + 1;
    // The shell is the child that prTerminalCreate starts (foreground.h): it returns once the shell has started. Only
    // this thread starts and ends shells, so no other child comes or goes meanwhile.
    const std::vector<int> before = childProcesses();
    PrStatus status = prTerminalCreate(&desc, &done.terminal);
    prRawStringReset(&home);
    if (status != PrStatus_Ok || done.terminal == nullptr) {
        std::fprintf(stderr, "nexus: could not start %s for a terminal tab (status %d): check that SHELL names a "
            "shell that runs\n", s.program.empty() ? "the login shell" : s.program.c_str(), int(status));
        done.terminal = nullptr;
        return done;
    }
    done.shell = startedShell(before, childProcesses());
    if (done.shell <= 0) {
        std::fprintf(stderr, "nexus: could not find the shell of a new terminal tab among nexus's child processes; the "
            "tab keeps its title while ssh runs in it\n");
    }
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
            } else if (wanted && s.shell > 0) {
                shells_[s.id] = s.shell;
            }
        }
    }
    bool ends = false;
    for (const Started& s : started) {
        if (asked_.erase(s.id) == 0) {
            ends = ends || s.terminal != nullptr;
            continue;
        }
        if (s.terminal == nullptr) {
            failed_.insert(s.id);
            continue;
        }
        terminals_[s.id] = s.terminal;
        labels_[s.id] = TabLabel();
        if (s.shell > 0) {
            pids_[s.id] = s.shell;
        }
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
    std::map<std::size_t, Seen> watch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        watch = watch_;
    }
    for (auto& [id, terminal] : terminals_) {
        Seen& seen = seen_[id];
        auto later = watch.find(id);
        if (later != watch.end() && later->second.at > seen.at) {
            seen = later->second;
        }
        // A new title: the label must see the foreground as of it, so the group is read now, and the arguments of a
        // group not seen before.
        const std::string title = programTitle(id);
        auto last = titles_.find(id);
        if (last == titles_.end() || last->second != title) {
            auto pid = pids_.find(id);
            const int group = pid != pids_.end() ? foregroundGroup(pid->second) : -1;
            if (group != seen.group || seen.at == std::chrono::steady_clock::time_point{}) {
                seen.group = group;
                seen.remote = remoteLabel(sshDestination(processArguments(group)), machines);
            }
            seen.at = std::chrono::steady_clock::now();
            titles_[id] = title;
        }
        labels_[id].observe(seen.remote, title, time);
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
    dirs_.erase(id);
    scales_.erase(id);
    labels_.erase(id);
    pids_.erase(id);
    seen_.erase(id);
    titles_.erase(id);
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
    PrVterm* vterm = prTerminalVtermLock(terminal);
    PrColor8 now = prVtermRoleColor(vterm, PrVtermColorRole_Background);
    bool changed = now.r != background_.r || now.g != background_.g || now.b != background_.b ||
        now.a != background_.a;
    if (changed) {
        prVtermRoleSetColor(vterm, PrVtermColorRole_Background, background_);
    }
    prTerminalVtermUnlock(terminal);
    return changed;
}

void PraxTerminals::show(std::size_t id, PrLayoutNode parent) {
    take();
    PrTerminal* terminal = find(id);
    if (terminal == nullptr) {
        return;     // starting: the tab shows the window's background until its shell is there
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
    prTerminalFrame(terminal, kit_, parent, PrTerminalBackground_Parent);
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
    PrProcessInfo info = prTerminalProcessInfo(terminal);
    return info.exited && info.outputEnded;
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
