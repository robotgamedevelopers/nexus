#include "url_opener.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <utility>

#include "pr/app_shell.h"
#include "sync/secrets.h"

namespace nexus {

struct UrlOpener::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable ended;
    std::deque<std::string> queue;
    std::size_t opened = 0;
    bool stop = false;
    bool done = false;      // the opener's thread has returned from its last call and holds nothing of the opener's
};

namespace {

// How long closing waits for a system call still opening a link before it leaves the thread to finish alone: a guess,
// not a measurement (a browser's launch is well under it; a call stuck on a hung system service is not worth a window
// that will not close).
constexpr auto kCloseWait = std::chrono::seconds(2);

void serve(const std::shared_ptr<UrlOpener::Shared>& shared, const UrlOpener::Open& open) {
    for (;;) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(shared->mutex);
            shared->wake.wait(lock, [&] { return shared->stop || !shared->queue.empty(); });
            if (shared->stop) {
                shared->done = true;
                shared->ended.notify_all();
                return;
            }
            url = std::move(shared->queue.front());
            shared->queue.pop_front();
        }
        open(url);
        sync::wipeBytes(url);
        std::lock_guard<std::mutex> lock(shared->mutex);
        ++shared->opened;
    }
}

} // namespace

UrlOpener::UrlOpener(Open open) : shared_(std::make_shared<Shared>()) {
    thread_ = std::thread([shared = shared_, open = std::move(open)] { serve(shared, open); });
}

UrlOpener::~UrlOpener() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->stop = true;
        for (std::string& url : shared_->queue) {
            sync::wipeBytes(url);
        }
        shared_->queue.clear();
    }
    shared_->wake.notify_one();
    // A bounded wait: a system call that never returns does not keep the window from closing. The thread then ends on
    // its own when the call returns (it holds the shared state, never the opener).
    bool done = false;
    {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        done = shared_->ended.wait_for(lock, kCloseWait, [this] { return shared_->done; });
    }
    if (done) {
        thread_.join();
    } else {
        std::fprintf(stderr, "nexus: a link was still being opened when the window closed; left to finish\n");
        thread_.detach();
    }
}

void UrlOpener::open(std::string url) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->queue.push_back(std::move(url));
    }
    shared_->wake.notify_one();
    sync::wipeBytes(url);   // moved from: empty, unless the move copied
}

UrlOpener::Open UrlOpener::handle() {
    return [this](const std::string& url) { open(url); };
}

std::size_t UrlOpener::opened() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->opened;
}

UrlOpener::Open systemBrowser(PrApp* app) {
#if defined(_WIN32)
    (void)app;
    return [](const std::string& url) {
        // COM for ShellExecuteW, started once on this thread (the opener's, never the frame thread) and ended as the
        // thread ends: each CoInitializeEx that succeeded (S_OK or S_FALSE) is balanced by one CoUninitialize.
        struct Com {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            Com() = default;
            Com(const Com&) = delete;
            Com& operator=(const Com&) = delete;
            ~Com() {
                if (SUCCEEDED(hr)) {
                    CoUninitialize();
                }
            }
        };
        thread_local const Com com;
        (void)com;
        if (url.size() > static_cast<std::size_t>(INT_MAX)) {
            std::fprintf(stderr, "nexus: a link of %zu bytes is too long and was not opened\n", url.size());
            return;
        }
        // Bytes that are not UTF-8 fail the conversion (MB_ERR_INVALID_CHARS), rather than become U+FFFD in a link the
        // system would then open as something else.
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()),
                                          nullptr, 0);
        if (n <= 0) {
            std::fprintf(stderr, "nexus: a link of %zu bytes is not UTF-8 and was not opened\n", url.size());
            return;
        }
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), wide.data(),
                                n) != n) {
            SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
            std::fprintf(stderr, "nexus: a link of %zu bytes is not UTF-8 and was not opened\n", url.size());
            return;
        }
        const HINSTANCE rc = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
        if (reinterpret_cast<INT_PTR>(rc) <= 32) {
            std::fprintf(stderr, "nexus: the system could not open a link of %zu bytes (error %lld)\n", url.size(),
                         static_cast<long long>(reinterpret_cast<INT_PTR>(rc)));
        }
    };
#else
    return [app](const std::string& url) {
        if (app == nullptr) {
            return;
        }
        const PrStatus status = prAppOpenUrl(app, PrStringView{url.data(), url.size()});
        if (status != PrStatus_Ok) {
            std::fprintf(stderr, "nexus: the system could not open a link of %zu bytes (status %d)\n", url.size(),
                         static_cast<int>(status));
        }
    };
#endif
}

} // namespace nexus
