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

std::string startErrorLog() {
    // A redirected stderr (nexus.exe 2> file, or a test harness's pipe) is kept.
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        return {};
    }
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    const std::wstring dir = std::wstring(local) + L"\\nexus";
    CreateDirectoryW(dir.c_str(), nullptr);    // ERROR_ALREADY_EXISTS is fine; a failure shows at the open below
    const std::wstring path = dir + L"\\nexus.log";
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
