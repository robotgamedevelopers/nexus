#include "server/supervisor.h"

#include <algorithm>
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
        if constexpr (std::is_same_v<M, Hello>) {
            return "hello";
        } else if constexpr (std::is_same_v<M, Credential>) {
            return "credential";
        } else if constexpr (std::is_same_v<M, Session>) {
            return "session";
        } else if constexpr (std::is_same_v<M, Text>) {
            return "text";
        } else if constexpr (std::is_same_v<M, TextEnd>) {
            return "text-end";
        } else if constexpr (std::is_same_v<M, Tool>) {
            return "tool";
        } else if constexpr (std::is_same_v<M, Done>) {
            return "done";
        } else if constexpr (std::is_same_v<M, Error>) {
            return "error";
        } else if constexpr (std::is_same_v<M, AskSecret>) {
            return "ask-secret";
        } else {
            return m.kind;
        }
    }, message);
}

// The turn a message of a turn names.
std::uint64_t turnOf(const DriverMessage& message) {
    return std::visit([](const auto& m) -> std::uint64_t {
        using M = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<M, Text> || std::is_same_v<M, TextEnd> || std::is_same_v<M, Tool> ||
                      std::is_same_v<M, Done>) {
            return m.turn;
        } else {
            return 0;
    }
    }, message);
}

// The longest start of `text` at most `max` bytes that ends between two characters (a whole one when the first is
// longer than `max`, which a limit of a few bytes would need).
std::size_t cutAt(const std::string& text, std::size_t max) {
    if (text.size() <= max) {
        return text.size();
    }
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    if (cut > 0) {
        return cut;
    }
    cut = 1;
    while (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        ++cut;
    }
    return cut;
}

std::string secondsText(Millis ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g s", static_cast<double>(ms) / 1000.0);
    return buffer;
}

// A sentence, ended by one full stop.
std::string sentence(std::string s) {
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) {
        s.pop_back();
    }
    return s + ".";
}

std::string quoted(const std::vector<std::string>& command) {
    return command.empty() ? std::string("(no program)") : "`" + command.front() + "`";
}

} // namespace

EntryKind entryKind(Effect::What what) {
    switch (what) {
        case Effect::What::Prompt: return EntryKind::Person;
        case Effect::What::Reply: return EntryKind::Friend;
        case Effect::What::Tool: return EntryKind::Tool;
        case Effect::What::Error:
        case Effect::What::Stopped:
        case Effect::What::Cut:
        case Effect::What::Notice: return EntryKind::Notice;
    }
    return EntryKind::Notice;
}

