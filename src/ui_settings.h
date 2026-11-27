// Fork-owned UI defaults. CMake derives fallback values from the shipped profile.
#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace nexus {
struct UiMetrics {
#define NEXUS_UI_STYLE(key, value) float key = static_cast<float>(value);
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_STYLE
    int stepsDown() const { return int((1 - zoom_min) / zoom_step + 0.0001f); }
    int stepsUp() const { return int((zoom_max - 1) / zoom_step + 0.0001f); }
    int defaultSteps(bool mobile) const { return int(std::round(((mobile ? default_zoom_mobile : default_zoom_desktop) - 1) / zoom_step)); }
    float scale(int steps) const { return std::clamp(1 + steps * zoom_step, zoom_min, zoom_max); }
    bool operator==(const UiMetrics&) const = default;
};
struct UiColors {
    std::map<std::string, unsigned, std::less<>> palette;
#define NEXUS_UI_COLORS(key, value) unsigned key = value;
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_COLORS
    bool operator==(const UiColors&) const = default;
};
struct UiText {
#define NEXUS_UI_TEXT(key, value) std::string key = value;
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_TEXT
    bool operator==(const UiText&) const = default;
};
struct UiKeys {
#define NEXUS_UI_KEYS(key, value) std::string key = value;
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_KEYS
    bool operator==(const UiKeys&) const = default;
};
struct UiFlags {
#define NEXUS_UI_FLAGS(key, value) bool key = value;
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_FLAGS
    bool operator==(const UiFlags&) const = default;
};
struct UiFonts {
#define NEXUS_UI_FONTS(key, value) std::string key = value;
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_FONTS
    bool operator==(const UiFonts&) const = default;
};
struct UiScroll {
#define NEXUS_UI_SCROLL(key, value) double key = value;
#include "nexus_ui_defaults.inc"
#undef NEXUS_UI_SCROLL
    bool operator==(const UiScroll&) const = default;
};
// One-pass substitution never interprets braces in account names or other inserted text.
inline std::string uiFormat(std::string_view pattern,
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    std::string out;
    for (size_t pos = 0; pos < pattern.size();) {
        if (pattern[pos] == '{') {
            const auto end = pattern.find('}', pos + 1);
            bool found = false;
            for (const auto& [key, value] : fields) {
                if (end != pattern.npos && pattern.substr(pos + 1, end - pos - 1) == key) {
                    out += value; pos = end + 1; found = true; break;
                }
            }
            if (found) continue;
        }
        out += pattern[pos++];
    }
    return out;
}
}
