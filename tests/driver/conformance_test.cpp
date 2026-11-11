// Protocol conformance of any driver (docs/driver-protocol.md), run against the program itself as a child process over
// pipes, its output read with the reference reader (src/driver/protocol.h). The same cases run against the fake driver
// (which proves the cases) and against each real driver over a scripted harness, so a real driver keeps the rules the
// server's tests rely on: hello first; turns answered with well-formed text blocks and exactly one done, nothing of a
// turn after its done; a stop of the open turn answered with done stopped, any other stop and any unknown kind
// ignored; settings taken or skipped; every line it cannot accept refused with one protocol error and an exit status
// other than 0; and the end of its input ending it with status 0, owing no done. Real processes and a real clock: a
// functional test.
//
//   driver_conformance_test <case> fake <path of nexus-fake-driver>
//   driver_conformance_test <case> prompted <program> [<argument>...]
//
// A prompted driver answers the prompt "reply" with a text block and done ok, and the prompt "wait" with a text block
// left open until the turn's stop, then done stopped (drivers/claude/test/fake-main.ts does this over a scripted
// query). For the fake, the test writes the script that does the same.

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "driver/protocol.h"

extern char** environ;

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

constexpr auto kPatience = std::chrono::seconds(20);

// The driver as a child: its standard input and output as pipes, its standard error passed through to ours.
class Child {
public:
    explicit Child(const std::vector<std::string>& command) {
        int in[2] = {-1, -1};
        int out[2] = {-1, -1};
        if (::pipe(in) != 0 || ::pipe(out) != 0) {
            std::perror("pipe");
            return;
        }
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_adddup2(&files, in[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&files, out[1], STDOUT_FILENO);
        for (int fd : {in[0], in[1], out[0], out[1]})
            posix_spawn_file_actions_addclose(&files, fd);
        std::vector<char*> argv;
        for (const std::string& a : command)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = posix_spawnp(&pid_, argv[0], &files, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&files);
        ::close(in[0]);
        ::close(out[1]);
        in_ = in[1];
        out_ = out[0];
        if (rc != 0) {
            std::fprintf(stderr, "cannot start %s: %s\n", argv[0], std::strerror(rc));
            pid_ = -1;
        }
    }
    ~Child() {
        closeInput();
        if (out_ >= 0)
            ::close(out_);
        if (pid_ > 0 && !status_) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    bool started() const { return pid_ > 0; }

    void send(const ServerMessage& m) {
        const Encoded e = encode(m);
        CHECK(e.error.empty());
        write(e.line);
    }
    void write(std::string_view bytes) {
        while (!bytes.empty() && in_ >= 0) {
            const ssize_t n = ::write(in_, bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return;     // the driver has gone; what it wrote says why
            bytes.remove_prefix(std::size_t(n));
        }
    }
    void closeInput() {
        if (in_ >= 0)
            ::close(in_);
        in_ = -1;
    }

    // Reads until `done(items)` holds, the output ends, or patience runs out. True when `done` held.
    template <class Done>
    bool readUntil(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (!done(items_)) {
            if (ended_ || std::chrono::steady_clock::now() >= deadline)
                return false;
            pollfd p{out_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0)
                continue;
            char buffer[4096];
            const ssize_t n = ::read(out_, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                reader_.finish(items_);
                ended_ = true;
            } else {
                reader_.feed(std::string_view(buffer, std::size_t(n)), items_);
            }
        }
        return true;
    }

    // Reads to the end of the output and waits for the exit: its status, or -1 if it did not exit by itself in time.
    int exitStatus() {
        readUntil([](const std::vector<DriverReader::Item>&) { return false; });
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (!status_ && std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const pid_t got = ::waitpid(pid_, &status, WNOHANG);
            if (got == pid_)
                status_ = status;
            else
                ::usleep(10000);
        }
        if (!status_ || !WIFEXITED(*status_))
            return -1;
        return WEXITSTATUS(*status_);
    }

    const std::vector<DriverReader::Item>& items() const { return items_; }

private:
    pid_t pid_ = -1;
    int in_ = -1;
    int out_ = -1;
    DriverReader reader_;
    std::vector<DriverReader::Item> items_;
    bool ended_ = false;
    std::optional<int> status_;
};

std::optional<std::uint64_t> turnOf(const DriverMessage& m) {
    return std::visit([](const auto& v) -> std::optional<std::uint64_t> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Text> || std::is_same_v<T, TextEnd> || std::is_same_v<T, Tool> ||
            std::is_same_v<T, Done>)
            return v.turn;
        else if constexpr (std::is_same_v<T, Error>)
            return v.turn;
        else
            return std::nullopt;
    }, m);
}

// Whether `items` hold a message for which `pred` holds.
template <class T, class Pred>
bool has(const std::vector<DriverReader::Item>& items, Pred pred) {
    for (const DriverReader::Item& item : items)
        if (item.message)
            if (const T* m = std::get_if<T>(&*item.message); m != nullptr && pred(*m))
                return true;
    return false;
}

auto doneOf(std::uint64_t turn) {
    return [turn](const std::vector<DriverReader::Item>& items) {
        return has<Done>(items, [turn](const Done& d) { return d.turn == turn; });
    };
}

auto textOf(std::uint64_t turn) {
    return [turn](const std::vector<DriverReader::Item>& items) {
        return has<Text>(items, [turn](const Text& t) { return t.turn == turn; });
    };
}

auto errorOf(ErrorCode code) {
    return [code](const std::vector<DriverReader::Item>& items) {
        return has<Error>(items, [code](const Error& e) { return e.code == code; });
    };
}

// The rules every stream keeps: each line decodes; hello first; text blocks closed before a tool, a done or another
// block; one done per turn, its last message, and only for a turn the server sent; no error but a protocol refusal for
// a turn not open.
void checkStream(const std::vector<DriverReader::Item>& items, const std::set<std::uint64_t>& sent) {
    std::map<std::uint64_t, bool> closed;
    std::optional<std::uint64_t> textOpen;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const DriverReader::Item& item = items[i];
        if (!item.message) {
            std::fprintf(stderr, "  a line that does not decode: %s\n", item.error.c_str());
            ++failures;
            continue;
        }
        const DriverMessage& m = *item.message;
        if (i == 0) {
            const Hello* hello = std::get_if<Hello>(&m);
            CHECK(hello != nullptr && hello->protocol == kProtocolVersion && !hello->harness.empty());
            continue;
        }
        CHECK(!std::holds_alternative<Hello>(m));
        const std::optional<std::uint64_t> turn = turnOf(m);
        if (turn && closed[*turn]) {
            const Error* e = std::get_if<Error>(&m);
            if (e == nullptr || e->code != ErrorCode::Protocol) {
                std::fprintf(stderr, "  a message of turn %llu after its done: %s", (unsigned long long)*turn,
                    encode(m).line.c_str());
                ++failures;
            }
        }
        if (std::holds_alternative<Text>(m)) {
            CHECK(!textOpen || *textOpen == *turn);
            textOpen = turn;
        } else if (std::holds_alternative<TextEnd>(m)) {
            CHECK(textOpen && *textOpen == *turn);
            textOpen.reset();
        } else if (std::holds_alternative<Tool>(m) || std::holds_alternative<Done>(m)) {
            CHECK(!textOpen);
        }
        if (const Done* d = std::get_if<Done>(&m)) {
            CHECK(sent.contains(d->turn));
            closed[d->turn] = true;
        }
    }
}

