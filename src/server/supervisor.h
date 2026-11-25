// The driver supervisor: what nexus-server does with one session's driver process (docs/driver-protocol.md). It
// starts a driver when a prompt waits and none is alive, writes each prompt to it once as the next turn, gathers the
// driver's replies into the session, stops a turn, and ends a driver that breaks the protocol, misses a deadline or
// exits.
//
// A pure state machine: no processes, pipes, files or clock. Each event (a prompt, a stop, a message from the driver, a
// line refused, a pipe closed, the driver's exit, a tick of the clock, the server stopping) takes the time as an
// argument and returns an Output: the effects on the session (entries to append, and the durable state when it
// changed), then the commands for the caller to carry out (start a driver, write a line, close its input, signal it).
// The caller commits the effects with the durable state in one step of the store first, and only then carries out the
// commands: so a turn number is durable before its send is written. models/Supervisor.tla is the model; tests/server/
// supervisor_walk.cpp binds this code to it.
//
// The rules (the model's properties): at most one driver alive and one turn open; turns numbered from 1, strictly
// increasing and never reused across driver and server restarts; each prompt written at most once, never again after
// its turn is cut; a prompt that arrives while the driver cannot take it waits, in order, up to a bound; a message for
// a turn that is not open has no effect on the session; a driver that exits or is ended with a turn open has that turn
// cut, with a notice that whether the AI got the prompt is unknown; consecutive failed starts stop the starting of
// drivers until the person's next prompt. There is no deadline on a working turn: a turn may run for hours. Each send
// carries the session's model and effort as they are when it is written, so a change of them never reaches a turn
// already open; a change of account ends the driver once no turn is open, never counted as a failed start, and the next
// starts under the new account.
//
// The session's queue (prompts queued for later, Command-Enter) is the hub's, apart from the transcript; the supervisor
// holds the same list, as it holds the driver's durable state, and changes it only in steps the caller commits whole.
// When no turn is open, no prompt waits and starting is not held back, the oldest queued prompt leaves the queue and
// waits like a prompt sent (shown as the person's entry): one per turn, oldest first, each once. A stop is written
// only for the open turn, at most once for it, and never waits behind anything.
//
// Steering (docs/driver-protocol.md, "Steering a turn"; models/Steering.tla, bound to this code by tests/driver/
// steering_walk.cpp): a message the person writes into a turn that runs goes to the driver as a steer or a follow-up
// only when the driver's hello named that kind (and settled), the turn is open, not being stopped and not settled;
// otherwise it waits as the next prompt, never a kind the driver did not name. The supervisor keeps each written into
// the open turn and the driver's last whole queue report; an id that leaves the reported queue was taken in by the
// harness. A driver that named settled writes settled before the turn's done: the next send still waits for the done
// (sequence rule 3). At the done, each written into the turn that the driver never took in waits again, first, in the
// order written; at a cut (the driver gone), it is cut with the turn. This state is the server's alone, in memory: a
// turn open across a restart is cut, and with it what was written into it.
//
// The open turn's text not yet in the session (gathered()) is the AI's text block as it grows: the caller shows it
// as an entry still open, kept in memory only. It goes into the session, durable, when the block ends (its text-end, a
// tool, the turn's done or cut) or reaches the entry limit; a server that crashes loses at most that one block, and the
// turn is cut with its notice when the server starts again.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "driver/protocol.h"
#include "model.h"
#include "sync/hub.h"

