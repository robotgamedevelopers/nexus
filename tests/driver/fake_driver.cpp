// A driver that speaks the protocol (src/driver/protocol.h) from a script instead of a harness: the stand-in the
// server's functional tests start as a friend's driver.
//
//   nexus-fake-driver --cwd <dir> [--resume <session id>] [--account <name>] --script <file>
//
// The script is lines of text; blank lines and lines starting with # are skipped. The lines before the first `send`
// run at start (they write the hello, so a script can also write a wrong one). Each `send` line starts the block that
// answers the next send the driver accepts, in order. The steps:
//
//   emit <json>          write this message, given as one JSON object. Inside a send block, an object with no "turn"
//                        member gets the send's turn. It must decode as a driver message; use raw for one that
//                        does not.
//   raw <text>           write <text> as a line exactly (a malformed line)
//   tail <text>          write <text> with no newline after it (a last line cut short)
//   flood <count> <json> emit the message <count> times, as fast as the pipe takes them
//   await-stop           read until a stop of this send's turn arrives, then go on with the block
//   await-secret <path>  read until a secret's answer arrives (or a stop of this send's turn), appending each line
//                        read to <path> as it came, then go on with the block
//   hang                 write nothing more and ignore what arrives, until standard input ends
//   exit <code>          exit at once with that status, mid-turn if a turn is open
//   signal <number>      end by that signal, sent to itself
//   stderr <text>        write <text> and a newline to standard error
//   record <path> [VAR ...]
//                        append to <path> the arguments it was started with and the directory it works in, as
//                        `cwd=<--cwd>`, `dir=<working directory>` and `resume=<id>` (empty when not given), then
//                        `account=<name>` (only when given), and each named environment variable as `VAR=<value>` or
//                        `VAR unset`
//   record-send <path>   in a send block: append to <path> `send <turn>:`, a newline, the send's prompt and a newline
//   count-starts <path>  count this start in the file <path> (which holds the count of earlier starts, or is absent)
//   on-start <n> <step>  run <step> only on the <n>th start, as counted by an earlier count-starts
//   option <name>        anywhere in the script, for the whole run:
//                          ignore-eof          when standard input ends, wait to be killed instead of exiting
//                          ignore-sigterm      ignore SIGTERM, so that only a stronger signal ends it
//                          stop-any-turn       act on any stop while a turn is open (breaks: a stop names its turn)
//                          accept-bad-input    report a line it refuses, or a send whose turn does not follow or that
//                                              arrives while a turn is open, and go on (a send while a turn is open is
//                                              dropped; one out of order is answered) (breaks: the driver exits)
//                          done-without-cause  answer a send the script has no block for with done outcome error
//                                              alone (breaks: an error states the cause first)
//
// By default it keeps the protocol's rules: it acts on a stop only for the open turn; it refuses any line it cannot
// accept (malformed, a known kind with a bad or missing field, a send whose turn is not above the last it saw or that
// arrives while a turn is open) with one error of code protocol, carrying the line's turn when it has a readable one,
// then exits 3; it answers a send the script has no block for with an error and a done whose outcome is error; it skips
// messages of unknown kinds; and it exits 0 when its standard input ends. A script that cannot be read or that writes a
// message which does not decode is a bug in the test: the fake says why on standard error and exits 2.

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "driver/protocol.h"
#include "pr/json.h"

namespace {

using namespace nexus::driver;

constexpr int kRefusedSend = 3;

[[noreturn]] void scriptBug(const std::string& why) {
    std::fprintf(stderr, "nexus-fake-driver: %s\n", why.c_str());
    std::exit(2);
}

void writeOut(std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            std::exit(1);   // the server closed our output: nothing is left to say
        bytes.remove_prefix(std::size_t(n));
    }
}

void emit(const DriverMessage& message) {
    const Encoded e = encode(message);
    if (!e.error.empty())
        scriptBug(e.error);
    writeOut(e.line);
}

// The arguments the driver was started with.
struct Arguments {
    std::string cwd;
    std::optional<std::string> resume;
    std::optional<std::string> account;
    std::string script;
};

struct Step {
    std::string op;
    std::string arg;
    int lineNumber = 0;
    int onStart = 0;            // run only on this start; 0 for every start
};

struct Script {
    std::vector<Step> start;
    std::vector<std::vector<Step>> sends;
    std::set<std::string> options;
};

