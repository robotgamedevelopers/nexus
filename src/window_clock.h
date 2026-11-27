// Host clock sampling stays outside the deterministic renderer and its tests.
#pragma once
#include <ctime>

namespace nexus {
struct WindowCalendar {
    int weekday = 0, day = 1, month = 1, year = 1970, hour = 0, minute = 0, second = 0;
    bool valid = false;
    bool operator==(const WindowCalendar&) const = default;
};
inline bool updateLocalCalendar(WindowCalendar& calendar) {
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) return false;
#else
    if (!localtime_r(&now, &local)) return false;
#endif
    const WindowCalendar next{local.tm_wday, local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
        local.tm_hour, local.tm_min, local.tm_sec, true};
    if (calendar == next) return false;
    calendar = next;
    return true;
}
}