namespace nexus::server {

using Millis = std::uint64_t;   // a time, in milliseconds, from any fixed start

// The deadlines. Each is a judgement, not measured: a driver writes hello before it starts its harness, so ten
// seconds is generous; a harness answers a stop within seconds when it is alive; the grace periods let a driver and
// its harness save what they hold before they are signalled.
struct Timing {
    Millis hello = 10'000;      // from the start to the driver's hello
    Millis stop = 10'000;       // from a stop to its turn's done
    Millis endGrace = 2'000;    // from closing the driver's input to SIGTERM
    Millis termGrace = 2'000;   // from SIGTERM to SIGKILL
};

// Consecutive failed starts (the driver ended before it finished a turn) after which no driver is started for the
// session until the person's next prompt. A judgement, not measured: three tries outlast a passing fault.
constexpr int kMaxFailedStarts = 3;

// The most of a driver's error cause, and of its fix, that goes into the session: one line of the driver's may be 1
// MiB, and an entry stays far within the hub's text limit.
constexpr std::size_t kReportBytes = 1024;

// Which AI, how to start its driver, and the bounds.
struct Config {
    std::string name;                   // the AI's name, as the notices say it
    std::vector<std::string> command;   // the driver's program and its arguments, before --cwd and --resume; empty
                                        // while the AI has no driver configured, and then nothing is taken
    std::string cwd;                    // the directory the harness works in
    std::string configured;             // where the driver is configured, for the fixes notices give
    std::string model;                  // the session's model and effort, written with each send (empty: the
    std::string effort;                 // harness's default)
    std::string account;                // the session's account: a driver runs under the one it was started with
    // What goes before the prompt of the first send after the session changed account or directory, where its harness
    // session cannot be resumed: a summary of the conversation so far (Drivers gives it from the transcript). Called
    // only then, with the prompt it goes before (already the transcript's, which the summary leaves out); unset,
    // nothing goes before it.
    std::function<std::string(const std::string& prompt)> summarize;
    bool here = true;                   // the session's instance is on this machine (setPlace): else no driver starts
    // The driver starts limited to its directory (`--limit cwd`, docs/driver-protocol.md): decided by the server
    // (accounts::limitedFor) and given at each start. Tightened, an open turn is stopped and the driver ended; loosened,
    // the driver is ended once no turn is open. Limited unless the server says otherwise: the safe direction.
    bool limited = true;
    // More directories a limited driver keeps the AI out of (ais.json "limit_keeps_out", accounts::Registry
    // keepsOut), given at each limited start as `--keep-out <dir>`, each once. Only ever more kept out.
    std::vector<std::string> keepsOut{};
    bool present = true;                // the session's AI is in ais.json (setPresent): else nothing runs
    Timing timing;
    int maxFailedStarts = kMaxFailedStarts;
    std::size_t maxWaiting = 8;             // prompts waiting, at most (the hub's queue limits)
    std::size_t maxWaitingBytes = 32 * 1024;
    bool autoAccount = false;
    std::function<std::string()> continuationContext{};
    std::size_t maxEntryBytes = 32 * 1024;  // the hub's text limit: longer text is split into several entries
};

// The model's phases: no driver; started, waiting for its hello; idle; a turn open; a stop written for the open turn,
// waiting for its done; ending (input closed, then SIGTERM, then SIGKILL, each after its grace period), waiting for
// the exit.
enum class Phase { None, Starting, Idle, Working, Stopping, Ending };
enum class Stage { None, Closed, Termed, Killed };

// What the caller does, in order, once the effects are durable.
struct Command {
    enum class Kind { Start, Write, CloseInput, Terminate, Kill };
    Kind kind = Kind::Start;
    std::vector<std::string> argv;      // Start: the program and its arguments
    std::string line;                   // Write: one protocol line, its newline included
};

// An entry to append to the session: the person's prompt; the AI's text; a line for a tool the AI used; an
// error the driver reported for the turn; the turn stopped; the turn cut short; any other notice. `turn` is the turn it
// belongs to (0 for none).
struct Effect {
    enum class What { Prompt, Reply, Tool, Error, Stopped, Cut, Notice };
    What what = What::Notice;
    std::string text;
    std::uint64_t turn = 0;
};

// The kind of transcript entry an effect is.
EntryKind entryKind(Effect::What what);

struct Output {
    std::vector<Effect> effects;
    bool durable = false;               // the durable state changed: commit Supervisor::state() with the effects
    std::vector<Command> commands;
    std::optional<driver::Credential> credential;   // the driver reported the credential it uses
    // What the driver said of its account's login, for the hub (Hub::reportSignedIn, Hub::rotate): signed in (a
    // subscription credential), not usable (no credential, or not a subscription), or at its usage limit. `account` is
    // the one the driver alive was started under (empty: none).
    std::optional<bool> signedIn;
    bool usageLimit = false;
    std::string account;
    std::optional<double> sessionCostUsd;           // a turn's done reported the session's cost so far
    std::size_t taken = 0;      // prompts taken from the head of the session's queue: commit as many dequeues, first
};

class Supervisor {
public:
    // The supervisor of a session whose durable state is `state`, as the server finds it when it starts: a turn left
    // open by the server before is cut, and a waiting prompt starts a driver. `queue` is the session's queue as the
    // store holds it: with no turn open and nothing waiting, its oldest prompt is taken. What that takes is written to
    // `out`.
    Supervisor(Config config, sync::DriverState state, Millis now, Output* out, std::vector<std::string> queue = {});