const std::set<std::string> kSteps = {"emit", "raw", "tail", "flood", "await-stop", "await-secret", "hang", "exit",
    "signal", "stderr", "record", "record-send", "count-starts"};
const std::set<std::string> kOptions = {"ignore-eof", "ignore-sigterm", "stop-any-turn", "accept-bad-input",
    "done-without-cause"};

// The first word of `s`, and the rest after one space.
std::pair<std::string, std::string> split(const std::string& s) {
    const std::size_t space = s.find(' ');
    return {s.substr(0, space), space == std::string::npos ? "" : s.substr(space + 1)};
}

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
        const std::string where = path + ":" + std::to_string(number) + ": ";
        auto [op, arg] = split(line);
        if (op == "send") {
            block = &script.sends.emplace_back();
            continue;
        }
        if (op == "option") {
            if (!kOptions.contains(arg))
                scriptBug(where + "unknown option \"" + arg + "\"");
            script.options.insert(arg);
            continue;
        }
        Step step{op, arg, number, 0};
        if (op == "on-start") {
            auto [n, rest] = split(arg);
            step.onStart = std::atoi(n.c_str());
            std::tie(step.op, step.arg) = split(rest);
            if (step.onStart < 1)
                scriptBug(where + "on-start takes a start number from 1 and a step");
        }
        if (!kSteps.contains(step.op))
            scriptBug(where + "unknown step \"" + step.op + "\"; see the steps at the top of fake_driver.cpp");
        block->push_back(std::move(step));
    }
    return script;
}

// The turn a line names, if it is JSON with a "turn" that is a valid turn number, even though the line was refused.
std::optional<std::uint64_t> readableTurn(const std::string& line) {
    PrJson* doc = prJsonCreate();
    std::optional<std::uint64_t> turn;
    if (prJsonParse(doc, PrStringView{line.data(), line.size()}, nullptr)) {
        const PrJsonValueIndex v = prJsonMember(doc, 0, PrStringView{"turn", 4});
        const double d = prJsonNumber(doc, v, 0);
        if (prJsonKind(doc, v) == PrJsonKind_Number && d >= 1 && d <= double(kMaxTurn) && d == double(std::uint64_t(d)))
            turn = std::uint64_t(d);
    }
    prJsonDestroy(doc);
    return turn;
}

// A line from the server: a message, or why it was refused and the turn it named if that was readable.
struct Input {
    std::optional<ServerMessage> message;
    std::string error;
    std::optional<std::uint64_t> turn;
    std::string line;           // the line as it came, when it was one
};

// The server's lines, read from standard input as they are needed.
class Reader {
public:
    explicit Reader(bool ignoreEof) : ignoreEof_(ignoreEof) {}

    // The next line, or nothing once standard input has ended.
    std::optional<Input> next() {
        while (pending_.empty()) {
            if (ended_) {
                while (ignoreEof_)
                    ::pause();
                return std::nullopt;
            }
            char buffer[4096];
            const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR)
                continue;
            lines_.clear();
            if (n <= 0) {
                ended_ = true;
                reader_.finish(lines_);
            } else {
                reader_.feed(std::string_view(buffer, std::size_t(n)), lines_);
            }
            for (const LineReader::Line& line : lines_) {
                if (!line.error.empty()) {
                    pending_.push_back(Input{std::nullopt, line.error, std::nullopt, std::string()});
                    continue;
                }
                Decoded<ServerMessage> d = decodeServerMessage(line.text);
                if (d.message)
                    pending_.push_back(Input{std::move(d.message), {}, std::nullopt, line.text});
                else
                    pending_.push_back(Input{std::nullopt, "line " + std::to_string(line.number) + ": " + d.error,
                        readableTurn(line.text), line.text});
            }
        }
        Input input = std::move(pending_.front());
        pending_.pop_front();
        return input;
    }

private:
    bool ignoreEof_;
    LineReader reader_;
    std::vector<LineReader::Line> lines_;
    std::deque<Input> pending_;
    bool ended_ = false;
};

// Whether the JSON object `json` has a "turn" member.
bool hasTurn(const std::string& json) {
    PrJson* doc = prJsonCreate();
    const bool has = prJsonParse(doc, PrStringView{json.data(), json.size()}, nullptr) &&
        prJsonMember(doc, 0, PrStringView{"turn", 4}) != kPrJsonMissing;
    prJsonDestroy(doc);
    return has;
}

