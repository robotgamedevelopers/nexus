// Functional tests of the fake driver (fake_driver.cpp) as a real child process over pipes (Prax's lib/process), read
// with the protocol's reader: a scripted exchange with stops, the protocol rules the fake keeps (turn order, a send
// while a turn is open, a bad line from the server), hang, the options that break those rules, and what the server's
// tests will script (a cut last line, a flood, recorded arguments and environment, a count of starts, standard error,
// a signal, a driver that ignores the end of its input). Real processes, a real clock and files in the test's own
// working directory: not unit tests.
//
//   driver_fake_test <path of nexus-fake-driver>            runs every case
//   driver_fake_test <path of nexus-fake-driver> <case>     runs one

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "driver/protocol.h"
#include "pr/process.h"

namespace {

using namespace nexus::driver;

int failures = 0;
std::string program;

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

// The fake driver as a child, its output read as driver messages and its standard error kept as text.
class Driver {
public:
    explicit Driver(const std::string& script, const std::string& cwd = ".") {
        launcher_ = prProcessLauncherCreate(PrProcessBackend_Default);
        const std::vector<std::string> args = {"--cwd", cwd, "--resume", "s-0", "--script", script};
        std::vector<PrStringView> views;
        for (const std::string& a : args)
            views.push_back(pr(a));
        PrProcessDesc desc{};
        desc.program = pr(program);
        desc.arguments = views.data();
        desc.argumentCount = uint32_t(views.size());
        desc.streams = PrProcessStreams_Pipes;
        desc.errors = PrProcessErrors_Pipe;
        desc.wake = &Waker::wake;
        desc.wakeContext = &waker_;
        if (launcher_ == nullptr || prProcessLaunch(launcher_, &desc, &process_) != PrStatus_Ok)
            std::fprintf(stderr, "cannot start %s; build the nexus_fake_driver target\n", program.c_str());
        CHECK(process_ != nullptr);
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
        write(e.line);
    }
    void write(std::string_view bytes) {
        prProcessWrite(process_, PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    }
    void closeInput() { prProcessCloseInput(process_); }

    // Reads until `count` items have arrived in all, or the output ends, or `patience` runs out.
    const std::vector<DriverReader::Item>& readUntil(std::size_t count,
        std::chrono::milliseconds patience = kPatience) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (items_.size() < count && !ended_ && std::chrono::steady_clock::now() < deadline) {
            drain();
            if (items_.size() < count && !ended_)
                waker_.wait();
        }
        return items_;
    }

    // Reads to the end of the output and waits up to `patience` for the exit.
    PrProcessState end(std::chrono::milliseconds patience = kPatience) {
        readUntil(SIZE_MAX, patience);
        const auto deadline = std::chrono::steady_clock::now() + patience;
        PrProcessState info = prProcessState(process_);
        while (!info.exited && std::chrono::steady_clock::now() < deadline) {
            waker_.wait();
            info = prProcessState(process_);
        }
        drain();
        return info;
    }

    // The status it exited with, or -1 if it did not exit by itself.
    int exitStatus() {
        const PrProcessState info = end();
        return info.exited && !info.signaled ? info.code : -1;
    }

    const std::string& errors() const { return errors_; }

private:
    void drain() {
        uint8_t buffer[4096];
        size_t got = 0;
        while ((got = prProcessRead(process_, PrProcessStream_Output, buffer, sizeof buffer)) > 0)
            reader_.feed(std::string_view(reinterpret_cast<const char*>(buffer), got), items_);
        while ((got = prProcessRead(process_, PrProcessStream_Error, buffer, sizeof buffer)) > 0)
            errors_.append(reinterpret_cast<const char*>(buffer), got);
        if (!ended_ && prProcessState(process_).outputEnded) {
            reader_.finish(items_);
            ended_ = true;
        }
    }

    Waker waker_;
    PrProcessLauncher* launcher_ = nullptr;
    PrProcess* process_ = nullptr;
    DriverReader reader_;
    std::vector<DriverReader::Item> items_;
    std::string errors_;
    bool ended_ = false;
};

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream(path) << text;
}

std::string readFile(const std::string& path) {
    std::ostringstream s;
    s << std::ifstream(path).rdbuf();
    return s.str();
}