Supervisor::Supervisor(Config config, sync::DriverState state, Millis now, Output* out, std::vector<std::string> queue)
    : config_(std::move(config)), state_(std::move(state)), queue_(std::move(queue)) {
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

Output Supervisor::configure(std::vector<std::string> command, std::string cwd, std::string configured, Millis now) {
    config_.command = std::move(command);
    config_.cwd = std::move(cwd);
    config_.configured = std::move(configured);
    advance(now);
    return finish();
}

Output Supervisor::setPlace(std::string cwd, bool here, Millis now) {
    config_.cwd = std::move(cwd);
    config_.here = here;
    advance(now);
    return finish();
}

// Tightened under an open turn on a driver started unlimited: that turn is stopped now, through the person's stop
// (Stop, then the stop deadline and the kill), so no driver runs with a weaker limit than its friend's for longer than
// the stop takes; once it is done the driver is ended (retire) and the next starts limited. Loosened: the open turn
// finishes as it started. Models: models/Supervisor.tla SetLimit, NeverWeaker.
Output Supervisor::setLimited(bool limited, Millis now) {
    config_.limited = limited;
    stopWeaker(now);
    advance(now);
    return finish();
}

// More kept out is a tightening, as setLimited(true) is; fewer, a loosening. Each limited start passes them all.
Output Supervisor::setKeepsOut(std::vector<std::string> keepsOut, Millis now) {
    config_.keepsOut = std::move(keepsOut);
    stopWeaker(now);
    advance(now);
    return finish();
}

// The alive driver keeps less out than the session's limit says: started unlimited while the session is limited, or
// limited without one of the directories the session's limit keeps out.
bool Supervisor::weaker() const {
    if (!config_.limited) {
        return false;
    }
    if (!driverLimited_) {
        return true;
    }
    return std::any_of(config_.keepsOut.begin(), config_.keepsOut.end(), [this](const std::string& d) {
        return std::find(driverKeepsOut_.begin(), driverKeepsOut_.end(), d) == driverKeepsOut_.end();
    });
}

// A turn open on a driver weaker than the session's limit is stopped now, as the person's stop does.
void Supervisor::stopWeaker(Millis now) {
    if (phase_ == Phase::Working && weaker()) {
        effect(Effect::What::Notice, driverLimited_
            ? config_.name + " is now kept out of more directories (limit_keeps_out), so the turn that was running "
              "without them kept out was stopped; the next turn starts keeping them out."
            : config_.name + " is now limited to its directory, so the turn that was running without the limit was "
              "stopped; the next turn starts limited.");
        command(Command::Kind::Write, {}, encode(Stop{state_.openTurn}).line);
        phase_ = Phase::Stopping;
        deadline_ = now + config_.timing.stop;
    }
}

Output Supervisor::setOptions(std::string model, std::string effort, Millis now) {
    config_.model = std::move(model);
    config_.effort = std::move(effort);
    retire(now);     // only: the change opens no turn and takes nothing
    return finish();
}

Output Supervisor::setAccount(std::string account, Millis now) {
    config_.account = std::move(account);
    advance(now);
    return finish();
}

sync::Activity Supervisor::activity() const {
    if (held_) {
        return sync::Activity::Down;
    }
    if (state_.openTurn != 0 || (!state_.waiting.empty() && config_.here) || phase_ == Phase::Starting) {
        return sync::Activity::Busy;
    }
    if (config_.command.empty() || !config_.here || (crashed_ && !down_)) {
        return sync::Activity::Down;
    }
    return sync::Activity::Idle;
}

std::optional<Millis> Supervisor::deadline() const {
    return deadline_;
}

Output Supervisor::prompt(const std::string& text, Millis now) {
    const std::size_t bytes = std::accumulate(state_.waiting.begin(), state_.waiting.end(), std::size_t(0),
        [](std::size_t n, const std::string& w) { return n + w.size(); });
    const Encoded line = encode(sendOf(state_.nextTurn, text));
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

Output Supervisor::queued(const std::string& text, Millis now) {
    queue_.push_back(text);
    if (held_) {
        held_ = false;
        failed_ = 0;
    }
    advance(now);
    return finish();
}

Output Supervisor::signedIn(Millis now) {
    if (held_) {
        held_ = false;
        failed_ = 0;
        advance(now);
    }
    return finish();
}

// A stop ends what runs when it arrives: the open turn, or, while the driver starts, the turn about to begin (its
// prompt is never written; the driver keeps starting for the next). Finding the turn already stopping, or nothing
// running, it changes nothing: no stop is kept for a later turn.
Output Supervisor::stop(Millis now) {
    if (phase_ == Phase::Working) {
        command(Command::Kind::Write, {}, encode(Stop{state_.openTurn}).line);
        phase_ = Phase::Stopping;
        deadline_ = now + config_.timing.stop;
    } else if (phase_ == Phase::Starting && !state_.waiting.empty()) {
        state_.waiting.erase(state_.waiting.begin());
        effect(Effect::What::Stopped, "Stopped.");
        out_.durable = true;
    }
    return finish();
}

Output Supervisor::message(const DriverMessage& message, Millis now) {
    if (phase_ == Phase::None) {
        return finish();    // no driver: nothing it says can arrive
    }
    if (phase_ == Phase::Ending) {
        return finish();    // being ended: its turn is closed already, and what it says is dropped
    }
    if (phase_ == Phase::Starting) {
        const Hello* hello = std::get_if<Hello>(&message);
        const bool canLimit = hello != nullptr &&
            std::find(hello->features.begin(), hello->features.end(), "limit") != hello->features.end();
        if (hello != nullptr && hello->protocol == kProtocolVersion && driverLimited_ && !canLimit) {
            // Started limited, and it does not say it keeps to its directory: it is never trusted to.
            end(config_.name + "'s driver does not support the limit to its directory (its hello lists no \"limit\" "
                "feature), and " + config_.name + " is limited, so it was ended. Install a driver that supports it ("
                + config_.configured + "), or set the friend's \"limit\" to false (docs/accounts.md).", now);
        } else if (hello != nullptr && hello->protocol == kProtocolVersion) {
            phase_ = Phase::Idle;
            crashed_ = false;
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
        out_.account = driverAccount_;
        out_.signedIn = c->billing == Billing::Subscription;
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
        if (state_.session != s->id || state_.sessionDir != driverCwd_) {
            state_.session = s->id;
            state_.sessionDir = driverCwd_;     // the harness keeps it by directory: resumed only there
            out_.durable = true;
        }
    } else if (const Error* e = std::get_if<Error>(&message)) {
        lastCause_ = e->cause.substr(0, cutAt(e->cause, kReportBytes));
        lastFix_ = e->fix.substr(0, cutAt(e->fix, kReportBytes));
        std::string text = e->code == ErrorCode::SessionNotFound
            ? "The earlier conversation with " + config_.name + " could not be resumed, so this is a new one: " +
              sentence(lastCause_)
            : config_.name + "'s driver reports: " + sentence(lastCause_) + " " + sentence(lastFix_);
        if (!e->turn) {
            effect(Effect::What::Notice, std::move(text));
        } else if (*e->turn == open && open != 0) {
            effect(Effect::What::Error, std::move(text), open);
        }
        // an error of a turn not open (the one a driver writes as it refuses a line) is dropped: it exits next
        if (e->code == ErrorCode::UsageLimit) {
            out_.account = driverAccount_;
            out_.usageLimit = true;
        }
        if (e->code == ErrorCode::NoCredential) {
            out_.account = driverAccount_;
            out_.signedIn = false;
            // It ends the open turn, if any, and exits (protocol rule 11): nothing more is written to it, and no
            // driver is started again until the person's next prompt.
            held_ = true;
            noCredential_ = true;
            if (phase_ == Phase::Idle) {
                endQuietly(now);
            }
        }
    } else if (std::holds_alternative<Unknown>(message) || std::holds_alternative<AskSecret>(message)) {
        // a kind this version does not know: skipped; an ask for a secret is the server's (Drivers), not the turn's
    } else if (open == 0 || turnOf(message) != open) {
        end(brokeProtocol("wrote " + kindOf(message) + " for turn " + std::to_string(turnOf(message)) + (open == 0
            ? std::string(" with no turn open") : " while turn " + std::to_string(open) + " is open")), now);
    } else if (const Text* t = std::get_if<Text>(&message)) {
        gathered_ += t->delta;
        // Gathered text past the entry limit goes into the session in whole entries now, so that a driver that never
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
        if (done->outcome == Outcome::Stopped) {
            effect(Effect::What::Stopped, "Stopped.", open);
        }
        if (done->sessionCostUsd) {
            out_.sessionCostUsd = done->sessionCostUsd;
        }
        close(now);
    }
}

Output Supervisor::refused(const std::string& cause, Millis now) {
    if (phase_ != Phase::None && phase_ != Phase::Ending) {
        end(config_.name + "'s driver wrote a line the server refused (" + cause + "), so it was ended. Fix the "
            "driver: each line it writes must be one message of docs/driver-protocol.md.", now);
    }
    advance(now);
    return finish();
}

Output Supervisor::closed(const std::string& what, Millis now) {
    if (phase_ != Phase::None && phase_ != Phase::Ending) {
        end(config_.name + "'s driver closed its " + what + " while it ran, so it was ended. A driver keeps its "
            "standard input and output open until it exits.", now);
    }
    advance(now);
    return finish();
}

Output Supervisor::exited(const std::string& how, Millis now) {
    if (phase_ == Phase::None) {
        return finish();
    }
    if (phase_ != Phase::Ending) {
        abandon(config_.name + "'s driver " + how + ".");
    }
    const bool counted = !finished_ && !held_ && !down_ && !retiring_;
    if (counted) {
        ++failed_;
    }
    phase_ = Phase::None;
    stage_ = Stage::None;
    finished_ = false;
    noCredential_ = false;
    gathered_.clear();
    driverAccount_.clear();
    driverCwd_.clear();
    driverLimited_ = false;
    driverKeepsOut_.clear();
    driverModel_.clear();
    driverEffort_.clear();
    retiring_ = false;
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
    if (!deadline_ || now < *deadline_) {
        return finish();
    }
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
    if (down_) {
        return finish();
    }
    down_ = true;
    if (phase_ == Phase::None || phase_ == Phase::Ending) {
        return finish();
    }
    if (state_.openTurn != 0) {
        end("nexus-server is stopping, so " + config_.name + "'s driver was ended.", now);
    } else {
        endQuietly(now);    // no turn open: nothing in the session
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

// The open turn's gathered text goes into the session as one entry: receive() keeps it within the entry limit.
void Supervisor::flush() {
    if (!gathered_.empty()) {
        effect(Effect::What::Reply, std::move(gathered_), state_.openTurn);
    }
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
    crashed_ = true;
    if (state_.openTurn != 0) {
        cutOpen();
    }
}

void Supervisor::end(const std::string& why, Millis now) {
    abandon(why);
    endQuietly(now);
}

// End the driver with no turn open: its input closed; TERM and KILL follow on deadlines.
void Supervisor::endQuietly(Millis now) {
    phase_ = Phase::Ending;
    stage_ = Stage::Closed;
    command(Command::Kind::CloseInput);
    deadline_ = now + config_.timing.endGrace;
}

// The open turn's done. A driver with no credential exits next, so it is ended rather than given a prompt.
void Supervisor::close(Millis now) {
    state_.openTurn = 0;
    openPrompt_.clear();
    out_.durable = true;
    phase_ = Phase::Idle;
    deadline_.reset();
    finished_ = true;
    failed_ = 0;
    if (noCredential_) {
        endQuietly(now);
    }
}

void Supervisor::startDriver(Millis now) {
    if (phase_ != Phase::None || state_.waiting.empty() || held_ || down_ || !config_.here) {
        return;
    }
    // The harness keeps its sessions by directory: one made in another directory (the session's instance moved) is
    // dropped, and the driver starts afresh; when the conversation has turns, its first send carries a summary of them.
    // A session from before its directory was kept (none recorded) is resumed.
    if (!state_.session.empty() && !state_.sessionDir.empty() && state_.sessionDir != config_.cwd) {
        const bool turns = state_.nextTurn > 1;
        if (turns) {
            effect(Effect::What::Notice, config_.name + " now works in " + config_.cwd + ", and the conversation "
                "so far was held in " + state_.sessionDir + ", where it cannot be resumed from here: " + config_.name +
                " starts afresh, given a summary of it with the next prompt.");
        }
        state_.session.clear();
        state_.sessionDir.clear();
        state_.summary = state_.summary || turns;
        out_.durable = true;
    }
    std::vector<std::string> argv = config_.command;
    if (!argv.empty()) {        // none configured: the start fails, as no program is configured
        argv.insert(argv.end(), {"--cwd", config_.cwd});
        if (!state_.session.empty()) {
            argv.insert(argv.end(), {"--resume", state_.session});
        }
        // Limited to --cwd: a driver that cannot keep the limit refuses to start rather than run unlimited (an
        // older one refuses the argument it does not know), so a limited session never runs without it.
        if (config_.limited) {
            argv.insert(argv.end(), {"--limit", "cwd"});
            for (const std::string& d : config_.keepsOut) {
                argv.insert(argv.end(), {"--keep-out", d});
            }
        }
    }
    command(Command::Kind::Start, std::move(argv));
    phase_ = Phase::Starting;
    stage_ = Stage::None;
    deadline_ = now + config_.timing.hello;
    finished_ = false;
    noCredential_ = false;
    lastCause_.clear();
    lastFix_.clear();
    driverAccount_ = config_.account;
    driverCwd_ = config_.cwd;
    driverLimited_ = config_.limited;
    driverKeepsOut_ = config_.limited ? config_.keepsOut : std::vector<std::string>{};
    driverModel_.clear();
    driverEffort_.clear();
    retiring_ = false;
}

// The oldest waiting prompt, to an idle driver, as the next turn: durable before the line is written.
void Supervisor::writeNext() {
    if (phase_ != Phase::Idle || state_.waiting.empty()) {
        return;
    }
    const std::uint64_t turn = state_.nextTurn;
    // The session's model and effort, where the driver's differ: it applies them from this send on. retire() has
    // ended a driver whose field the session has put back to the harness's default, which this line cannot say, so
    // each field written is one the session has. The hub keeps a setting short and printable, so the line encodes.
    if (driverModel_ != config_.model || driverEffort_ != config_.effort) {
        const driver::Settings settings{driverModel_ != config_.model ? config_.model : std::string(),
            driverEffort_ != config_.effort ? config_.effort : std::string()};
        command(Command::Kind::Write, {}, encode(settings).line);
        driverModel_ = config_.model;
        driverEffort_ = config_.effort;
    }
    // prompt() refused any prompt that does not encode, and a turn number's digits cannot push a prompt within the
    // hub's text limit past the line limit, so this line is always whole.
    // After a move to another directory, the first send carries the summary before the prompt.
    const std::string summary = state_.summary && config_.summarize ? config_.summarize(state_.waiting.front()) :
        std::string();
    if (state_.summary) {
        state_.summary = false;     // durable with the turn, below
    }
    command(Command::Kind::Write, {}, encode(sendOf(turn, summary + state_.waiting.front())).line);
    openPrompt_ = std::move(state_.waiting.front());
    state_.waiting.erase(state_.waiting.begin());
    state_.nextTurn = turn + 1;
    state_.openTurn = turn;
    out_.durable = true;
    phase_ = Phase::Working;
}

// With a driver configured, no turn open, nothing waiting and starting not held back, the oldest queued prompt leaves
// the queue and waits, shown as the person's entry: one per turn. One that cannot be written as a protocol line (the
// hub's text limit keeps every prompt far within the line limit, so only text that is not UTF-8) leaves the queue with
// a notice, and the next is taken in its place.
void Supervisor::take() {
    while (!config_.command.empty() && state_.openTurn == 0 && state_.waiting.empty() && !held_ && !down_ &&
           config_.here && !queue_.empty()) {
        std::string text = std::move(queue_.front());
        queue_.erase(queue_.begin());
        ++out_.taken;
        const Encoded line = encode(sendOf(state_.nextTurn, text));
        if (!line.error.empty()) {
            effect(Effect::What::Notice, "A queued prompt to " + config_.name + " was not sent: " +
                sentence(line.error));
            continue;
        }
        effect(Effect::What::Prompt, text);
        state_.waiting.push_back(std::move(text));
        out_.durable = true;
    }
}

// With no turn open, a driver started under an account that is no longer the session's is ended quietly: the next one
// starts under the session's account. So is one started in a directory that is no longer the session's, or any while
// the session's instance is on another machine, or one started under another limit than the session's now (limited or
// not, or limited keeping out other directories). So is one
// told a model or effort the session has since put back to the
// harness's default, which a settings line cannot say: the next one starts on the defaults. A turn open runs to its
// end first.
void Supervisor::retire(Millis now) {
    const bool cleared = (!driverModel_.empty() && config_.model.empty()) ||
        (!driverEffort_.empty() && config_.effort.empty());
    if (phase_ == Phase::Idle && (driverAccount_ != config_.account || driverCwd_ != config_.cwd || !config_.here ||
                                  driverLimited_ != config_.limited ||
                                  (driverLimited_ && driverKeepsOut_ != config_.keepsOut) || cleared)) {
        retiring_ = true;
        endQuietly(now);
    }
}

// A send of turn `turn`.
driver::Send Supervisor::sendOf(std::uint64_t turn, const std::string& prompt) const {
    return driver::Send{turn, prompt};
}

// What follows every event: the oldest queued prompt taken at a turn's end, a driver under an old account ended, a
// driver started for a waiting prompt, or the prompt written to an idle one.
void Supervisor::advance(Millis now) {
    take();
    retire(now);
    startDriver(now);
    writeNext();
}

Output Supervisor::finish() {
    Output o = std::move(out_);
    out_ = Output{};
    return o;
}

} // namespace nexus::server
