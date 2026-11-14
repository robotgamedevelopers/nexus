// The hitch detector: always on in the window. The client runs at the display's refresh rate, 120 Hz on ProMotion, so
// a frame's main-thread work has 1000 / 120 = 8.3 ms (AGENTS.md "Real time"). A frame whose work runs past that
// budget is a hitch, and a hitch is a bug.
//
// Each frame the host calls begin() as the frame's work starts (after the swap chain's wait, which is not work) and
// end() once it is drawn. Between them, and between frames for input, it names the phases of the frame with cheap
// scoped markers (HitchScope) or adds the times Prax measured itself (add). Work done between two frames, such as an
// event, counts in the next frame: it delays that frame just the same.
//
// A hitch is kept in a ring of kHitchRing entries, each with the frame's number, its work, the time from the frame
// before it began, and the time in each named phase. The ring and every entry are fixed in size: nothing allocates per
// frame. Each hitch is written as one line to the log, at most one line a second; the line after a quiet spell says
// how many it left out. Tests read the counter and the ring.
//
// The clock is passed in, in milliseconds on a monotonic clock, so unit tests drive it. No OS, no Prax.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nexus {

// The phases a frame's work is named by. Phases may nest: Layout (Prax's rounds) holds the tab updates made inside the
// window's build, and the entry shows each as measured.
enum class Phase : std::uint8_t {
    Input,          // events, between frames and in the frame
    Terminals,      // reading each terminal's label and directory at the frame's start
    Keep,           // handing the window's saved preferences to their writer
    SyncApply,      // the server's latest state into the model (sync/mirror.h)
    Board,          // the sprint tab's page at the frame's start (board.h frame)
    Layout,         // the rounds: the window's build and Prax's layout
    Draw,           // taking the tree's scene and playing it into the draw list
    Present,        // rendering the list into the swap chain
    TerminalTab,    // one terminal tab's update or show, with its Tab::id
    Other,          // phases past kHitchPhases in one frame, together
    Count,
};

// The phase's name, as the log line gives it.
const char* phaseName(Phase phase);

// The time in one phase of one frame.
struct PhaseTime {
    Phase phase = Phase::Other;
    std::uint32_t tab = 0;      // with TerminalTab, the tab's Tab::id
    float ms = 0;
};

// The most phases one entry holds; more in one frame are summed into Other.
constexpr std::size_t kHitchPhases = 16;
// The hitches the ring keeps, newest replacing oldest.
constexpr std::size_t kHitchRing = 64;

struct Hitch {
    std::uint64_t frame = 0;    // the frame's number, from 1
    double work = 0;            // its main-thread work, ms
    double interval = 0;        // from the previous frame's start, ms; 0 for the first frame
    std::uint8_t phaseCount = 0;
    std::array<PhaseTime, kHitchPhases> phases{};
};

// A clock in milliseconds from a monotonic origin. `now` is called with `context`.
struct HitchClock {
    double (*now)(void* context) = nullptr;
    void* context = nullptr;
    double read() const { return now(context); }
};

// Where a hitch's line goes; the line has no line feed. Called on the thread that ends the frame.
struct HitchLog {
    void (*write)(void* context, const char* line) = nullptr;
    void* context = nullptr;
};

// The refresh rate the budget comes from when the display gives none.
constexpr double kDefaultRefreshHz = 120;

class HitchDetector {
public:
    HitchDetector(HitchClock clock, HitchLog log);

    // The budget, from the display's refresh rate in Hz: 1000 / hz ms. Zero or less (none known) is kDefaultRefreshHz.
    void setRefreshRate(double hz);
    double budget() const { return budget_; }

    // The frame's work starts / ends. A begin() with no end() since is ended first.
    void begin();
    void end();
    bool inFrame() const { return inFrame_; }

    // Adds `ms` to `phase` (with `tab` for TerminalTab), in the frame that runs or else the next one. Phases measured
    // outside a frame count in the next frame's work too.
    void add(Phase phase, double ms, std::uint32_t tab = 0);

    double now() const { return clock_.read(); }

    // Frames ended, and hitches among them.
    std::uint64_t frames() const { return frames_; }
    std::uint64_t hitches() const { return hitches_; }
    // The worst frame's work, ms, of every frame ended.
    double worst() const { return worst_; }
    // The hitches the ring holds, oldest first: `i` from 0 to held() less one.
    std::size_t held() const;
    const Hitch& hitch(std::size_t i) const;
    // The frame being measured (or the work waiting for the next one), as end() would keep it.
    const Hitch& current() const { return current_; }

    // Writes the hitch as one line into `out` (at most `size` bytes, NUL ended), as the log gets it. Returns `out`.
    static const char* format(const Hitch& h, double budget, char* out, std::size_t size);

private:
    void log(const Hitch& h);

    HitchClock clock_;
    HitchLog log_;
    double budget_ = 1000.0 / kDefaultRefreshHz;
    bool inFrame_ = false;
    double began_ = 0;
    double lastBegan_ = -1;
    double between_ = 0;            // phases measured since the last frame ended, outside a frame
    std::uint64_t frames_ = 0;
    std::uint64_t hitches_ = 0;
    double worst_ = 0;
    Hitch current_;
    std::array<Hitch, kHitchRing> ring_{};
    std::size_t next_ = 0;          // where the next hitch goes
    double loggedAt_ = -1e300;      // when the last line was written
    std::uint64_t unlogged_ = 0;    // hitches since then not written
};

// Times its scope as `phase` of the detector's frame. A null detector times nothing.
class HitchScope {
public:
    HitchScope(HitchDetector* detector, Phase phase, std::uint32_t tab = 0)
            : detector_(detector), phase_(phase), tab_(tab), start_(detector != nullptr ? detector->now() : 0) {}
    ~HitchScope() {
        if (detector_ != nullptr)
            detector_->add(phase_, detector_->now() - start_, tab_);
    }
    HitchScope(const HitchScope&) = delete;
    HitchScope& operator=(const HitchScope&) = delete;

private:
    HitchDetector* detector_;
    Phase phase_;
    std::uint32_t tab_;
    double start_;
};

} // namespace nexus