bool is(const DriverReader::Item& item, const DriverMessage& m) {
    if (!item.message)
        std::fprintf(stderr, "  got an error: %s\n", item.error.c_str());
    return item.message && *item.message == m;
}

bool isError(const DriverReader::Item& item, std::optional<std::uint64_t> turn, ErrorCode code,
    std::string_view cause) {
    const Error* e = item.message ? std::get_if<Error>(&*item.message) : nullptr;
    if (e == nullptr || e->turn != turn || e->code != code || e->cause.find(cause) == std::string::npos) {
        std::fprintf(stderr, "  wanted an error with \"%.*s\", got %s\n", int(cause.size()), cause.data(),
            item.message ? encode(*item.message).line.c_str() : item.error.c_str());
        return false;
    }
    return true;
}

const char* kHello = "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n";

// Start, two turns (the second stopped mid-turn, after a stop of another turn that it ignores), a stop with no turn
// open, an unknown kind, the end of input. A summary that says "turn" is not taken for a turn.
void exchange() {
    writeFile("exchange.script", std::string("# start\n") + kHello +
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n"
        "emit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "send\n"
        "emit {\"kind\":\"text\",\"delta\":\"Hel\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"lo.\"}\n"
        "emit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"tool\",\"name\":\"Read\",\"summary\":\"turn\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\",\"session_cost_usd\":0.02,\"usage\":{\"input_tokens\":12}}\n"
        "send\n"
        "emit {\"kind\":\"text\",\"delta\":\"Work\"}\n"
        "await-stop\n"
        "emit {\"kind\":\"text\",\"delta\":\"ing\"}\n"
        "emit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"stopped\",\"session_cost_usd\":0.03}\n");
    Driver d("exchange.script");
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
    CHECK(is(items[6], Tool{1, "Read", "turn"}));
    CHECK(is(items[7], Done{1, Outcome::Ok, 0.02, Usage{12, {}, {}, {}}}));

    // The second turn: once its first text has arrived, stops of an earlier and a later turn, which get nothing for a
    // while, then the stop of turn 2.
    d.send(Send{2, "work on it"});
    d.readUntil(9);
    CHECK(items.size() == 9 && is(items[8], Text{2, "Work"}));
    d.send(Stop{1});
    d.send(Stop{3});
    d.readUntil(10, std::chrono::milliseconds(300));
    CHECK(items.size() == 9);
    d.send(Stop{2});
    d.readUntil(12);
    CHECK(items.size() == 12);
    if (items.size() != 12)
        return;
    CHECK(is(items[9], Text{2, "ing"}));
    CHECK(is(items[10], TextEnd{2}));
    CHECK(is(items[11], Done{2, Outcome::Stopped, 0.03, std::nullopt}));

    // A stop with no turn open and an unknown kind are answered by nothing; the end of input ends the driver.
    d.send(Stop{2});
    d.write("{\"kind\":\"pause\"}\n");
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    CHECK(items.size() == 12);
}

// A line the driver refuses, of any kind: one error of code protocol, with the line's turn if it has a readable one,
// then exit 3.
void refused() {
    writeFile("refused.script", kHello);
    const std::pair<const char*, std::optional<std::uint64_t>> lines[] = {{"not a message\n", std::nullopt},
        {"{\"kind\":\"send\",\"turn\":7}\n", 7}, {"{\"kind\":\"stop\"}\n", std::nullopt},
        {"{\"kind\":\"send\",\"turn\":0,\"prompt\":\"x\"}\n", std::nullopt}};
    for (const auto& [line, turn] : lines) {
        Driver d("refused.script");
        if (!d.started())
            return;
        d.write(line);
        CHECK(d.exitStatus() == 3);
        const std::vector<DriverReader::Item>& items = d.readUntil(3);
        CHECK(items.size() == 2 && isError(items[1], turn, ErrorCode::Protocol, "line 1: "));
    }
}

// raw writes its line exactly, a malformed one here, which the reader refuses and reads past.
void raw() {
    writeFile("raw.script", std::string(kHello) + "raw {\"kind\":\"text\",\"turn\":\n"
        "emit {\"kind\":\"session\",\"id\":\"s-9\"}\nexit 0\n");
    Driver d("raw.script");
    if (!d.started())
        return;
    CHECK(d.exitStatus() == 0);
    const std::vector<DriverReader::Item>& items = d.readUntil(4);
    CHECK(items.size() == 3);
    if (items.size() == 3) {
        CHECK(!items[1].message && items[1].error.find("line 2: the line is not JSON") != std::string::npos);
        CHECK(is(items[2], Session{"s-9"}));
    }
}

