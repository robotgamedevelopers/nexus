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
// cut, with a notice that whether the friend got the prompt is unknown; consecutive failed starts stop the starting of
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
// The open turn's text not yet in the session (gathered()) is the friend's text block as it grows: the caller shows it
// as an entry still open, kept in memory only. It goes into the session, durable, when the block ends (its text-end, a
// tool, the turn's done or cut) or reaches the entry limit; a server that crashes loses at most that one block, and the
// turn is cut with its notice when the server starts again.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// Which friend, how to start its driver, and the bounds.
struct Config {
    std::string name;                   // the friend's name, as the notices say it
    std::vector<std::string> command;   // the driver's program and its arguments, before --cwd and --resume; empty
                                        // while the friend has no driver configured, and then nothing is taken
    std::string cwd;                    // the directory the harness works in
    std::string configured;             // where the driver is configured, for the fixes notices give
    std::string model;                  // the session's model and effort, written with each send (empty: the
    std::string effort;                 // harness's default)
    std::string account;                // the session's account: a driver runs under the one it was started with
    Timing timing;
    int maxFailedStarts = kMaxFailedStarts;
    std::size_t maxWaiting = 8;             // prompts waiting, at most (the hub's queue limits)
    std::size_t maxWaitingBytes = 32 * 1024;
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

// An entry to append to the session: the person's prompt; the friend's text; a line for a tool the friend used; an
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
    // The person stopped the friend's work: a stop for the open turn, if one is open and no stop is out; while the
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

    // The driver's command, for the next start: empty when none is configured, and then no queued prompt is taken.
    void configure(std::vector<std::string> command, std::string cwd, std::string configured);
    // The person set the session's model and effort: the next send is written after a settings line that gives them
    // to the driver, so they apply from the next turn; a turn already open keeps the ones it began with, and nothing
    // is written now. A field put back to the harness's default, which a settings line cannot say, ends the driver
    // quietly once no turn is open (never a failed start); the next starts on the defaults.
    Output setOptions(std::string model, std::string effort, Millis now);
    // The person set the session's account. A driver started under another is ended quietly once no turn is open (an
    // open turn runs to its end first), never counted as a failed start; the next starts under this account.
    Output setAccount(std::string account, Millis now);

    std::optional<Millis> deadline() const;     // when tick() next has something to do
    const sync::DriverState& state() const { return state_; }
    Phase phase() const { return phase_; }
    Stage stage() const { return stage_; }
    bool alive() const { return phase_ != Phase::None; }
    bool gathering() const { return !gathered_.empty(); }   // text of the open turn not yet in the session
    const std::string& gathered() const { return gathered_; }   // that text: the friend's block, still open
    const std::vector<std::string>& queue() const { return queue_; }    // the session's queue, oldest first
    bool finished() const { return finished_; }             // the driver alive finished a turn
    int failedStarts() const { return failed_; }
    bool held() const { return held_; }                     // no driver is started until the next prompt
    bool noCredential() const { return noCredential_; }     // the driver alive said it has no credential
    bool shuttingDown() const { return down_; }
    const std::string& model() const { return config_.model; }
    const std::string& effort() const { return config_.effort; }
    const std::string& account() const { return config_.account; }
    const std::string& driverAccount() const { return driverAccount_; }    // the alive driver's; empty with none
    bool retiring() const { return retiring_; }     // the driver is being ended for an account change
    const std::string& openPrompt() const { return openPrompt_; }

private:
    void effect(Effect::What what, std::string text, std::uint64_t turn = 0);
    void command(Command::Kind kind, std::vector<std::string> argv = {}, std::string line = {});
    void receive(const driver::DriverMessage& message, Millis now);
    void flush();
    void cutOpen();
    void abandon(const std::string& why);
    void end(const std::string& why, Millis now);
    void endQuietly(Millis now);
    void close(Millis now);
    void take();
    void startDriver(Millis now);
    void writeNext();
    void retire(Millis now);
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
    bool finished_ = false;
    int failed_ = 0;
    bool held_ = false;
    bool noCredential_ = false;
    bool down_ = false;
    std::string lastFailure_;           // why the last driver ended, for the notice that holds starts back
    std::string lastCause_;             // the cause the driver gave with its last error, if any
    std::string lastFix_;               // and its fix
    std::string driverAccount_;         // the account the driver alive was started under
    std::string driverModel_;           // the model and effort it was told (empty: the harness's default)
    std::string driverEffort_;
    bool retiring_ = false;             // it is being ended for a change of account (or a setting put back)
    Output out_;                        // the output of the event in progress
};

} // namespace nexus::server
