#include "cursor.h"

#include <algorithm>
#include <cmath>

namespace nexus {

namespace {

// The colours, in OKLCH. Chosen by eye against the window's background (style.h kBackground, OKLCH lightness 0.21),
// not measured: light blue breathes between the two lightnesses; dark blue is the same hue, darker and stronger; red
// and grey stand clear of both. Grey has no chroma and is dimmer than every blue, so a lost link never reads as one.
constexpr Oklch kBusyDim{0.66f, 0.10f, 235};
constexpr Oklch kBusyBright{0.86f, 0.10f, 235};
constexpr Oklch kIdle{0.50f, 0.15f, 262};
constexpr Oklch kDown{0.62f, 0.21f, 27};
constexpr Oklch kLost{0.42f, 0, 0};

constexpr double kPi = 3.14159265358979323846;

} // namespace

int breathLevel(double seconds) {
    const double t = std::max(seconds, 0.0);
    const double phase = std::fmod(t, kBreathSeconds) / kBreathSeconds;
    const double rise = 0.5 - 0.5 * std::cos(2 * kPi * phase);   // 0 at the start of a breath, 1 halfway
    return std::clamp(static_cast<int>(std::lround(rise * (kBreathLevels - 1))), 0, kBreathLevels - 1);
}

Oklch cursorColor(CursorState state, int level) {
    switch (state) {
        case CursorState::Working: return Oklch{0.760f, 0.160f, 62.0f};
        case CursorState::Busy: {
            const float f = static_cast<float>(std::clamp(level, 0, kBreathLevels - 1)) / (kBreathLevels - 1);
            return Oklch{kBusyDim.l + (kBusyBright.l - kBusyDim.l) * f, kBusyDim.c, kBusyDim.h};
        }
        case CursorState::Idle:
            return kIdle;
        case CursorState::Down:
            return kDown;
        case CursorState::Lost:
            return kLost;
        case CursorState::None:
            break;
    }
    return Oklch{};
}

} // namespace nexus
