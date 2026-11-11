#include "server/supervisor.h"

#include <cstdio>
#include <numeric>
#include <type_traits>
#include <utility>
#include <variant>

namespace nexus::server {

namespace {

using namespace nexus::driver;

// The kind a message has on the line.
std::string kindOf(const DriverMessage& message) {
    return std::visit([](const auto& m) -> std::string {
        using M = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<M, Hello>) return "hello";
        else if constexpr (std::is_same_v<M, Credential>) return "credential";
        else if constexpr (std::is_same_v<M, Session>) return "session";
        else if constexpr (std::is_same_v<M, Text>) return "text";
        else if constexpr (std::is_same_v<M, TextEnd>) return "text-end";
        else if constexpr (std::is_same_v<M, Tool>) return "tool";
        else if constexpr (std::is_same_v<M, Done>) return "done";
        else if constexpr (std::is_same_v<M, Error>) return "error";
        else return m.kind;
    }, message);
}

// The turn a message of a turn names.
std::uint64_t turnOf(const DriverMessage& message) {
    return std::visit([](const auto& m) -> std::uint64_t {
        using M = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<M, Text> || std::is_same_v<M, TextEnd> || std::is_same_v<M, Tool> ||
                      std::is_same_v<M, Done>)
            return m.turn;
        else
            return 0;
    }, message);
}

// The longest start of `text` at most `max` bytes that ends between two characters (a whole one when the first is
// longer than `max`, which a limit of a few bytes would need).
std::size_t cutAt(const std::string& text, std::size_t max) {
    if (text.size() <= max)
        return text.size();
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    if (cut > 0)
        return cut;
    cut = 1;
    while (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        ++cut;
    return cut;
}

std::string secondsText(Millis ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g s", static_cast<double>(ms) / 1000.0);
    return buffer;
}

// A sentence, ended by one full stop.
std::string sentence(std::string s) {
    while (!s.empty() && (s.back() == '.' || s.back() == ' '))
        s.pop_back();
    return s + ".";
}

std::string quoted(const std::vector<std::string>& command) {
    return command.empty() ? std::string("(no program)") : "`" + command.front() + "`";
}

} // namespace

EntryKind entryKind(Effect::What what) {
    switch (what) {
        case Effect::What::Prompt: return EntryKind::Person;
        case Effect::What::Reply:
        case Effect::What::Tool: return EntryKind::Friend;
        case Effect::What::Error:
        case Effect::What::Stopped:
        case Effect::What::Cut:
        case Effect::What::Notice: return EntryKind::Notice;
    }
    return EntryKind::Notice;
}

Supervisor::Supervisor(Config config, sync::DriverState state, Millis now, Output* out)
    : config_(std::move(config)), state_(std::move(state)) {
    if (state_.openTurn != 0) {
        effect(Effect::What::Cut, "The reply was cut short: nexus-server restarted while " + config_.name +
            " was answering. Whether " + config_.name + " got the prompt is unknown; send it again if you still want "
            "an answer.", state_.openTurn);
        state_.openTurn = 0;
        out_.durable = true;
    }
    advance(now);
    *out = finish();
}

void Supervisor::configure(std::vector<std::string> command, std::string cwd, std::string configured) {
    config_.command = std::move(command);
    config_.cwd = std::move(cwd);
    config_.configured = std::move(configured);
}

std::optional<Millis> Supervisor::deadline() const {
    return deadline_;
}

