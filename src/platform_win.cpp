#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace nexus {

namespace {

std::string gErrorLog;  // the log's path in UTF-8, once startErrorLog has opened it

std::wstring wide(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(std::size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    }
    return out;
}

std::string narrow(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    }
    return out;
}

} // namespace

std::string startErrorLog(const std::string& home) {
    // A redirected stderr (nexus.exe 2> file, or a test harness's pipe) is kept.
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        return {};
    }
    if (home.empty()) {
        return {};
    }
    const std::wstring path = wide(home) + L"\\nexus.log";
    FILE* file = nullptr;
    if (_wfreopen_s(&file, path.c_str(), L"a", stderr) != 0 || file == nullptr) {
        return {};
    }
    setvbuf(stderr, nullptr, _IONBF, 0);
    gErrorLog = narrow(path);
    return gErrorLog;
}

void showLaunchError(const std::string& text) {
    std::string shown = text;
    if (!gErrorLog.empty()) {
        shown += "\n\nnexus writes its errors to " + gErrorLog + ".";
    }
    MessageBoxW(nullptr, wide(shown).c_str(), L"nexus", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

void protectOverlayFocus(const PrSurface& surface, bool enabled, bool restore) {
    (void)restore; // Windows blocks the request before focus changes; deliberate OS switching remains available.
    static HWND locked = nullptr;
    if (surface.kind != PrSurfaceKind_Hwnd) return;
    const HWND window = static_cast<HWND>(surface.handle);
    const bool foreground = window && IsWindowVisible(window) && GetForegroundWindow() == window;
    const bool switching = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if ((!enabled || !foreground || switching) && locked) {
        LockSetForegroundWindow(LSFW_UNLOCK);
        locked = nullptr;
    }
    if (enabled && foreground && !switching && locked != window && LockSetForegroundWindow(LSFW_LOCK))
        locked = window;
}

bool coverScreen(const PrSurface& surface) {
    if (surface.kind != PrSurfaceKind_Hwnd) {
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
