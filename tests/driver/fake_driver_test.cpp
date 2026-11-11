// Functional tests of the fake driver (fake_driver.cpp) as a real child process over pipes (Prax's lib/process), read
// with the protocol's reader: one scripted exchange with a stop mid-turn, and a driver that writes a malformed line and
// exits mid-turn. Real processes, a real clock and files in the test's own working directory: not unit tests.
//
//   driver_fake_test <path of nexus-fake-driver>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "driver/protocol.h"
#include "pr/process.h"

namespace {

using namespace nexus::driver;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr auto kPatience = std::chrono::seconds(10);

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

// Wakes the reader when the process has output or has exited.
struct Waker {
    std::mutex mutex;
    std::condition_variable woken;
    bool signaled = false;

    static void wake(void* context) {
        Waker& w = *static_cast<Waker*>(context);
        {
            std::lock_guard<std::mutex> lock(w.mutex);
            w.signaled = true;
        }
        w.woken.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        woken.wait_for(lock, std::chrono::milliseconds(100), [this] { return signaled; });
        signaled = false;
    }
};

// The fake driver as a child, its output read as driver messages.
class Driver {
public:
    Driver(const std::string& program, const std::string& script) {
        launcher_ = prProcessLauncherCreate(PrProcessBackend_Default);
        const std::vector<std::string> args = {"--cwd", ".", "--resume", "s-0", "--script", script};
        std::vector<PrStringView> views;
        for (const std::string& a : args)
            views.push_back(pr(a));
        PrProcessDesc desc{};
        desc.program = pr(program);
        desc.arguments = views.data();
        desc.argumentCount = uint32_t(views.size());
        desc.streams = PrProcessStreams_Pipes;
        desc.errors = PrProcessErrors_Inherit;
        desc.wake = &Waker::wake;
        desc.wakeContext = &waker_;
        if (launcher_ == nullptr || prProcessLaunch(launcher_, &desc, &process_) != PrStatus_Ok)
            std::fprintf(stderr, "cannot start %s; build the nexus_fake_driver target\n", program.c_str());
    }
    ~Driver() {
        prProcessDestroy(process_);
        prProcessLauncherDestroy(launcher_);
    }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    bool started() const { return process_ != nullptr; }

    void send(const ServerMessage& m) {
        const Encoded e = encode(m);
        CHECK(e.error.empty());
        prProcessWrite(process_, PrByteView{reinterpret_cast<const uint8_t*>(e.line.data()), e.line.size()});
    }
    void closeInput() { prProcessCloseInput(process_); }

    // Reads until `count` items have arrived in all, or the output ends, or patience runs out.
    const std::vector<DriverReader::Item>& readUntil(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (items_.size() < count && !ended_ && std::chrono::steady_clock::now() < deadline) {
            drain();
            if (items_.size() < count && !ended_)
                waker_.wait();
        }
        return items_;
    }

    // Reads to the end of the output and waits for the exit; its status, or -1.
    int exitStatus() {
        readUntil(SIZE_MAX);
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        PrProcessInfo info = prProcessInfo(process_);
        while (!info.exited && std::chrono::steady_clock::now() < deadline) {
            waker_.wait();
            info = prProcessInfo(process_);
        }
        return info.exited && !info.signaled ? info.code : -1;
    }

private:
    void drain() {
        uint8_t buffer[4096];
        size_t got = 0;
        while ((got = prProcessRead(process_, PrProcessStream_Output, buffer, sizeof buffer)) > 0)
            reader_.feed(std::string_view(reinterpret_cast<const char*>(buffer), got), items_);
        if (prProcessInfo(process_).outputEnded) {
            reader_.finish(items_);
            ended_ = true;
        }
    }

    Waker waker_;
    PrProcessLauncher* launcher_ = nullptr;
    PrProcess* process_ = nullptr;
    DriverReader reader_;
    std::vector<DriverReader::Item> items_;
    bool ended_ = false;
};

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream(path) << text;
}