Output Supervisor::prompt(const std::string& text, Millis now) {
    const std::size_t bytes = std::accumulate(state_.waiting.begin(), state_.waiting.end(), std::size_t(0),
        [](std::size_t n, const std::string& w) { return n + w.size(); });
    const Encoded line = encode(Send{state_.nextTurn, text});
    const std::string notSent = "A prompt to " + config_.name + " was not sent: ";
    if (state_.waiting.size() >= config_.maxWaiting) {
        effect(Effect::What::Notice, notSent + std::to_string(state_.waiting.size()) + " prompts already wait for " +
            config_.name + ", the limit. Send it again once " + config_.name + " has answered.");
    } else if (bytes + text.size() > config_.maxWaitingBytes) {
        effect(Effect::What::Notice, notSent + "the prompts waiting for " + config_.name + " would hold more than " +
            std::to_string(config_.maxWaitingBytes / 1024) + " KiB. Send it again once " + config_.name +
            " has answered.");
    } else if (!line.error.empty()) {
        effect(Effect::What::Notice, notSent + sentence(line.error));
    } else {
        effect(Effect::What::Prompt, text);
        state_.waiting.push_back(text);
        out_.durable = true;
        if (held_) {
            held_ = false;
            failed_ = 0;
        }
        advance(now);
    }
    return finish();
}

Output Supervisor::stop(Millis now) {
    if (phase_ == Phase::Working) {
        command(Command::Kind::Write, {}, encode(Stop{state_.openTurn}).line);
        phase_ = Phase::Stopping;
        deadline_ = now + config_.timing.stop;
    }
    return finish();
}

Output Supervisor::message(const DriverMessage& message, Millis now) {
    if (phase_ == Phase::None)
        return finish();    // no driver: nothing it says can arrive
    if (phase_ == Phase::Ending)
        return finish();    // being ended: its turn is closed already, and what it says is dropped
    if (phase_ == Phase::Starting) {
        const Hello* hello = std::get_if<Hello>(&message);
        if (hello != nullptr && hello->protocol == kProtocolVersion) {
            phase_ = Phase::Idle;
            deadline_.reset();
        } else if (hello != nullptr) {
            end(config_.name + "'s driver speaks driver protocol version " + std::to_string(hello->protocol) +
                ", and this nexus-server speaks " + std::to_string(kProtocolVersion) + ", so it was ended. Install "
                "the driver that goes with this nexus-server (" + config_.configured + ").", now);
        } else {
            end(brokeProtocol("wrote " + kindOf(message) + " before its hello"), now);
        }
    } else {
        receive(message, now);
    }
    advance(now);
    return finish();
}

// A message from a driver past its hello.
void Supervisor::receive(const DriverMessage& message, Millis now) {
    const std::uint64_t open = state_.openTurn;
    if (std::holds_alternative<Hello>(message)) {
        end(brokeProtocol("wrote a second hello"), now);
    } else if (const Credential* c = std::get_if<Credential>(&message)) {
        if (c->billing == Billing::Subscription) {
            out_.credential = *c;
        } else {
            end(config_.name + (c->billing == Billing::ApiKey ? "'s harness is using an API key, and nexus uses "
                "subscription logins only" : "'s harness has no credential it can use") + ", so its driver was "
                "ended. Sign the harness in with a subscription, then send a prompt to " + config_.name +
                " again.", now);
            held_ = true;
        }
    } else if (const Session* s = std::get_if<Session>(&message)) {
        if (state_.session != s->id) {
            state_.session = s->id;
            out_.durable = true;
        }
    } else if (const Error* e = std::get_if<Error>(&message)) {
        lastCause_ = e->cause;
        lastFix_ = e->fix;
        std::string text = e->code == ErrorCode::SessionNotFound
            ? "The earlier conversation with " + config_.name + " could not be resumed, so this is a new one: " +
              sentence(e->cause)
            : config_.name + "'s driver reports: " + sentence(e->cause) + " " + sentence(e->fix);
        if (!e->turn)
            effect(Effect::What::Notice, std::move(text));
        else if (*e->turn == open && open != 0)
            effect(Effect::What::Error, std::move(text), open);
        // an error of a turn not open (the one a driver writes as it refuses a line) is dropped: it exits next
    } else if (std::holds_alternative<Unknown>(message)) {
        // a kind this version does not know: skipped
    } else if (open == 0 || turnOf(message) != open) {
        end(brokeProtocol("wrote " + kindOf(message) + " for turn " + std::to_string(turnOf(message)) + (open == 0
            ? std::string(" with no turn open") : " while turn " + std::to_string(open) + " is open")), now);
    } else if (const Text* t = std::get_if<Text>(&message)) {
        gathered_ += t->delta;
        // Gathered text past the entry limit goes into the tab in whole entries now, so that a driver that never
        // ends its block holds no more than one entry here.
        while (gathered_.size() > config_.maxEntryBytes) {
            const std::size_t cut = cutAt(gathered_, config_.maxEntryBytes);
            effect(Effect::What::Reply, gathered_.substr(0, cut), open);
            gathered_.erase(0, cut);
        }
    } else if (std::holds_alternative<TextEnd>(message)) {
        flush();
    } else if (const Tool* tool = std::get_if<Tool>(&message)) {
        flush();
        std::string line = tool->name + ": " + tool->summary;
        line.resize(cutAt(line, config_.maxEntryBytes));
        effect(Effect::What::Tool, std::move(line), open);
    } else if (const Done* done = std::get_if<Done>(&message)) {
        flush();
        if (done->outcome == Outcome::Stopped)
            effect(Effect::What::Stopped, "Stopped.", open);
        if (done->sessionCostUsd)
            out_.sessionCostUsd = done->sessionCostUsd;
        close();
    }
}

