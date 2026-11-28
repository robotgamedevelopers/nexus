#include "server/supervisor.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <utility>
#include <variant>

namespace nexus::server {

namespace {

using namespace nexus::driver;

std::string_view expectedEmail(const std::vector<std::string>& command) {
    const auto flag = std::find(command.begin(), command.end(), "--account-email");
    return flag != command.end() && std::next(flag) != command.end() ? std::string_view(*std::next(flag)) : std::string_view();
}

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
        } else if constexpr (std::is_same_v<M, Quota>) {
            return "quota";
        } else if constexpr (std::is_same_v<M, ChildAgents>) {
            return "child-agents";
        } else if constexpr (std::is_same_v<M, Tool>) {
            return "tool";
        } else if constexpr (std::is_same_v<M, Done>) {
            return "done";
        } else if constexpr (std::is_same_v<M, Error>) {
            return "error";
        } else if constexpr (std::is_same_v<M, AskSecret>) {
            return "ask-secret";
        } else if constexpr (std::is_same_v<M, Queue>) {
            return "queue";
        } else if constexpr (std::is_same_v<M, Settled>) {
            return "settled";
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
                      std::is_same_v<M, Done> || std::is_same_v<M, Queue> || std::is_same_v<M, Settled>) {
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

bool names(const Hello& hello, std::string_view feature) {
    return std::find(hello.features.begin(), hello.features.end(), feature) != hello.features.end();
}

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string quoted(const std::vector<std::string>& command) {
    return command.empty() ? std::string("(no program)") : "`" + command.front() + "`";
}

} // namespace

EntryKind entryKind(Effect::What what) {
    switch (what) {
        case Effect::What::Prompt: return EntryKind::Person;
        case Effect::What::Reply: return EntryKind::AI;
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
        clearContinuation();     // the interrupted send may already have run in its provider
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
// (Stop, then the stop deadline and the kill), so no driver runs with a weaker limit than its AI's for longer than
// the stop takes; once it is done the driver is ended (retire) and the next starts limited. Loosened: the open turn
// finishes as it started. Models: models/Supervisor.tla SetLimit, NeverWeaker.
Output Supervisor::setLimited(bool limited, Millis now) {
    config_.limited = limited;
    stopWeaker(now);
    advance(now);
    return finish();
}

// The AI taken out: an open turn gets the person's stop (its done, or the stop's deadline, ends it; Advance then ends
// the driver, as Retire ends any driver while the AI is out); a driver still starting has no turn open, and is
// ended at once, never counted as a failed start. Put back: what waits goes on.
Output Supervisor::setPresent(bool present, Millis now) {
    if (present == config_.present) {
        return finish();
    }
    config_.present = present;
    if (!present && phase_ == Phase::Working) {
        stopOpen(now);
    } else if (!present && phase_ == Phase::Starting) {
        retiring_ = true;
        endQuietly(now);
    }
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

bool Supervisor::awaitingRotation() const {
    return !state_.continuationPrompt.empty() && state_.openTurn == 0 && !state_.modelLimitPending &&
        std::find(state_.exhaustedAccounts.begin(), state_.exhaustedAccounts.end(), config_.account) !=
        state_.exhaustedAccounts.end();
}

void Supervisor::clearContinuation() {
    state_.continuationPrompt.clear();
    state_.exhaustedAccounts.clear();
    state_.modelLimitPending = false;
}

void Supervisor::acceptModelChoice(const sync::DriverState& durable) {
    if (state_.modelLimitPending && !durable.modelLimitPending) {
        state_.modelLimitPending = false;
        state_.exhaustedAccounts = durable.exhaustedAccounts;
    }
}

Output Supervisor::rotationExhausted() {
    if (awaitingRotation()) {
        if (!config_.autoAccount)
            effect(Effect::What::Notice, "The selected account " + config_.account +
                " already reached its limit on this request. The interrupted work stopped.");
        clearContinuation();
        held_ = true;
        out_.durable = true;
    }
    return finish();
}

Output Supervisor::resetContext(bool compact, Millis now) {
    const auto next = state_.nextTurn;
    state_ = {};
    state_.nextTurn = next;
    state_.summary = compact;
    clearSteering();     // the conversation starts over: nothing written into its old turn goes on
    gathered_.clear();
    openPrompt_.clear();
    if (phase_ != Phase::None && phase_ != Phase::Ending) {
        retiring_ = true;
        endQuietly(now);
    }
    out_.durable = true;
    return finish();
}

Output Supervisor::refreshSandbox(bool tightened, Millis now) {
    if (phase_ == Phase::None || phase_ == Phase::Ending) return finish();
    sandboxChanged_ = true;
    if (phase_ == Phase::Starting) {
        retiring_ = true;
        endQuietly(now);
    } else if (tightened && phase_ == Phase::Working) {
        stopOpen(now);
    }
    advance(now);
    return finish();
}

Output Supervisor::retireIdleLogin(Millis now) {
    if (phase_ == Phase::Idle) {
        retiring_ = true;
        endQuietly(now);
    }
    return finish();
}

Output Supervisor::setAccount(std::string account, Millis now, bool automatic,
                              const std::vector<std::string>* command) {
    // Changing the account may start queued work. Bind its verified launch arguments first.
    if (command) config_.command = *command;
    // A named selection can finish a pending handoff. advance() checks the new payer against
    // the checkpoint, just as recovery does when the selection was committed before a restart.
    config_.autoAccount = automatic;
    config_.account = std::move(account);
    advance(now);
    return finish();
}

sync::Activity Supervisor::activity() const {
    if (held_) {
        return sync::Activity::Down;
    }
    if (childAgents_) return sync::Activity::Working;
    if (state_.openTurn != 0 || ((!state_.waiting.empty() || !state_.continuationPrompt.empty()) &&
            !state_.modelLimitPending && config_.here && config_.present) ||
        phase_ == Phase::Starting) {
        return sync::Activity::Busy;
    }
    if (config_.command.empty() || !config_.here || !config_.present || (crashed_ && !down_)) {
        return sync::Activity::Down;
    }
    return sync::Activity::Idle;
}

std::optional<Millis> Supervisor::deadline() const {
    return deadline_;
}

// Models: models/Steering.tla Held, Fits.
std::string Supervisor::fits(const std::string& text) const {
    std::size_t count = state_.waiting.size();
    std::size_t bytes = std::accumulate(state_.waiting.begin(), state_.waiting.end(), std::size_t(0),
        [](std::size_t n, const std::string& w) { return n + w.size(); });
    std::size_t intoTurn = 0;
    for (const Written& w : written_) {
        if (!taken(w.id)) {
            ++count;
            ++intoTurn;
            bytes += w.text.size();
        }
    }
    const std::string held = intoTurn == 0 ? std::string() : " (counting " + plural(intoTurn, "message", "messages") +
        " written into the turn that runs, which wait again if it ends before taking them in)";
    if (count >= config_.maxWaiting) {
        return std::to_string(count) + (intoTurn == 0 ? " prompts" : " messages") + " already wait for " +
            config_.name + held + ", the limit";
    }
    if (bytes + text.size() > config_.maxWaitingBytes) {
        return std::string(intoTurn == 0 ? "the prompts" : "the messages") + " waiting for " + config_.name + held +
            " would hold more than " + std::to_string(config_.maxWaitingBytes / 1024) + " KiB";
    }
    return {};
}

Output Supervisor::prompt(const std::string& text, Millis now) {
    const Encoded line = encode(sendOf(state_.nextTurn, text));
    const std::string notSent = "A prompt to " + config_.name + " was not sent: ";
    if (const std::string full = fits(text); !full.empty()) {
        effect(Effect::What::Notice, notSent + full + ". Send it again once " + config_.name + " has answered.");
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
        stopOpen(now);
    } else if (state_.openTurn == 0 && !state_.continuationPrompt.empty()) {
        clearContinuation();
        effect(Effect::What::Stopped, "Stopped.");
        out_.durable = true;
        advance(now);
    } else if (phase_ == Phase::Starting && !state_.waiting.empty()) {
        state_.waiting.erase(state_.waiting.begin());
        effect(Effect::What::Stopped, "Stopped.");
        out_.durable = true;
    }
    return finish();
}

// The stop of the open turn, once: its line to the driver, and the deadline for its done.
void Supervisor::stopOpen(Millis now) {
    command(Command::Kind::Write, {}, encode(Stop{state_.openTurn}).line);
    phase_ = Phase::Stopping;
    deadline_ = now + config_.timing.stop;
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
                + config_.configured + "), or set the AI's \"limit\" to false (docs/accounts.md).", now);
        } else if (hello != nullptr && hello->protocol == kProtocolVersion &&
                   (names(*hello, kFeatureSteer) || names(*hello, kFeatureFollowUp)) && !names(*hello, kFeatureSettled)) {
            // Without settled the server cannot tell a turn's end from one model run's, nor what the driver never took.
            end(config_.name + "'s driver names \"" + std::string(names(*hello, kFeatureSteer) ? kFeatureSteer :
                kFeatureFollowUp) + "\" in its hello without \"settled\", which a driver that takes steers or "
                "follow-ups must also name, so it was ended. Fix the driver (docs/driver-protocol.md, \"Steering a "
                "turn\"), or install one that goes with this nexus-server (" + config_.configured + ").", now);
        } else if (hello != nullptr && hello->protocol == kProtocolVersion) {
            // Only what it named is ever written to it (models/Steering.tla NamedOnly); a feature this server does
            // not know is skipped.
            driverSteers_ = names(*hello, kFeatureSteer);
            driverFollowsUp_ = names(*hello, kFeatureFollowUp);
            driverSettles_ = names(*hello, kFeatureSettled);
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
    if (steering(message, open, now)) {
        return;
    }
    if (std::holds_alternative<Hello>(message)) {
        end(brokeProtocol("wrote a second hello"), now);
    } else if (const auto* q = std::get_if<Quota>(&message)) {
        out_.account = driverAccount_;
        if (out_.quota.size() < 16 || out_.quota.contains(q->minutes))
            out_.quota[q->minutes] = q->usedPercent;
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
        if (state_.session != s->id || state_.sessionDir != driverCwd_ || state_.sessionAccount != driverAccount_) {
            state_.session = s->id;
            state_.sessionAccount = driverAccount_;
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
        const bool current = !e->turn || (*e->turn == open && open != 0);
        const bool accountLimit = e->code == ErrorCode::UsageLimit;
        const bool modelLimit = e->code == ErrorCode::ModelLimit;
        // An in-flight turn finishes on its original payer after a selection changes. A cap
        // reported by that turn hands the unfinished work to the payer already selected.
        const bool replacementSelected = !config_.account.empty() && config_.account != driverAccount_;
        const bool checkpoint = current && phase_ == Phase::Working && !driverAccount_.empty() &&
            !config_.account.empty() && (modelLimit || (accountLimit && (config_.autoAccount || replacementSelected)));
        if (checkpoint) {
            flush();
        }
        if (!e->turn) {
            effect(Effect::What::Notice, std::move(text));
        } else if (*e->turn == open && open != 0) {
            effect(Effect::What::Error, std::move(text), open);
        }
        // an error of a turn not open (the one a driver writes as it refuses a line) is dropped: it exits next
        if (checkpoint) {
            state_.continuationPrompt = openPrompt_;
            if (std::find(state_.exhaustedAccounts.begin(), state_.exhaustedAccounts.end(), driverAccount_) ==
                state_.exhaustedAccounts.end()) {
                state_.exhaustedAccounts.push_back(driverAccount_);
            }
            state_.modelLimitPending = modelLimit && !replacementSelected;
            cutWritten();
            state_.openTurn = 0;
            openPrompt_.clear();
            out_.durable = true;
            retiring_ = true;
            endQuietly(now);    // no further done or output from this provider can dispatch the waiting work
        }
        if (accountLimit && current && phase_ != Phase::Stopping) {
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
    } else if (const auto* agents = std::get_if<ChildAgents>(&message)) {
        childAgents_ = agents->count;
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

// The steering lines (queue, settled), and the rules they put on the rest of a turn: nothing of the turn after its
// settled but its done, and no done before settled from a driver that named it. True when the message was handled
// here. Models: models/Steering.tla Receive (SoundQueue, ServerKeptRules).
bool Supervisor::steering(const DriverMessage& message, std::uint64_t open, Millis now) {
    const std::string turnText = open == 0 ? std::string(" with no turn open") : " while turn " +
        std::to_string(open) + " is open";
    if (const Queue* q = std::get_if<Queue>(&message)) {
        if (!driverSteers_ && !driverFollowsUp_) {
            end(brokeProtocol("wrote queue, though its hello named neither steer nor follow-up"), now);
        } else if (open == 0 || q->turn != open) {
            end(brokeProtocol("wrote queue for turn " + std::to_string(q->turn) + turnText), now);
        } else if (settled_) {
            end(brokeProtocol("wrote queue for turn " + std::to_string(open) + " after its settled"), now);
        } else {
            // Each id one written into this turn as that kind, and not one the harness already took in: an id leaves
            // the queue only when it is taken in.
            auto sound = [this](const std::vector<std::uint64_t>& ids, bool followUp) -> std::string {
                for (const std::uint64_t id : ids) {
                    const auto w = std::find_if(written_.begin(), written_.end(), [&](const Written& x) {
                        return x.id == id && x.followUp == followUp;
                    });
                    if (w == written_.end()) {
                        return "reported " + std::string(followUp ? "follow-up " : "steer ") + std::to_string(id) +
                            ", which the server did not write into turn " + std::to_string(state_.openTurn) +
                            " as one";
                    }
                    if (taken(id)) {
                        return "reported " + std::string(followUp ? "follow-up " : "steer ") + std::to_string(id) +
                            " again after its harness took it in";
                    }
                }
                return {};
            };
            std::string bad = sound(q->steer, false);
            if (bad.empty()) {
                bad = sound(q->followUp, true);
            }
            if (!bad.empty()) {
                end(brokeProtocol(bad), now);
            } else {
                reported_ = *q;
                everReported_.insert(q->steer.begin(), q->steer.end());
                everReported_.insert(q->followUp.begin(), q->followUp.end());
            }
        }
        return true;
    }
    if (const Settled* st = std::get_if<Settled>(&message)) {
        if (!driverSettles_) {
            end(brokeProtocol("wrote settled, though its hello did not name settled"), now);
        } else if (open == 0 || st->turn != open) {
            end(brokeProtocol("wrote settled for turn " + std::to_string(st->turn) + turnText), now);
        } else if (settled_) {
            end(brokeProtocol("wrote a second settled for turn " + std::to_string(open)), now);
        } else {
            settled_ = true;
        }
        return true;
    }
    const Error* error = std::get_if<Error>(&message);
    const std::uint64_t turn = error != nullptr ? error->turn.value_or(0) : turnOf(message);
    if (open == 0 || turn != open) {
        return false;
    }
    if (settled_ && !std::holds_alternative<Done>(message)) {
        end(brokeProtocol("wrote " + kindOf(message) + " for turn " + std::to_string(open) + " after its settled, "
            "where only its done may follow"), now);
        return true;
    }
    if (std::holds_alternative<Done>(message) && driverSettles_ && !settled_) {
        end(brokeProtocol("wrote done for turn " + std::to_string(open) + " before its settled"), now);
        return true;
    }
    return false;
}

bool Supervisor::taken(std::uint64_t id) const {
    return everReported_.contains(id) &&
        std::find(reported_.steer.begin(), reported_.steer.end(), id) == reported_.steer.end() &&
        std::find(reported_.followUp.begin(), reported_.followUp.end(), id) == reported_.followUp.end();
}

// At the turn's done: each written into it that the harness never took in waits again, first, in the order written
// (models/Steering.tla Close, NeverLost).
void Supervisor::giveBack() {
    std::vector<std::string> back;
    for (const Written& w : written_) {
        if (!taken(w.id)) {
            back.push_back(w.text);
        }
    }
    if (!back.empty()) {
        effect(Effect::What::Notice, plural(back.size(), "message", "messages") + " written into the turn " +
            (back.size() == 1 ? "was" : "were") + " not taken in by " + config_.name + " before it ended, so " +
            (back.size() == 1 ? "it goes" : "they go, in order,") + " as the next prompt.");
        state_.waiting.insert(state_.waiting.begin(), std::make_move_iterator(back.begin()),
            std::make_move_iterator(back.end()));
        out_.durable = true;
    }
    clearSteering();
}

// The turn is cut: what was written into it and not known taken in is cut with it, never written again
// (models/Steering.tla Exited, CutOnlyOnExit).
void Supervisor::cutWritten() {
    std::size_t lost = 0;
    for (const Written& w : written_) {
        lost += taken(w.id) ? 0 : 1;
    }
    if (lost != 0) {
        effect(Effect::What::Cut, plural(lost, "message", "messages") + " written into the turn may not have reached " +
            config_.name + "; send again what you still want it to have.", state_.openTurn);
    }
    clearSteering();
}

void Supervisor::clearSteering() {
    written_.clear();
    reported_ = Queue{};
    everReported_.clear();
    settled_ = false;
}

Output Supervisor::steer(const std::string& text, Millis now) {
    return into(text, false, now);
}

Output Supervisor::followUp(const std::string& text, Millis now) {
    return into(text, true, now);
}

// Into the open turn when the driver takes that kind and the turn is open, not stopping and not settled, and it fits
// the waiting limits, since it waits again if the turn ends before taking it in (models/Steering.tla Steer, CanSteer,
// Fits); otherwise a prompt for the next turn, which refuses it with a notice when it does not fit.
Output Supervisor::into(const std::string& text, bool followUp, Millis now) {
    const bool takes = followUp ? driverFollowsUp_ : driverSteers_;
    if (phase_ == Phase::Working && takes && !settled_ && written_.size() < kMaxQueued && fits(text).empty()) {
        const std::uint64_t id = nextWritten_;
        const Encoded line = followUp ? encode(FollowUp{state_.openTurn, id, text})
                                      : encode(driver::Steer{state_.openTurn, id, text});
        if (!line.error.empty()) {
            effect(Effect::What::Notice, "A message to " + config_.name + " was not sent: " + sentence(line.error));
            return finish();
        }
        ++nextWritten_;
        effect(Effect::What::Prompt, text, state_.openTurn);
        written_.push_back(Written{id, followUp, text});
        command(Command::Kind::Write, {}, line.line);
        return finish();
    }
    if (!followUp && phase_ == Phase::Working && !driverSteers_) {
        effect(Effect::What::Notice, config_.name + "'s driver does not take a message into a turn that runs (its "
            "hello names no \"steer\"), so this one waits for the next turn.");
    } else if (phase_ == Phase::Working && takes && !settled_ && written_.size() >= kMaxQueued) {
        effect(Effect::What::Notice, std::to_string(kMaxQueued) + " messages were already written into this turn, "
            "the most one turn takes, so this one waits for the next turn.");
    }
    return prompt(text, now);
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
    childAgents_ = 0;
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
    driverSteers_ = false;
    driverFollowsUp_ = false;
    driverSettles_ = false;
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
    cutWritten();
    effect(Effect::What::Cut, "The reply was cut short. Whether " + config_.name + " got the prompt is unknown; send "
        "it again if you still want an answer.", state_.openTurn);
    state_.openTurn = 0;
    openPrompt_.clear();
    clearContinuation();
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
    giveBack();
    state_.openTurn = 0;
    openPrompt_.clear();
    clearContinuation();
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
    if (phase_ != Phase::None || (state_.waiting.empty() && state_.continuationPrompt.empty()) || held_ || down_ || !config_.here || !config_.present) {
        return;
    }
    childAgents_ = 0;
    // A harness resume ID belongs to the account that reported it. A legacy ID with no recorded owner
    // cannot be attributed after an earlier account switch. Seed a fresh session from the Nexus transcript.
    const bool accountChanged = state_.sessionAccount != config_.account;
    const bool dirChanged = !state_.sessionDir.empty() && state_.sessionDir != config_.cwd;
    if (!state_.session.empty() && (accountChanged || dirChanged)) {
        const bool turns = state_.nextTurn > 1;
        if (turns) {
            std::string reason;
            if (accountChanged) {
                reason = state_.sessionAccount ? "The previous harness session belongs to a different account. " :
                    "The previous harness session has no recorded account. ";
            } else {
                reason = config_.name + " now works in " + config_.cwd + ", and the conversation so far was held in " +
                    state_.sessionDir + ", where it cannot be resumed from here: ";
            }
            effect(Effect::What::Notice, reason + config_.name +
                " starts afresh, given a summary of it with the next prompt.");
        }
        state_.session.clear();
        state_.sessionDir.clear();
        state_.sessionAccount.reset();
        state_.summary = state_.summary || turns;
        out_.durable = true;
    }
    std::vector<std::string> argv = config_.command;
    if (!argv.empty()) {        // none configured: the start fails, as no program is configured
        // A queued prompt can start as settings are applied. Bind the argument from the same account we record
        // as the owner of this process, even if the previous resolved command named a different account.
        for (std::size_t i = 1; i + 1 < argv.size(); ++i) {
            if (argv[i] == "--account") {
                argv[++i] = config_.account;
            }
        }
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
    driverEmail_ = expectedEmail(config_.command);
    driverCwd_ = config_.cwd;
    sandboxChanged_ = false;
    driverLimited_ = config_.limited;
    driverKeepsOut_ = config_.limited ? config_.keepsOut : std::vector<std::string>{};
    driverModel_.clear();
    driverEffort_.clear();
    driverSteers_ = false;
    driverFollowsUp_ = false;
    driverSettles_ = false;
    retiring_ = false;
}

// The oldest waiting prompt, to an idle driver, as the next turn: durable before the line is written.
void Supervisor::writeNext() {
    if (phase_ != Phase::Idle || (state_.waiting.empty() && state_.continuationPrompt.empty())) {
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
    // After dropping a harness session, the first send carries the summary before the prompt.
    std::string prefix;
    if (!state_.continuationPrompt.empty()) {
        if (config_.continuationContext) {
            prefix = config_.continuationContext();
        }
        prefix += "[nexus: Continue the unfinished request below from the point where the provider stopped. "
            "Use the recorded replies and tool activity as progress. Check the current workspace or external state "
            "before repeating any operation whose outcome is uncertain. Do not repeat completed work.]\n\n";
        openPrompt_ = state_.continuationPrompt;
    } else {
        if (state_.summary && config_.summarize) {
            prefix = config_.summarize(state_.waiting.front());
        }
        openPrompt_ = std::move(state_.waiting.front());
        state_.waiting.erase(state_.waiting.begin());
    }
    state_.summary = false;
    command(Command::Kind::Write, {}, encode(sendOf(turn, prefix + openPrompt_)).line);
    state_.nextTurn = turn + 1;
    state_.openTurn = turn;
    out_.durable = true;
    phase_ = Phase::Working;
}

// With a driver configured, the AI in ais.json, no turn open, nothing waiting and starting not held back, the
// oldest queued prompt leaves the queue and waits, shown as the person's entry: one per turn. One that cannot be
// written as a protocol line (the hub's text limit keeps every prompt far within the line limit, so only text that is
// not UTF-8) leaves the queue with a notice, and the next is taken in its place.
void Supervisor::take() {
    while (!config_.command.empty() && state_.openTurn == 0 && state_.waiting.empty() && !held_ && !down_ &&
           config_.here && config_.present && !queue_.empty()) {
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
// not, or limited keeping out other directories). So is one told a model or effort the session has since put back to
// the harness's default, which a settings line cannot say: the next one starts on the defaults. So is any while the
// AI is not in ais.json. A turn open runs to its end first.
void Supervisor::retire(Millis now) {
    const bool changedIdentity = driverEmail_ != expectedEmail(config_.command);
    if (changedIdentity && driverAccount_ == config_.account && phase_ == Phase::Working) {
        stopOpen(now);
    }
    const bool cleared = (!driverModel_.empty() && config_.model.empty()) ||
        (!driverEffort_.empty() && config_.effort.empty());
    if (phase_ == Phase::Idle && (sandboxChanged_ || changedIdentity || driverAccount_ != config_.account || driverCwd_ != config_.cwd || !config_.here ||
                                  driverLimited_ != config_.limited ||
                                  (driverLimited_ && driverKeepsOut_ != config_.keepsOut) || !config_.present ||
                                  cleared)) {
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
    if (state_.modelLimitPending || awaitingRotation() ||
        (!state_.continuationPrompt.empty() && config_.command.empty())) {
        return;
    }
    if (state_.continuationPrompt.empty()) {
        take();
    }
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