class Fake {
public:
    Fake(Script script, Arguments arguments)
        : script_(std::move(script)), arguments_(std::move(arguments)), in_(option("ignore-eof")) {}

    int run() {
        play(script_.start, 0);
        while (std::optional<Input> input = in_.next()) {
            if (!input->message) {
                badLine(*input);
                continue;
            }
            if (const Send* send = std::get_if<Send>(&*input->message))
                answer(*send);
            // a stop with no turn open, and any unknown kind: nothing
        }
        return 0;
    }

private:
    bool option(const char* name) const { return script_.options.contains(name); }

    // A line the driver refused: one error with the line's turn if it was readable, then the end.
    void badLine(const Input& input) {
        refuse(Error{"the server sent a line the driver refused: " + input.error, "the server must write one "
            "protocol message per line (docs/driver-protocol.md)", input.turn, ErrorCode::Protocol});
    }

    // A send the driver cannot accept: the same, with its turn.
    void badSend(const Send& send, const std::string& why) {
        refuse(Error{"send of turn " + std::to_string(send.turn) + " " + why, "the server must send turns in order, "
            "each after the last one's done (docs/driver-protocol.md)", send.turn, ErrorCode::Protocol});
    }

    void refuse(const Error& error) {
        emit(error);
        if (!option("accept-bad-input"))
            std::exit(kRefusedSend);
    }

    void answer(const Send& send) {
        if (send.turn <= lastTurn_)
            badSend(send, "does not follow turn " + std::to_string(lastTurn_));
        lastTurn_ = send.turn;
        lastPrompt_ = send.prompt;
        if (nextBlock_ == script_.sends.size()) {
            if (!option("done-without-cause"))
                emit(Error{"the script has no reply for send " + std::to_string(nextBlock_ + 1), "add a send block "
                    "to the fake driver's script", send.turn, ErrorCode::Other});
            emit(Done{send.turn, Outcome::Error, std::nullopt, std::nullopt});
            return;
        }
        play(script_.sends[nextBlock_++], send.turn);
    }

    void play(const std::vector<Step>& steps, std::uint64_t turn) {
        for (const Step& step : steps) {
            if (step.onStart != 0) {
                if (start_ == 0)
                    scriptBug("script line " + std::to_string(step.lineNumber) + ": on-start needs a count-starts "
                        "before it");
                if (step.onStart != start_)
                    continue;
            }
            run(step, turn);
        }
    }

    void run(const Step& step, std::uint64_t turn) {
        if (step.op == "emit") {
            writeOut(withTurn(step, step.arg, turn));
        } else if (step.op == "raw") {
            writeOut(step.arg + "\n");
        } else if (step.op == "tail") {
            writeOut(step.arg);
        } else if (step.op == "flood") {
            auto [count, json] = split(step.arg);
            const std::string line = withTurn(step, json, turn);
            for (long i = std::atol(count.c_str()); i > 0; --i)
                writeOut(line);
        } else if (step.op == "await-stop") {
            awaitStop(turn);
        } else if (step.op == "await-secret") {
            awaitSecret(step.arg, turn);
        } else if (step.op == "hang") {
            while (in_.next()) {
            }
            std::exit(0);
        } else if (step.op == "exit") {
            std::exit(std::atoi(step.arg.c_str()));
        } else if (step.op == "signal") {
            std::raise(std::atoi(step.arg.c_str()));
        } else if (step.op == "stderr") {
            std::fprintf(stderr, "%s\n", step.arg.c_str());
        } else if (step.op == "record") {
            record(step.arg);
        } else if (step.op == "record-send") {
            std::ofstream out(step.arg, std::ios::app);
            out << "send " << turn << ":\n" << lastPrompt_ << "\n";
            if (!out) {
                scriptBug("cannot record to " + step.arg);
            }
        } else {
            countStarts(step.arg);
        }
    }