Output Supervisor::refused(const std::string& cause, Millis now) {
    if (phase_ != Phase::None && phase_ != Phase::Ending)
        end(config_.name + "'s driver wrote a line the server refused (" + cause + "), so it was ended. Fix the "
            "driver: each line it writes must be one message of docs/driver-protocol.md.", now);
    advance(now);
    return finish();
}

Output Supervisor::closed(const std::string& what, Millis now) {
    if (phase_ != Phase::None && phase_ != Phase::Ending)
        end(config_.name + "'s driver closed its " + what + " while it ran, so it was ended. A driver keeps its "
            "standard input and output open until it exits.", now);
    advance(now);
    return finish();
}

Output Supervisor::exited(const std::string& how, Millis now) {
    if (phase_ == Phase::None)
        return finish();
    if (phase_ != Phase::Ending)
        abandon(config_.name + "'s driver " + how + ".");
    const bool counted = !finished_ && !held_ && !down_;
    if (counted)
        ++failed_;
    phase_ = Phase::None;
    stage_ = Stage::None;
    finished_ = false;
    gathered_.clear();
    deadline_.reset();
    if (counted && failed_ >= config_.maxFailedStarts) {
        const std::string fix = lastFix_.empty()
            ? "Check the driver configured in " + config_.configured + ", and run " + quoted(config_.command) +
              " by hand to see what it says."
            : sentence(lastFix_);
        effect(Effect::What::Notice, "nexus stopped starting " + config_.name + "'s driver after " +
            std::to_string(failed_) + " failed starts in a row. The last: " +
            sentence(lastCause_.empty() ? lastFailure_ : lastCause_) + " " + fix + " Your next prompt to " +
            config_.name + " tries again.");
        held_ = true;
    }
    advance(now);
    return finish();
}

Output Supervisor::tick(Millis now) {
    if (!deadline_ || now < *deadline_)
        return finish();
    if (phase_ == Phase::Starting) {
        end(config_.name + "'s driver did not say hello within " + secondsText(config_.timing.hello) + ", so it was "
            "ended. Check that " + quoted(config_.command) + " is a nexus driver (docs/driver-protocol.md).", now);
    } else if (phase_ == Phase::Stopping) {
        end(config_.name + "'s driver did not end the stopped turn within " + secondsText(config_.timing.stop) +
            ", so it was ended.", now);
    } else if (phase_ == Phase::Ending && stage_ == Stage::Closed) {
        command(Command::Kind::Terminate);
        stage_ = Stage::Termed;
        deadline_ = now + config_.timing.termGrace;
    } else if (phase_ == Phase::Ending && stage_ == Stage::Termed) {
        command(Command::Kind::Kill);
        stage_ = Stage::Killed;
        deadline_.reset();
    }
    advance(now);
    return finish();
}

