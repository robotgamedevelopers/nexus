// The client lock's system side on Windows (client_lock_platform.h): Windows Hello through UserConsentVerifier, asked
// over the nexus window (IUserConsentVerifierInterop, so the prompt belongs to it), and the session and display notices
// Prax does not report, on a hidden window of this file's own on the window's thread.

#include "client_lock_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>

#include <UserConsentVerifierInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace nexus {

namespace {

namespace ui = winrt::Windows::Security::Credentials::UI;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

// The codes lockAnswerForConsent and lockReasonForSessionChange (client_lock.h) name, as this SDK defines them.
static_assert(kConsentVerified == int(ui::UserConsentVerificationResult::Verified));
static_assert(kConsentDeviceNotPresent == int(ui::UserConsentVerificationResult::DeviceNotPresent));
static_assert(kConsentNotConfiguredForUser == int(ui::UserConsentVerificationResult::NotConfiguredForUser));
static_assert(kConsentDisabledByPolicy == int(ui::UserConsentVerificationResult::DisabledByPolicy));
static_assert(kConsentDeviceBusy == int(ui::UserConsentVerificationResult::DeviceBusy));
static_assert(kConsentRetriesExhausted == int(ui::UserConsentVerificationResult::RetriesExhausted));
static_assert(kConsentCanceled == int(ui::UserConsentVerificationResult::Canceled));
static_assert(kWtsConsoleDisconnect == static_cast<unsigned long>(WTS_CONSOLE_DISCONNECT));
static_assert(kWtsRemoteDisconnect == static_cast<unsigned long>(WTS_REMOTE_DISCONNECT));
static_assert(kWtsSessionLogoff == static_cast<unsigned long>(WTS_SESSION_LOGOFF));
static_assert(kWtsSessionLock == static_cast<unsigned long>(WTS_SESSION_LOCK));

std::string narrow(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

class WinAuthenticator final : public LockAuthenticator {
public:
    std::string method() const override { return "Windows Hello"; }

    void request(std::uint64_t attempt, const PrSurface& surface, LockPost post) override {
        const auto unavailable = [&](const std::string& why) {
            post({LockNotice::Kind::Answer, attempt, LockAnswer::Unavailable, LockReason::None, why});
        };
        HWND window = surface.kind == PrSurfaceKind_Hwnd ? static_cast<HWND>(surface.handle) : nullptr;
        if (window == nullptr) {
            unavailable("no window to ask over");
            return;
        }
        // The window's thread has COM already (Prax), in some apartment; this asks for nothing more than that.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comStarted = SUCCEEDED(com);
        try {
            auto interop = winrt::get_activation_factory<ui::UserConsentVerifier, IUserConsentVerifierInterop>();
            IAsyncOperation<ui::UserConsentVerificationResult> op{nullptr};
            const winrt::hstring message = L"unlock nexus";
            winrt::check_hresult(interop->RequestVerificationForWindowAsync(window,
                static_cast<HSTRING>(winrt::get_abi(message)),
                winrt::guid_of<IAsyncOperation<ui::UserConsentVerificationResult>>(), winrt::put_abi(op)));
            {
                std::lock_guard<std::mutex> hold(mutex_);
                if (op_) op_.Cancel();
                op_ = op;
            }
            op.Completed([attempt, post](IAsyncOperation<ui::UserConsentVerificationResult> const& done,
                                         AsyncStatus status) {
                if (status == AsyncStatus::Completed) {
                    const int result = int(done.GetResults());
                    post({LockNotice::Kind::Answer, attempt, lockAnswerForConsent(result), LockReason::None,
                          result == kConsentVerified ? std::string() : "result " + std::to_string(result)});
                } else if (status == AsyncStatus::Canceled) {
                    post({LockNotice::Kind::Answer, attempt, LockAnswer::Cancelled, LockReason::None, {}});
                } else {
                    char code[16];
                    std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(int32_t(done.ErrorCode())));
                    post({LockNotice::Kind::Answer, attempt, LockAnswer::Failed, LockReason::None,
                          std::string("error ") + code});
                }
            });
        } catch (const winrt::hresult_error& e) {
            unavailable(narrow(std::wstring(e.message())));
        }
        if (comStarted) CoUninitialize();
    }

    void cancel() override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (op_) op_.Cancel();
        op_ = nullptr;
    }

private:
    std::mutex mutex_;
    IAsyncOperation<ui::UserConsentVerificationResult> op_{nullptr};
};

constexpr wchar_t kEventsClass[] = L"nexus-lock-events";

// A hidden top-level window (a message-only one gets no broadcasts) that hears the session's and the display's notices.
class WinEvents final : public LockEvents {
public:
    explicit WinEvents(LockPost post) : post_(std::move(post)) {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = &WinEvents::proc;
        wc.hInstance = instance;
        wc.lpszClassName = kEventsClass;
        RegisterClassW(&wc);    // a second registration fails harmlessly: the class is the same
        window_ = CreateWindowExW(0, kEventsClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (window_ == nullptr) {
            std::fprintf(stderr, "nexus: the lock's session watch could not start (CreateWindowExW error %lu); the "
                "lock still locks at sleep and at the screen's lock, through Prax\n", GetLastError());
            return;
        }
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (!WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION))
            std::fprintf(stderr, "nexus: the lock cannot hear session switches (WTSRegisterSessionNotification error "
                "%lu)\n", GetLastError());
        display_ = RegisterPowerSettingNotification(window_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (display_ == nullptr)
            std::fprintf(stderr, "nexus: the lock cannot hear the display turning off (RegisterPowerSettingNotification "
                "error %lu)\n", GetLastError());
    }

    ~WinEvents() override {
        if (display_ != nullptr) UnregisterPowerSettingNotification(display_);
        if (window_ != nullptr) {
            WTSUnRegisterSessionNotification(window_);
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            DestroyWindow(window_);
        }
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<WinEvents*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            if (message == WM_WTSSESSION_CHANGE) {
                const LockReason why = lockReasonForSessionChange(static_cast<unsigned long>(wParam));
                if (why != LockReason::None) self->relock(why);
                return 0;
            }
            if (message == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE && lParam != 0) {
                const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                // GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed. Off locks (modern standby, the lid).
                if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) &&
                        setting->DataLength >= sizeof(DWORD) &&
                        *reinterpret_cast<const DWORD*>(setting->Data) == 0)
                    self->relock(LockReason::Sleep);
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void relock(LockReason why) { post_({LockNotice::Kind::Relock, 0, LockAnswer::Failed, why, {}}); }

    LockPost post_;
    HWND window_ = nullptr;
    HPOWERNOTIFY display_ = nullptr;
};

} // namespace

#ifndef NEXUS_TEST_AUTHENTICATOR
std::unique_ptr<LockAuthenticator> systemLockAuthenticator() {
    return std::make_unique<WinAuthenticator>();
}
#endif

std::unique_ptr<LockEvents> watchLockEvents(LockPost post) {
    return std::make_unique<WinEvents>(std::move(post));
}

} // namespace nexus
