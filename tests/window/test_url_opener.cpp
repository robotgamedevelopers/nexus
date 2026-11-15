// Functional tests of the window's link opener (src/url_opener.h) over a stub browser (no link is opened): real
// threads and real time, so functional, never unit.
//
//   ordered   links are opened one at a time, in order, off the caller's thread
//   stuck     a browser call that does not return keeps the opener's end waiting 2 s at most (a window still closes),
//             and the call, once it returns, ends its thread on its own

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "url_opener.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

using Clock = std::chrono::steady_clock;

void ordered() {
    struct Seen {
        std::mutex mutex;
        std::vector<std::string> urls;
        std::thread::id thread;
    };
    auto seen = std::make_shared<Seen>();
    {
        UrlOpener urls([seen](const std::string& url) {
            std::lock_guard<std::mutex> lock(seen->mutex);
            seen->urls.push_back(url);
            seen->thread = std::this_thread::get_id();
        });
        urls.open("https://a.example/1");
        urls.open("https://a.example/2");
        const Clock::time_point until = Clock::now() + std::chrono::seconds(5);
        while (urls.opened() < 2 && Clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(urls.opened() == 2);
    }
    std::lock_guard<std::mutex> lock(seen->mutex);
    CHECK((seen->urls == std::vector<std::string>{"https://a.example/1", "https://a.example/2"}));
    CHECK(seen->thread != std::this_thread::get_id());
}

void stuck() {
    // The browser call blocks until the test lets it go; everything it touches is shared, so that it outlives the
    // opener.
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        std::atomic<bool> returned{false};
    };
    auto gate = std::make_shared<Gate>();
    Clock::time_point closed;
    const Clock::time_point start = Clock::now();
    {
        UrlOpener urls([gate](const std::string&) {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [&] { return gate->release; });
            gate->returned = true;
        });
        urls.open("https://stuck.example/");
        std::unique_lock<std::mutex> lock(gate->mutex);
        CHECK(gate->cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }));
    }
    closed = Clock::now();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(closed - start).count();
    CHECK(took >= 1500 && took < 4000);     // the 2 s wait, not the stuck call's
    CHECK(!gate->returned);
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    const Clock::time_point until = Clock::now() + std::chrono::seconds(5);
    while (!gate->returned && Clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(gate->returned);
    // A beat for the detached thread to leave serve() (it holds its own share of the state).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

struct Case {
    const char* name;
    void (*run)();
};

const Case kCases[] = {{"ordered", ordered}, {"stuck", stuck}};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: nexus_url_opener_tests <case>\n");
        return 2;
    }
    for (const Case& c : kCases) {
        if (std::strcmp(argv[1], c.name) == 0) {
            c.run();
            if (failures != 0) {
                std::fprintf(stderr, "url_opener %s: %d check(s) failed\n", c.name, failures);
                return 1;
            }
            std::printf("url_opener %s: ok\n", c.name);
            return 0;
        }
    }
    std::fprintf(stderr, "url_opener: no case %s\n", argv[1]);
    return 2;
}