    // The person sent `text`: it is shown and waits, and is written as the next turn as soon as a driver is idle (a
    // driver is started for it if none is alive). Refused with a notice past the bound, or if it cannot be written as
    // a protocol line. A prompt lifts the hold that failed starts put on starting drivers.
    Output prompt(const std::string& text, Millis now);
    // The person queued `text` for later, which the hub has put at the end of the session's queue (within its limits):
    // it is taken at once if no turn is open and nothing waits. Like a prompt, it lifts the hold on starting drivers.
    Output queued(const std::string& text, Millis now);
    // The session's account signed in (its login arrived through the secret dialog, docs/secrets.md): like a prompt,
    // it lifts the hold on starting drivers, without a restart, so what waits goes on (models/Supervisor.tla SignIn).
    // Nothing when starting is not held back.
    Output signedIn(Millis now);
    Output resetContext(bool compact, Millis now);
    Output refreshSandbox(bool tightened, Millis now);
    Output retireIdleLogin(Millis now);
    // The person wrote `text` into the turn that runs: a steer (taken in after the tool calls running, before the next
    // model call) or a follow-up (taken in only when the turn would otherwise settle). Written to the driver when it
    // takes that kind and the turn is open, not stopping and not settled; otherwise it is a prompt (prompt()), waiting
    // for the next turn, with a notice when a steer could not be taken because the driver does not take steers.
    Output steer(const std::string& text, Millis now);
    Output followUp(const std::string& text, Millis now);
    // The person stopped the AI's work: a stop for the open turn, if one is open and no stop is out; while the
    // driver starts, the turn about to begin is cancelled. Otherwise nothing: no stop is kept for a later turn.
    Output stop(Millis now);
    // A message the driver wrote.
    Output message(const driver::DriverMessage& message, Millis now);
    // A line of the driver's the reader refused, with its cause: the driver is ended.
    Output refused(const std::string& cause, Millis now);
    // The driver's output or input closed while it ran (`what` says which): the driver is ended.
    Output closed(const std::string& what, Millis now);
    // The driver exited (`how` says how: its status, its signal, or why it could not start).
    Output exited(const std::string& how, Millis now);
    // The clock: a deadline at or before `now` passes.
    Output tick(Millis now);
    // The server is stopping: the driver is ended, and none is started again.
    Output shutdown(Millis now);