// A send the script has no block for: an error with its turn, then done outcome error.
void noBlock() {
    writeFile("noblock.script", kHello);
    Driver d("noblock.script");
    if (!d.started())
        return;
    d.send(Send{4, "go"});
    d.readUntil(3);
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    const std::vector<DriverReader::Item>& items = d.readUntil(4);
    CHECK(items.size() == 3);
    if (items.size() == 3) {
        CHECK(isError(items[1], 4, ErrorCode::Other, "no reply for send 1"));
        CHECK(is(items[2], Done{4, Outcome::Error, std::nullopt, std::nullopt}));
    }
}

// ignore-sigterm: a SIGTERM does not end it.
void ignoreSigterm() {
    writeFile("sigterm.script", std::string(kHello) + "option ignore-sigterm\nsignal 15\n"
        "emit {\"kind\":\"session\",\"id\":\"s-7\"}\nexit 0\n");
    Driver d("sigterm.script");
    if (!d.started())
        return;
    CHECK(d.exitStatus() == 0);
    const std::vector<DriverReader::Item>& items = d.readUntil(3);
    CHECK(items.size() == 2 && is(items[1], Session{"s-7"}));
}

// A send whose turn does not follow the last: one error of code protocol with that send's turn, then exit 3.
void order() {
    writeFile("order.script", std::string(kHello) + "send\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n"
        "send\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    for (std::uint64_t again : {5, 4}) {
        Driver d("order.script");
        if (!d.started())
            return;
        d.send(Send{5, "first"});
        d.send(Send{again, "not after it"});
        CHECK(d.exitStatus() == 3);
        const std::vector<DriverReader::Item>& items = d.readUntil(3);
        CHECK(items.size() == 3);
        if (items.size() == 3) {
            CHECK(is(items[1], Done{5, Outcome::Ok, std::nullopt, std::nullopt}));
            CHECK(isError(items[2], again, ErrorCode::Protocol, "does not follow turn 5"));
        }
    }
}

// A send while a turn is open: the same.
void sendWhileOpen() {
    writeFile("open.script", std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"a\"}\nawait-stop\n"
        "emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n");
    Driver d("open.script");
    if (!d.started())
        return;
    d.send(Send{1, "go"});
    d.readUntil(2);
    d.send(Send{2, "too soon"});
    CHECK(d.exitStatus() == 3);
    const std::vector<DriverReader::Item>& items = d.readUntil(3);
    CHECK(items.size() == 3 && isError(items[2], 2, ErrorCode::Protocol, "while turn 1 is open"));
}

// hang: nothing more is written, whatever arrives, until the input ends.
void hang() {
    writeFile("hang.script", std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"a\"}\nhang\n"
        "emit {\"kind\":\"text-end\"}\n");
    Driver d("hang.script");
    if (!d.started())
        return;
    d.send(Send{1, "go"});
    d.readUntil(2);
    d.send(Stop{1});
    d.send(Send{2, "more"});
    d.write("not a message\n");
    // Still running, not exited: a hang is not an exit.
    CHECK(!d.end(std::chrono::milliseconds(300)).exited);
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    CHECK(d.readUntil(3).size() == 2);
}

