// A driver that speaks the protocol (src/driver/protocol.h) from a script instead of a harness: the stand-in the
// server's functional tests start as a friend's driver.
//
//   nexus-fake-driver --cwd <dir> [--resume <session id>] --script <file>
//
// The script is lines of text; blank lines and lines starting with # are skipped. The lines before the first `send`
// run at start (they write the hello, so a script can also write a wrong one). Each `send` line starts the block that
// answers the next send the driver reads, in order. In a block:
//
//   emit <json>    write this message, given as one JSON object. Inside a send block, an object with no "turn" key gets
//                  the send's turn. It must decode as a driver message (src/driver/protocol.h); use raw for one that
//                  does not.
//   raw <text>     write <text> as a line exactly (a malformed line)
//   await-stop     read until a stop arrives, then go on with the block
//   hang           write nothing more and ignore what arrives, until standard input ends (silent; after await-stop it
//                  ignores a stop)
//   exit <code>    exit at once with that status, mid-turn if a turn is open
//
// Like a real driver it ignores a stop with no turn open and messages of unknown kinds, reports a bad line from the
// server, and a send that does not follow the last turn or arrives while a turn is open, as an error, and answers a
// send the script has no block for with an error and a done whose outcome is error. It exits 0 when its standard input
// ends. A script that cannot be read or that writes a message which does not decode is a bug in the test: the fake
// says why on standard error and exits 2.

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "driver/protocol.h"

namespace {

using namespace nexus::driver;

[[noreturn]] void scriptBug(const std::string& why) {
    std::fprintf(stderr, "nexus-fake-driver: %s\n", why.c_str());
    std::exit(2);
}

void writeLine(std::string_view line) {
    while (!line.empty()) {
        const ssize_t n = ::write(STDOUT_FILENO, line.data(), line.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            std::exit(1);   // the server closed our output: nothing is left to say
        line.remove_prefix(std::size_t(n));
    }
}

void emit(const DriverMessage& message) {
    const Encoded e = encode(message);
    if (!e.error.empty())
        scriptBug(e.error);
    writeLine(e.line);
}

struct Step {
    std::string op;
    std::string arg;
    int lineNumber = 0;
};

struct Script {
    std::vector<Step> start;
    std::vector<std::vector<Step>> sends;
};

Script readScript(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        scriptBug("cannot read the script " + path + ": " + std::strerror(errno));
    Script script;
    std::vector<Step>* block = &script.start;
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        if (line.empty() || line[0] == '#')
            continue;
        const std::size_t space = line.find(' ');
        Step step{line.substr(0, space), space == std::string::npos ? "" : line.substr(space + 1), number};
        if (step.op == "send") {
            block = &script.sends.emplace_back();
            continue;
        }
        if (step.op != "emit" && step.op != "raw" && step.op != "await-stop" && step.op != "hang" && step.op != "exit")
            scriptBug(path + ":" + std::to_string(number) + ": unknown step \"" + step.op + "\"; the steps are send, "
                "emit, raw, await-stop, hang and exit");
        block->push_back(std::move(step));
    }
    return script;
}

// The server's messages, read from standard input as they are needed.
class Input {
public:
    // The next message or bad line, or nothing once standard input has ended.
    std::optional<ServerReader::Item> next() {
        while (pending_.empty()) {
            if (ended_)
                return std::nullopt;
            char buffer[4096];
            const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR)
                continue;
            items_.clear();
            if (n <= 0) {
                ended_ = true;
                reader_.finish(items_);
            } else {
                reader_.feed(std::string_view(buffer, std::size_t(n)), items_);
            }
            pending_.insert(pending_.end(), items_.begin(), items_.end());
        }
        ServerReader::Item item = std::move(pending_.front());
        pending_.pop_front();
        return item;
    }

private:
    ServerReader reader_;
    std::vector<ServerReader::Item> items_;
    std::deque<ServerReader::Item> pending_;
    bool ended_ = false;
};

class Fake {
public:
    explicit Fake(Script script) : script_(std::move(script)) {}