bool is(const DriverReader::Item& item, const DriverMessage& m) {
    if (!item.message)
        std::fprintf(stderr, "  got an error: %s\n", item.error.c_str());
    return item.message && *item.message == m;
}

// Start, two turns (the second stopped mid-turn), a stop with no turn open, an unknown kind, the end of input.
void exchange(const std::string& program) {
    writeFile("exchange.script",
        "# start\n"
        "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n"
        "emit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "send\n"
        "emit {\"kind\":\"text\",\"delta\":\"Hel\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"lo.\"}\n"
        "emit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"tool\",\"name\":\"Bash\",\"summary\":\"ls src\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\",\"session_cost_usd\":0.02,\"usage\":{\"input_tokens\":12}}\n"
        "send\n"
        "emit {\"kind\":\"text\",\"delta\":\"Work\"}\n"
        "await-stop\n"
        "emit {\"kind\":\"text\",\"delta\":\"ing\"}\n"
        "emit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"stopped\",\"session_cost_usd\":0.03}\n");
    Driver d(program, "exchange.script");
    CHECK(d.started());
    if (!d.started())
        return;
    const std::vector<DriverReader::Item>& items = d.readUntil(3);
    CHECK(items.size() == 3);
    if (items.size() != 3)
        return;
    CHECK(is(items[0], Hello{1, "fake"}));
    CHECK(is(items[1], Credential{Billing::Subscription, "max"}));
    CHECK(is(items[2], Session{"s-1"}));

    d.send(Send{1, "say hello"});
    d.readUntil(8);
    CHECK(items.size() == 8);
    if (items.size() != 8)
        return;
    CHECK(is(items[3], Text{1, "Hel"}));
    CHECK(is(items[4], Text{1, "lo."}));
    CHECK(is(items[5], TextEnd{1}));
    CHECK(is(items[6], Tool{1, "Bash", "ls src"}));
    CHECK(is(items[7], Done{1, Outcome::Ok, 0.02, Usage{12, {}, {}, {}}}));

    // The second turn: stop it once its first text has arrived.
    d.send(Send{2, "work on it"});
    d.readUntil(9);
    CHECK(items.size() == 9 && is(items[8], Text{2, "Work"}));
    d.send(Stop{});
    d.readUntil(12);
    CHECK(items.size() == 12);
    if (items.size() != 12)
        return;
    CHECK(is(items[9], Text{2, "ing"}));
    CHECK(is(items[10], TextEnd{2}));
    CHECK(is(items[11], Done{2, Outcome::Stopped, 0.03, std::nullopt}));

    // A stop with no turn open and an unknown kind are answered by nothing; the end of input ends the driver.
    d.send(Stop{});
    d.send(Unknown{"pause"});
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    CHECK(items.size() == 12);
}

// A driver that writes a malformed line and exits mid-turn: the reader reports the line with its cause and goes on.
void crash(const std::string& program) {
    writeFile("crash.script",
        "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n"
        "send\n"
        "emit {\"kind\":\"text\",\"delta\":\"par\"}\n"
        "raw {\"kind\":\"text\",\"turn\":\n"
        "emit {\"kind\":\"text\",\"delta\":\"tial\"}\n"
        "exit 3\n");
    Driver d(program, "crash.script");
    CHECK(d.started());
    if (!d.started())
        return;
    d.send(Send{7, "go"});
    CHECK(d.exitStatus() == 3);
    const std::vector<DriverReader::Item>& items = d.readUntil(4);
    CHECK(items.size() == 4);
    if (items.size() != 4)
        return;
    CHECK(is(items[0], Hello{1, "fake"}));
    CHECK(is(items[1], Text{7, "par"}));
    CHECK(!items[2].message && items[2].error.find("line 3: the line is not JSON") != std::string::npos);
    CHECK(is(items[3], Text{7, "tial"}));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: driver_fake_test <path of nexus-fake-driver>\n");
        return 2;
    }
    exchange(argv[1]);
    crash(argv[1]);
    if (failures != 0) {
        std::fprintf(stderr, "driver_fake_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("driver_fake_test: ok\n");
    return 0;
}
