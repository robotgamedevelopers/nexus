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
    std::deque<std::string> queue;
    std::size_t opened = 0;
    bool stop = false;
};

namespace {

void serve(const std::shared_ptr<UrlOpener::Shared>& shared, const UrlOpener::Open& open) {
    for (;;) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(shared->mutex);
            shared->wake.wait(lock, [&] { return shared->stop || !shared->queue.empty(); });
            if (shared->stop) {
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
    thread_.join();
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
        // COM for ShellExecuteW, once on this thread (the opener's), which is never the frame thread.
        thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        (void)com;
        const int n = MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), nullptr, 0);
        if (n <= 0) {
            std::fprintf(stderr, "nexus: a link of %zu bytes is not UTF-8 and was not opened\n", url.size());
            return;
        }
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), wide.data(), n);
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