Output Supervisor::shutdown(Millis now) {
    if (down_)
        return finish();
    down_ = true;
    if (phase_ == Phase::None || phase_ == Phase::Ending)
        return finish();
    if (state_.openTurn != 0) {
        end("nexus-server is stopping, so " + config_.name + "'s driver was ended.", now);
    } else {
        // No turn open: ended quietly, with nothing in the tab.
        phase_ = Phase::Ending;
        stage_ = Stage::Closed;
        command(Command::Kind::CloseInput);
        deadline_ = now + config_.timing.endGrace;
    }
    return finish();
}

void Supervisor::effect(Effect::What what, std::string text, std::uint64_t turn) {
    out_.effects.push_back(Effect{what, std::move(text), turn});
}

void Supervisor::command(Command::Kind kind, std::vector<std::string> argv, std::string line) {
    out_.commands.push_back(Command{kind, std::move(argv), std::move(line)});
}

std::string Supervisor::brokeProtocol(const std::string& what) const {
    return config_.name + "'s driver " + what + ", which breaks the driver protocol, so it was ended. Fix the driver "
        "(docs/driver-protocol.md).";
}

// The open turn's gathered text goes into the tab as one entry: receive() keeps it within the entry limit.
void Supervisor::flush() {
    if (!gathered_.empty())
        effect(Effect::What::Reply, std::move(gathered_), state_.openTurn);
    gathered_.clear();
}

// The open turn ends without its done. Its prompt is never written again.
void Supervisor::cutOpen() {
    effect(Effect::What::Cut, "The reply was cut short. Whether " + config_.name + " got the prompt is unknown; send "
        "it again if you still want an answer.", state_.openTurn);
    state_.openTurn = 0;
    openPrompt_.clear();
    out_.durable = true;
}

// The driver is gone or going: its gathered text, why, and the open turn cut.
void Supervisor::abandon(const std::string& why) {
    flush();
    effect(Effect::What::Notice, why);
    lastFailure_ = why;
    if (state_.openTurn != 0)
        cutOpen();
}

void Supervisor::end(const std::string& why, Millis now) {
    abandon(why);
    phase_ = Phase::Ending;
    stage_ = Stage::Closed;
    command(Command::Kind::CloseInput);
    deadline_ = now + config_.timing.endGrace;
}

// The open turn's done.
void Supervisor::close() {
    state_.openTurn = 0;
    openPrompt_.clear();
    out_.durable = true;
    phase_ = Phase::Idle;
    deadline_.reset();
    finished_ = true;
    failed_ = 0;
}

void Supervisor::startDriver(Millis now) {
    if (phase_ != Phase::None || state_.waiting.empty() || held_ || down_)
        return;
    std::vector<std::string> argv = config_.command;
    argv.insert(argv.end(), {"--cwd", config_.cwd});
    if (!state_.session.empty())
        argv.insert(argv.end(), {"--resume", state_.session});
    command(Command::Kind::Start, std::move(argv));
    phase_ = Phase::Starting;
    stage_ = Stage::None;
    deadline_ = now + config_.timing.hello;
    finished_ = false;
    lastCause_.clear();
    lastFix_.clear();
}

// The oldest waiting prompt, to an idle driver, as the next turn: durable before the line is written.
void Supervisor::writeNext() {
    if (phase_ != Phase::Idle || state_.waiting.empty())
        return;
    const std::uint64_t turn = state_.nextTurn;
    // prompt() refused any prompt that does not encode, and a turn number's digits cannot push a prompt within the
    // hub's text limit past the line limit, so this line is always whole.
    command(Command::Kind::Write, {}, encode(Send{turn, state_.waiting.front()}).line);
    openPrompt_ = std::move(state_.waiting.front());
    state_.waiting.erase(state_.waiting.begin());
    state_.nextTurn = turn + 1;
    state_.openTurn = turn;
    out_.durable = true;
    phase_ = Phase::Working;
}

// What follows every event: a driver started for a waiting prompt, or the prompt written to an idle one.
void Supervisor::advance(Millis now) {
    startDriver(now);
    writeNext();
}

Output Supervisor::finish() {
    Output o = std::move(out_);
    out_ = Output{};
    return o;
}

} // namespace nexus::server
