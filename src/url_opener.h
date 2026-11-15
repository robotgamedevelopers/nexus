// Opens links in the system's browser off the frame thread (frame_host.h LinkSignIn's sign-in page, board.h's links
// off the board). open() hands the URL to a thread of the opener's own and returns at once: the system's call (Launch
// Services on macOS, ShellExecute on Windows) may take its time, and never holds a frame. One at a time, in order; each
// URL is wiped from the opener's memory once handed to the system.
//
// Which call opens them is given (systemBrowser for the window; a test gives a stub), and is called on the opener's
// thread alone.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

struct PrApp;

namespace nexus {

class UrlOpener {
public:
    using Open = std::function<void(const std::string& url)>;

    explicit UrlOpener(Open open);
    // Drops what is still queued (wiped) and waits for the one being opened, if any.
    ~UrlOpener();
    UrlOpener(const UrlOpener&) = delete;
    UrlOpener& operator=(const UrlOpener&) = delete;

    // Queues `url` and returns at once.
    void open(std::string url);
    // open() as a function to hand on (LinkSignIn::openUrl, PraxBoard::Desc::open); valid while this opener lives.
    Open handle();
    // How many URLs the opener's thread has handed to the system so far.
    std::size_t opened() const;

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

// The system's browser for `app`, safe to call on any thread: on macOS Prax's prAppOpenUrl (NSWorkspace, which holds
// none of the app's state); on Windows ShellExecuteW over a UTF-16 copy of the URL of its own and COM started on the
// calling thread (Prax's Win32 prAppOpenUrl converts through a buffer the app shares, so it is the frame thread's
// alone); elsewhere prAppOpenUrl. A URL the system cannot open is said on stderr by its length alone, never its bytes.
UrlOpener::Open systemBrowser(PrApp* app);

} // namespace nexus