    // The driver's command, for the next start: empty when none is configured, and then no queued prompt is taken
    // and the AI shows down (activity()). With one configured again, what waits for it goes on (the
    // oldest queued prompt is taken), as at every step.
    Output configure(std::vector<std::string> command, std::string cwd, std::string configured, Millis now);
    // The person set the session's model and effort: the next send is written after a settings line that gives them
    // to the driver, so they apply from the next turn; a turn already open keeps the ones it began with, and nothing
    // is written now. A field put back to the harness's default, which a settings line cannot say, ends the driver
    // quietly once no turn is open (never a failed start); the next starts on the defaults.
    Output setOptions(std::string model, std::string effort, Millis now);
    // The person set the session's account. A driver started under another is ended quietly once no turn is open (an
    // open turn runs to its end first), never counted as a failed start; the next starts under this account.
    Output setAccount(std::string account, Millis now, bool automatic = false,
                      const std::vector<std::string>* command = nullptr);
    // A model-limit choice was committed to the store together with its new account or model.
    void acceptModelChoice(const sync::DriverState& durable);
    bool awaitingRotation() const;
    Output rotationExhausted();
    // The session's instance changed (sync::Hub settles it, or a reload moved it): its directory is `cwd`, and `here`
    // says whether it is on this server's machine. A driver started in another directory, or any driver once the
    // instance is elsewhere, is ended quietly once no turn is open, like an account change; the next starts in `cwd`.
    // The harness keeps its sessions by directory and account. When either changes, the next driver starts afresh,
    // its first send carrying a summary of the conversation so far (Config::summarize), with a notice. While not here,
    // no queued prompt is taken, no driver is started, and the AI shows down unless a turn runs on.
    Output setPlace(std::string cwd, bool here, Millis now);
    // The session's limit changed (its AI's "limit", accounts::limitedFor): a driver started under the other one is
    // ended quietly once no turn is open, like a change of place (an open turn runs to its end first); the next starts
    // under this one.
    Output setLimited(bool limited, Millis now);
    // The directories a limited driver keeps out changed (ais.json "limit_keeps_out"): as a change of the limit. A
    // limited driver not keeping out every one of them is weaker than the session's limit: a turn open on it is stopped
    // now and the driver ended; one keeping out more than them is ended once no turn is open.
    Output setKeepsOut(std::vector<std::string> keepsOut, Millis now);
    // The session's AI was taken out of ais.json (`present` false) or put back (true), by a reload
    // (models/Supervisor.tla RemoveAI, AddBack). Taken out, what runs is stopped and the driver ended: an open turn
    // gets the person's stop (stop(): the turn ends at its done, or is cut at the stop's deadline), and the driver is
    // ended once no turn is open; a driver still starting is ended at once. While the AI is out no driver starts and
    // nothing queued is taken: the waiting prompts and the queue are kept, and go on once it is back. The server refuses
    // a send first (Drivers::send). Nothing when it is already so.
    Output setPresent(bool present, Millis now);

    std::optional<Millis> deadline() const;     // when tick() next has something to do
    const sync::DriverState& state() const { return state_; }
    Phase phase() const { return phase_; }
    Stage stage() const { return stage_; }
    bool alive() const { return phase_ != Phase::None; }
    bool gathering() const { return !gathered_.empty(); }   // text of the open turn not yet in the session
    const std::string& gathered() const { return gathered_; }   // that text: the AI's block, still open
    const std::vector<std::string>& queue() const { return queue_; }    // the session's queue, oldest first
    bool finished() const { return finished_; }             // the driver alive finished a turn
    int failedStarts() const { return failed_; }
    bool held() const { return held_; }                     // no driver is started until the next prompt
    bool noCredential() const { return noCredential_; }     // the driver alive said it has no credential
    bool crashed() const { return crashed_; }   // the last driver failed, and none has said hello since
    bool unconfigured() const { return config_.command.empty(); }  // no driver configured
    bool shuttingDown() const { return down_; }
    const std::string& model() const { return config_.model; }
    const std::string& effort() const { return config_.effort; }
    const std::string& account() const { return config_.account; }
    const std::string& driverAccount() const { return driverAccount_; }    // the alive driver's; empty with none
    const std::string& cwd() const { return config_.cwd; }                  // the session's directory
    const std::string& driverCwd() const { return driverCwd_; }    // the alive driver's directory; empty with none
    bool limited() const { return config_.limited; }                // the next driver starts limited
    bool driverLimited() const { return driverLimited_; }           // the alive driver was started limited
    bool here() const { return config_.here; }                           // the session's instance is on this machine
    bool present() const { return config_.present; }                     // the session's AI is in ais.json
    bool retiring() const { return retiring_; }     // the driver is being ended for an account change
    const std::string& openPrompt() const { return openPrompt_; }
    // What the AI is doing, for the window's state cursor (models/Supervisor.tla Activity): Down while starting is
    // held back (failed starts, or no credential); otherwise Busy while a turn is open or about to begin (a prompt
    // waits with the instance here and the AI in ais.json, or a driver starts); otherwise Down when no driver is
    // configured (nothing queued is taken, and a prompt reaches no driver), or the instance is on another machine (no
    // driver starts here), or the AI is not in ais.json (nothing runs for it), or when
    // the last driver failed (crashed, broke the protocol, missed a deadline) and none has
    // said hello since, unless the server is stopping; otherwise Idle, which is only ever with
    // nothing running, nothing waiting and nothing queued that will be taken: the AI is waiting for the person.
    sync::Activity activity() const;