    int run() {
        play(script_.start, 0);
        while (std::optional<ServerReader::Item> item = in_.next()) {
            if (!item->message) {
                badLine(item->error);
                continue;
            }
            if (const Send* send = std::get_if<Send>(&*item->message))
                answer(*send);
            // a stop with no turn open, and any unknown kind: nothing
        }
        return 0;
    }

private:
    void badLine(const std::string& error) {
        emit(Error{"the server sent a line the driver refused: " + error,
            "the server must write one protocol message per line (docs/driver-protocol.md)", std::nullopt});
    }

    void answer(const Send& send) {
        if (send.turn <= lastTurn_) {
            emit(Error{"send of turn " + std::to_string(send.turn) + " after turn " + std::to_string(lastTurn_) +
                "; turns only increase", "the server must number each send above the last", std::nullopt});
            return;
        }
        lastTurn_ = send.turn;
        if (nextBlock_ == script_.sends.size()) {
            emit(Error{"the script has no reply for send " + std::to_string(nextBlock_ + 1), "add a send block to the "
                "fake driver's script", send.turn});
            emit(Done{send.turn, Outcome::Error, std::nullopt, std::nullopt});
            return;
        }
        play(script_.sends[nextBlock_++], send.turn);
    }

    void play(const std::vector<Step>& steps, std::uint64_t turn) {
        for (const Step& step : steps) {
            if (step.op == "emit")
                writeLine(withTurn(step, turn));
            else if (step.op == "raw")
                writeLine(step.arg + "\n");
            else if (step.op == "await-stop")
                awaitStop(turn);
            else if (step.op == "hang")
                hang();
            else
                std::exit(std::atoi(step.arg.c_str()));
        }
    }

    // The step's message as a line, given `turn` if it is in a send block and has none.
    std::string withTurn(const Step& step, std::uint64_t turn) const {
        const std::string where = "script line " + std::to_string(step.lineNumber);
        std::string line = step.arg;
        if (turn != 0 && line.find("\"turn\"") == std::string::npos) {
            const std::size_t brace = line.find('{');
            if (brace == std::string::npos)
                scriptBug(where + ": emit takes a JSON object");
            line.insert(brace + 1, "\"turn\":" + std::to_string(turn) + ",");
        }
        const Decoded<DriverMessage> d = decodeDriverMessage(line);
        if (!d.message)
            scriptBug(where + ": " + d.error + " (write a malformed line with raw)");
        return line + "\n";
    }

    void awaitStop(std::uint64_t turn) {
        while (std::optional<ServerReader::Item> item = in_.next()) {
            if (!item->message) {
                badLine(item->error);
                continue;
            }
            if (std::holds_alternative<Stop>(*item->message))
                return;
            if (const Send* send = std::get_if<Send>(&*item->message))
                emit(Error{"send of turn " + std::to_string(send->turn) + " while turn " + std::to_string(turn) +
                    " is open; it was dropped", "the server must wait for a turn's done before the next send",
                    std::nullopt});
        }
        std::exit(0);
    }

    void hang() {
        while (in_.next()) {
        }
        std::exit(0);
    }

    Script script_;
    Input in_;
    std::uint64_t lastTurn_ = 0;
    std::size_t nextBlock_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    std::string cwd;
    std::string script;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        // --resume is taken and ignored: the fake has no session to resume.
        if ((a == "--cwd" || a == "--resume" || a == "--script") && i + 1 < argc) {
            const char* value = argv[++i];
            if (a == "--cwd")
                cwd = value;
            else if (a == "--script")
                script = value;
        } else {
            scriptBug("unknown or incomplete argument " + std::string(a) + "; the arguments are --cwd <dir>, "
                "--resume <session id> and --script <file>");
        }
    }
    if (cwd.empty() || script.empty())
        scriptBug("--cwd <dir> and --script <file> are required");
    Fake fake(readScript(script));
    if (::chdir(cwd.c_str()) != 0) {
        emit(Hello{kProtocolVersion, "fake"});
        emit(Error{"cannot work in " + cwd + ": " + std::strerror(errno), "start the driver with --cwd naming a "
            "directory that exists", std::nullopt});
        return 2;
    }
    return fake.run();
}