    // `json` as a line, given `turn` if it is in a send block and has none.
    std::string withTurn(const Step& step, std::string json, std::uint64_t turn) const {
        if (turn != 0 && !hasTurn(json)) {
            const std::size_t brace = json.find('{');
            if (brace != std::string::npos)
                json.insert(brace + 1, "\"turn\":" + std::to_string(turn) + ",");
        }
        const Decoded<DriverMessage> d = decodeDriverMessage(json);
        if (!d.message)
            scriptBug("script line " + std::to_string(step.lineNumber) + ": " + d.error + " (write a malformed line "
                "with raw)");
        return json + "\n";
    }

    void awaitStop(std::uint64_t turn) {
        while (std::optional<Input> item = in_.next()) {
            if (!item->message) {
                badLine(*item);
                continue;
            }
            if (const Stop* stop = std::get_if<Stop>(&*item->message)) {
                if (stop->turn == turn || option("stop-any-turn"))
                    return;
            } else if (const Send* send = std::get_if<Send>(&*item->message)) {
                badSend(*send, "arrived while turn " + std::to_string(turn) + " is open");
            }
        }
        std::exit(0);
    }

    // Each line read written to `path` as it came, until a secret's answer, or a stop of `turn`, arrives.
    void awaitSecret(const std::string& path, std::uint64_t turn) {
        std::ofstream out(path, std::ios::app);
        while (std::optional<Input> item = in_.next()) {
            out << item->line << "\n";
            out.flush();
            if (!out) {
                scriptBug("cannot record to " + path);
            }
            if (!item->message) {
                badLine(*item);
                continue;
            }
            if (std::holds_alternative<SecretAnswer>(*item->message)) {
                return;
            }
            if (const Stop* stop = std::get_if<Stop>(&*item->message)) {
                if (stop->turn == turn || option("stop-any-turn")) {
                    return;
                }
            } else if (const Send* send = std::get_if<Send>(&*item->message)) {
                badSend(*send, "arrived while turn " + std::to_string(turn) + " is open");
            }
        }
        std::exit(0);
    }

    void record(const std::string& arg) {
        auto [path, names] = split(arg);
        std::ofstream out(path, std::ios::app);
        const std::string dir = std::filesystem::current_path().string();
        out << "cwd=" << arguments_.cwd << "\ndir=" << dir << "\nresume=" << arguments_.resume.value_or("") << "\n";
        if (arguments_.account) {
            out << "account=" << *arguments_.account << "\n";
        }
        while (!names.empty()) {
            auto [name, rest] = split(names);
            const char* value = std::getenv(name.c_str());
            out << name << (value != nullptr ? "=" + std::string(value) : std::string(" unset")) << "\n";
            names = rest;
        }
        if (!out)
            scriptBug("cannot record to " + path);
    }

    void countStarts(const std::string& path) {
        int earlier = 0;
        std::ifstream(path) >> earlier;
        start_ = earlier + 1;
        if (!(std::ofstream(path) << start_ << "\n"))
            scriptBug("cannot count starts in " + path);
    }

    Script script_;
    Arguments arguments_;
    Reader in_;
    std::uint64_t lastTurn_ = 0;
    std::string lastPrompt_;    // the prompt of the send being answered, for record-send
    std::size_t nextBlock_ = 0;
    int start_ = 0;             // which start this is, once count-starts has run
};

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if ((a == "--cwd" || a == "--resume" || a == "--script" || a == "--account") && i + 1 < argc) {
            const char* value = argv[++i];
            if (a == "--cwd") {
                arguments.cwd = value;
            } else if (a == "--resume") {
                arguments.resume = value;
            } else if (a == "--account") {
                arguments.account = value;
            } else {
                arguments.script = value;
            }
        } else {
            scriptBug("unknown or incomplete argument " + std::string(a) + "; the arguments are --cwd <dir>, "
                "--resume <session id>, --account <name> and --script <file>");
        }
    }
    if (arguments.cwd.empty() || arguments.script.empty())
        scriptBug("--cwd <dir> and --script <file> are required");
    Script script = readScript(arguments.script);
    if (::chdir(arguments.cwd.c_str()) != 0) {
        emit(Hello{kProtocolVersion, "fake"});
        emit(Error{"cannot work in " + arguments.cwd + ": " + std::strerror(errno), "start the driver with --cwd "
            "naming a directory that exists", std::nullopt, ErrorCode::Other});
        return 2;
    }
    if (script.options.contains("ignore-sigterm"))
        std::signal(SIGTERM, SIG_IGN);
    return Fake(std::move(script), std::move(arguments)).run();
}