    // Steering, for the window and the walk: what was written into the open turn (oldest first), the driver's last
    // whole queue report, each id the driver reported and the harness has since taken in, whether the open turn has
    // settled, and which kinds the driver alive named.
    struct Written {
        std::uint64_t id = 0;
        bool followUp = false;
        std::string text;
    };
    const std::vector<Written>& written() const { return written_; }
    const driver::Queue& reported() const { return reported_; }
    bool taken(std::uint64_t id) const;
    const std::set<std::uint64_t>& everReported() const { return everReported_; }
    bool settledTurn() const { return settled_; }
    bool driverSteers() const { return driverSteers_; }
    bool driverFollowsUp() const { return driverFollowsUp_; }
    bool driverSettles() const { return driverSettles_; }
    std::uint32_t activeAgents() const { return childAgents_ + (state_.openTurn != 0); }

private:
    void effect(Effect::What what, std::string text, std::uint64_t turn = 0);
    void command(Command::Kind kind, std::vector<std::string> argv = {}, std::string line = {});
    void receive(const driver::DriverMessage& message, Millis now);
    void flush();
    void cutOpen();
    Output into(const std::string& text, bool followUp, Millis now);
    bool steering(const driver::DriverMessage& message, std::uint64_t open, Millis now);
    void giveBack();
    // Whether one more message of text fits the waiting limits, counting the prompts waiting and what was written into
    // the open turn and not seen taken in (any of which may wait again at done): empty when it fits, else why not.
    std::string fits(const std::string& text) const;
    void cutWritten();
    void clearSteering();
    void clearContinuation();
    void abandon(const std::string& why);
    void end(const std::string& why, Millis now);
    void endQuietly(Millis now);
    void close(Millis now);
    void stopOpen(Millis now);
    void take();
    void startDriver(Millis now);
    void writeNext();
    void retire(Millis now);
    bool weaker() const;
    void stopWeaker(Millis now);
    driver::Send sendOf(std::uint64_t turn, const std::string& prompt) const;
    void advance(Millis now);
    Output finish();
    std::string brokeProtocol(const std::string& what) const;

    Config config_;
    sync::DriverState state_;
    std::vector<std::string> queue_;    // the session's queue, as the store holds it once the output is committed
    Phase phase_ = Phase::None;
    Stage stage_ = Stage::None;
    std::optional<Millis> deadline_;
    std::string openPrompt_;            // the open turn's prompt
    std::string gathered_;              // the open turn's text not yet in the session
    std::uint32_t childAgents_ = 0;
    bool finished_ = false;
    int failed_ = 0;
    bool held_ = false;
    bool noCredential_ = false;
    bool crashed_ = false;              // the last driver failed (abandoned), and none has said hello since
    bool down_ = false;
    std::string lastFailure_;           // why the last driver ended, for the notice that holds starts back
    std::string lastCause_;             // the cause the driver gave with its last error, if any
    std::string lastFix_;               // and its fix
    std::string driverEmail_;           // registry email verified by the alive driver
    std::string driverAccount_;         // the account the driver alive was started under
    std::string driverCwd_;             // the directory it was started in
    bool driverLimited_ = false;        // whether it was started limited
    std::vector<std::string> driverKeepsOut_;   // and, limited, the directories it was told to keep out
    std::string driverModel_;           // the model and effort it was told (empty: the harness's default)
    std::string driverEffort_;
    bool sandboxChanged_ = false;
    bool retiring_ = false;             // it is being ended for a change of account or place (or a setting put back)
    bool driverSteers_ = false;         // the alive driver's hello named steer, follow-up, settled
    bool driverFollowsUp_ = false;
    bool driverSettles_ = false;
    std::uint64_t nextWritten_ = 1;     // the next steer or follow-up's id: from 1, never reused in this server
    std::vector<Written> written_;      // written into the open turn, oldest first
    driver::Queue reported_;            // the driver's last whole queue report for the open turn
    std::set<std::uint64_t> everReported_;  // every id it reported for the open turn
    bool settled_ = false;              // the open turn's settled has arrived
    Output out_;                        // the output of the event in progress
};

} // namespace nexus::server
