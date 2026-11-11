#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nexus {

bool coverScreen(const PrAppSurface& surface) {
    if (surface.kind != PrAppSurfaceKind_Hwnd) {
        return false;
    }
    HWND window = static_cast<HWND>(surface.handle);
    if (window == nullptr || IsWindow(window) == FALSE) {
        return false;
    }
    POINT pointer{};
    HMONITOR monitor = GetCursorPos(&pointer) != FALSE ? MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY)
                                                       : MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        return false;
    }
    // rcMonitor is the display's whole frame, taskbar included (rcWork is the area Prax holds an overlay to).
    const RECT& frame = info.rcMonitor;
    return SetWindowPos(window, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
               SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

} // namespace nexus