const Done* lastDone(const std::vector<DriverReader::Item>& items, std::uint64_t turn) {
    const Done* found = nullptr;
    for (const DriverReader::Item& item : items)
        if (item.message)
            if (const Done* d = std::get_if<Done>(&*item.message); d != nullptr && d->turn == turn)
                found = d;
    return found;
}

std::size_t count(const std::vector<DriverReader::Item>& items, ErrorCode code) {
    std::size_t n = 0;
    for (const DriverReader::Item& item : items)
        if (item.message)
            if (const Error* e = std::get_if<Error>(&*item.message); e != nullptr && e->code == code)
                ++n;
    return n;
}

// ---- The cases: each the server's lines and what must come back ----

// Two turns: one answered, one stopped part way after a stop of another turn that it ignores; a stop of an ended turn,
// an unknown kind and a settings between them; then the end of input.
void exchange(Child& d) {
    CHECK(d.readUntil([](const auto& items) { return !items.empty(); }));
    d.send(Send{7, "reply"});
    CHECK(d.readUntil(doneOf(7)));
    const Done* first = lastDone(d.items(), 7);
    CHECK(first != nullptr && first->outcome == Outcome::Ok);
    d.send(Stop{7});
    d.write("{\"kind\":\"pause\",\"turn\":8}\n");
    d.send(Settings{"some-model", "low"});
    d.send(Send{9, "wait"});
    CHECK(d.readUntil(textOf(9)));
    d.send(Stop{8});
    d.send(Stop{9});
    CHECK(d.readUntil(doneOf(9)));
    const Done* second = lastDone(d.items(), 9);
    CHECK(second != nullptr && second->outcome == Outcome::Stopped);
    d.send(Stop{9});
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    checkStream(d.items(), {7, 9});
    CHECK(count(d.items(), ErrorCode::Protocol) == 0);
}