// The options that break the rules, for the server's tests of what it does about a broken driver.
void options() {
    writeFile("options.script", std::string(kHello) + "option stop-any-turn\noption accept-bad-input\n"
        "option done-without-cause\n"
        "send\nawait-stop\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "send\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    Driver d("options.script");
    if (!d.started())
        return;
    d.send(Send{4, "go"});
    d.send(Send{5, "while open: dropped"});
    d.write("not a message\n");
    d.send(Stop{9});
    d.send(Send{2, "out of order: answered"});
    d.send(Send{6, "no block left"});
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    const std::vector<DriverReader::Item>& items = d.readUntil(8);
    CHECK(items.size() == 7);
    if (items.size() == 7) {
        CHECK(isError(items[1], 5, ErrorCode::Protocol, "while turn 4 is open"));
        CHECK(isError(items[2], std::nullopt, ErrorCode::Protocol, "line 3: the line is not JSON"));
        CHECK(is(items[3], Done{4, Outcome::Stopped, std::nullopt, std::nullopt}));
        CHECK(isError(items[4], 2, ErrorCode::Protocol, "does not follow turn 4"));
        CHECK(is(items[5], Done{2, Outcome::Ok, std::nullopt, std::nullopt}));
        CHECK(is(items[6], Done{6, Outcome::Error, std::nullopt, std::nullopt}));
    }
}

// What the server's tests script, in a directory of its own given as --cwd: the first start fails, the second records
// its arguments, directory and environment, floods more than a pipe holds (so the fake waits on its reader), writes to
// standard error and ends with a last line cut short; the third ends by a signal.
void starts() {
    constexpr std::size_t kFlood = 30000;     // about 1 MB of lines
    std::filesystem::remove_all("starts.dir");
    std::filesystem::create_directory("starts.dir");
    ::setenv("NEXUS_FAKE_PROBE", "42", 1);
    writeFile("starts.script",
        "count-starts starts.count\n"
        "record starts.record NEXUS_FAKE_PROBE NEXUS_FAKE_ABSENT\n"
        "on-start 1 stderr the first start fails\n"
        "on-start 1 exit 4\n" +
        std::string(kHello) +
        "on-start 3 signal 15\n"
        "flood " + std::to_string(kFlood) + " {\"kind\":\"session\",\"id\":\"s-2\"}\n"
        "stderr written to standard error\n"
        "tail {\"kind\":\"session\",\"id\":\"s-3\"}\n"
        "exit 0\n");
    {
        Driver d("starts.script", "starts.dir");
        CHECK(d.exitStatus() == 4);
        CHECK(d.readUntil(1).empty());
        CHECK(d.errors() == "the first start fails\n");
    }
    {
        Driver d("starts.script", "starts.dir");
        CHECK(d.exitStatus() == 0);
        const std::vector<DriverReader::Item>& items = d.readUntil(kFlood + 3);
        CHECK(items.size() == kFlood + 2);
        if (items.size() == kFlood + 2) {
            CHECK(is(items[0], Hello{1, "fake"}));
            CHECK(is(items[kFlood], Session{"s-2"}));
            CHECK(!items[kFlood + 1].message && items[kFlood + 1].error.find("ended inside line " +
                std::to_string(kFlood + 2)) != std::string::npos);
        }
        CHECK(d.errors() == "written to standard error\n");
    }
    {
        Driver d("starts.script", "starts.dir");
        const PrProcessState info = d.end();
        CHECK(info.exited && info.signaled && info.code == 15);
    }
    const std::string once = "cwd=starts.dir\ndir=" + std::filesystem::canonical("starts.dir").string() +
        "\nresume=s-0\nNEXUS_FAKE_PROBE=42\nNEXUS_FAKE_ABSENT unset\n";
    CHECK(readFile("starts.dir/starts.record") == once + once + once);
    CHECK(readFile("starts.dir/starts.count") == "3\n");
}

// ignore-eof: the end of input does not end it; only a kill does.
void ignoreEof() {
    writeFile("eof.script", std::string(kHello) + "option ignore-eof\n");
    Driver d("eof.script");
    if (!d.started())
        return;
    d.readUntil(1);
    d.closeInput();
    const PrProcessState info = d.end(std::chrono::milliseconds(500));
    CHECK(!info.exited);
}

struct Case {
    const char* name;
    void (*run)();
};

const Case kCases[] = {
    {"exchange", exchange},
    {"refused", refused},
    {"raw", raw},
    {"no_block", noBlock},
    {"ignore_sigterm", ignoreSigterm},
    {"order", order},
    {"send_while_open", sendWhileOpen},
    {"hang", hang},
    {"options", options},
    {"starts", starts},
    {"ignore_eof", ignoreEof},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: driver_fake_test <path of nexus-fake-driver> [case]\n");
        return 2;
    }
    program = argv[1];
    bool ran = false;
    for (const Case& c : kCases) {
        if (argc == 3 && std::strcmp(argv[2], c.name) != 0)
            continue;
        c.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "driver_fake_test: no case %s\n", argv[2]);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "driver_fake_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("driver_fake_test: ok\n");
    return 0;
}