// A known kind with a field missing: one protocol error with the line's turn, then an exit other than 0.
void refused(Child& d) {
    d.write("{\"kind\":\"send\",\"turn\":7}\n");
    CHECK(d.readUntil(errorOf(ErrorCode::Protocol)));
    const int status = d.exitStatus();
    CHECK(status > 0);
    CHECK(count(d.items(), ErrorCode::Protocol) == 1);
    CHECK(has<Error>(d.items(), [](const Error& e) { return e.code == ErrorCode::Protocol && e.turn == 7u; }));
    checkStream(d.items(), {});
}

// A line that is not JSON: one protocol error with no turn, then an exit other than 0.
void notJson(Child& d) {
    d.write("nope\n");
    CHECK(d.readUntil(errorOf(ErrorCode::Protocol)));
    CHECK(d.exitStatus() > 0);
    CHECK(has<Error>(d.items(), [](const Error& e) { return e.code == ErrorCode::Protocol && !e.turn; }));
    checkStream(d.items(), {});
}

// A send whose turn does not follow the last: refused with its turn.
void order(Child& d) {
    d.send(Send{5, "reply"});
    CHECK(d.readUntil(doneOf(5)));
    d.send(Send{5, "reply"});
    CHECK(d.readUntil(errorOf(ErrorCode::Protocol)));
    CHECK(d.exitStatus() > 0);
    CHECK(has<Error>(d.items(), [](const Error& e) { return e.code == ErrorCode::Protocol && e.turn == 5u; }));
    checkStream(d.items(), {5});
}

// A send while a turn is open: refused with its turn.
void sendWhileOpen(Child& d) {
    d.send(Send{3, "wait"});
    CHECK(d.readUntil(textOf(3)));
    d.send(Send{4, "reply"});
    CHECK(d.readUntil(errorOf(ErrorCode::Protocol)));
    CHECK(d.exitStatus() > 0);
    CHECK(has<Error>(d.items(), [](const Error& e) { return e.code == ErrorCode::Protocol && e.turn == 4u; }));
    CHECK(!doneOf(4)(d.items()));
    checkStream(d.items(), {3});
}

// The end of input while a turn is open: exit 0, owing no done.
void eofWhileOpen(Child& d) {
    d.send(Send{2, "wait"});
    CHECK(d.readUntil(textOf(2)));
    d.closeInput();
    CHECK(d.exitStatus() == 0);
    CHECK(!doneOf(2)(d.items()));
    checkStream(d.items(), {2});
}

// The fake driver's script for each case: "reply" and "wait" sends, in the order the case sends them.
std::string fakeScript(const std::string& name) {
    const std::string hello = "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n";
    const std::string reply = "send\nemit {\"kind\":\"text\",\"delta\":\"hello\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    const std::string wait = "send\nemit {\"kind\":\"text\",\"delta\":\"partial\"}\nawait-stop\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n";
    if (name == "exchange")
        return hello + reply + wait;
    if (name == "order")
        return hello + reply;
    if (name == "send_while_open" || name == "eof_while_open")
        return hello + wait;
    return hello;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: driver_conformance_test <case> fake <nexus-fake-driver> | "
            "<case> prompted <program> [<argument>...]\n");
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);
    const std::string name = argv[1];
    const std::string kind = argv[2];
    const std::map<std::string, void (*)(Child&)> cases = {{"exchange", exchange}, {"refused", refused},
        {"not_json", notJson}, {"order", order}, {"send_while_open", sendWhileOpen}, {"eof_while_open", eofWhileOpen}};
    const auto found = cases.find(name);
    if (found == cases.end() || (kind != "fake" && kind != "prompted")) {
        std::fprintf(stderr, "driver_conformance_test: no case %s of kind %s\n", name.c_str(), kind.c_str());
        return 2;
    }
    std::vector<std::string> command(argv + 3, argv + argc);
    command.insert(command.end(), {"--cwd", "."});
    if (kind == "fake") {
        const std::string script = "conformance-" + name + ".script";
        std::ofstream(script) << fakeScript(name);
        command.insert(command.end(), {"--script", script});
    }
    Child child(command);
    CHECK(child.started());
    if (child.started())
        found->second(child);
    if (failures != 0) {
        std::fprintf(stderr, "driver_conformance_test %s %s: %d check(s) failed; the driver wrote:\n", name.c_str(),
            kind.c_str(), failures);
        for (const DriverReader::Item& item : child.items())
            std::fprintf(stderr, "  %s", item.message ? encode(*item.message).line.c_str() : (item.error + "\n").c_str());
    }
    return failures == 0 ? 0 : 1;
}
