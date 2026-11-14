// Trace walk: drives the real driver supervisor (src/server/supervisor.h) with a seeded random walk and writes the
// trace as SupervisorTraceData.tla, for TLC to check against models/Supervisor.tla (models/SupervisorTrace.tla). No
// processes, no clock: the walk plays the driver and the person, keeps the time itself, and commits each step's
// durable state as the server's store does, so that a restart starts from exactly what was committed.
//
//   supervisor_walk <seed> <steps> [--faulty-turn | --faulty-resume | --faulty-queue | --faulty-setting |
//                                   --faulty-account | --faulty-activity | --faulty-dir] <out.tla>
//
// The steps it records, each with the supervisor's whole state after it, the sends it wrote so far, and the entries
// the step appended to the tab:
//   prompt    the person sends the next prompt ("p1", "p2", ...)
//   queue     the person queues the next prompt for later (the hub's limit, the waiting bound here, is kept: a queue
//             past it is refused by the hub before the supervisor sees it, so the walk does not make one)
//   stop      the person stops the friend's work
//   message   the driver writes a message: mostly what the protocol allows where it is (hello, a turn's text, tools
//             and done, a session id, the credential, an error), and at times what it does not (a message before
//             hello, a wrong protocol, a message for a turn other than the open one, an API key's credential)
//   broken    a line the server refuses, or the driver's output closing
//   exit      the driver exits
//   timeout   the clock reaches the supervisor's deadline (hello, a stop's done, a grace period); before each, the
//             walk ticks one millisecond early and requires that nothing happens
//   shutdown  the server stops
//   restart   the server starts again from the durable state committed last, the driver of the old one gone
//   setting, account, place, configure
//             the person changes the model and effort, or the account; an edit moves the session's instance
//             (/work, the model's "", or /work/d, its "d"; on this machine or another); the drivers file loses the
//             friend's driver, or gives it back
//
// The faults, for the tests that show the check can fail:
//   --faulty-turn    a restart finds the next turn number as 1, as if the store had not kept it
//   --faulty-resume  a restart finds no session id, as if the store had not kept it
//   --faulty-queue   a restart finds the queue empty, as if the store had not kept it
//   --faulty-setting a restart finds the model and effort empty, as if the store had not kept them
//   --faulty-account a restart finds the account empty, as if the store had not kept it
//   --faulty-activity the activity it records reads a crashed driver as idle, as a cursor that forgot the crash would
//   --faulty-dir     a restart finds no directory for the harness session, as if the store had not kept it

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "driver/protocol.h"
#include "fake_mosh.h"
#include "server/supervisor.h"

namespace {

using namespace nexus;
using namespace nexus::server;
using test::Rng;

constexpr std::size_t kPrompts = 100;
constexpr std::size_t kMaxWaiting = 2;
constexpr int kMaxFailed = 2;
constexpr std::size_t kMaxRestarts = 3;
constexpr std::size_t kMaxSwitches = 12;

enum class Fault { None, Turn, Resume, Queue, Setting, Account, Activity, Dir };

// The walk's directories: /work (where the session starts, the model's "") and /work/d (its "d").
const char* const kWork = "/work";
const char* const kWorkD = "/work/d";

// A directory as the model names it: "" for /work (and for none), "d" for /work/d.
std::string dirName(const std::string& dir) {
    return dir == kWorkD ? "d" : "";
}

// What Config::summarize gives here: a send carrying it starts with it.
const char* const kSummary = "S|";

// A model and effort as the spec writes a setting: a record of the two, "" for the harness's default.
std::string settingName(const std::string& model, const std::string& effort) {
    return "[m |-> \"" + model + "\", e |-> \"" + effort + "\"]";
}

std::string str(const std::string& s) {
    return "\"" + s + "\"";
}

const char* phaseName(Phase p) {
    switch (p) {
        case Phase::None: return "none";
        case Phase::Starting: return "starting";
        case Phase::Idle: return "idle";
        case Phase::Working: return "working";
        case Phase::Stopping: return "stopping";
        case Phase::Ending: return "ending";
    }
    return "?";
}

const char* stageName(Stage s) {
    switch (s) {
        case Stage::None: return "none";
        case Stage::Closed: return "closed";
        case Stage::Termed: return "termed";
        case Stage::Killed: return "killed";
    }
    return "?";
}

const char* activityName(sync::Activity a) {
    switch (a) {
        case sync::Activity::Idle: return "idle";
        case sync::Activity::Busy: return "busy";
        case sync::Activity::Down: return "down";
    }
    return "?";
}

const char* effectKind(Effect::What w) {
    switch (w) {
        case Effect::What::Prompt: return "P";
        case Effect::What::Reply: return "F";
        case Effect::What::Tool: return "T";
        case Effect::What::Error: return "E";
        case Effect::What::Stopped: return "K";
        case Effect::What::Cut: return "X";
        case Effect::What::Notice: return "N";
    }
    return "?";
}

// "p7" is prompt 7; anything else 0.
std::size_t promptId(const std::string& text) {
    return text.size() > 1 && text[0] == 'p' ? std::strtoull(text.c_str() + 1, nullptr, 10) : 0;
}

[[noreturn]] void fail(const std::string& why) {
    std::fprintf(stderr, "supervisor_walk: %s\n", why.c_str());
    std::exit(1);
}

Config config() {
    Config c;
    c.name = "ada";
    c.command = {"driver"};
    c.cwd = kWork;
    c.summarize = [](const std::string&) { return std::string(kSummary); };
    c.configured = "the walk";
    c.timing = Timing{100, 200, 50, 50};
    c.maxFailedStarts = kMaxFailed;
    c.maxWaiting = kMaxWaiting;
    return c;
}

class Walk {
public:
    explicit Walk(Fault fault) : fault_(fault) {
        Output out;
        sup_.emplace(configured(), committed_, now_, &out);
        carryOut(out);
        records_.push_back(record("init", "", 0, "", out));
    }

    bool step(Rng& rng) {
        Supervisor& s = *sup_;
        const bool alive = procs_ > 0 && s.phase() != Phase::None;
        const bool ending = s.phase() == Phase::Ending;
        const std::size_t r = rng.below(1000);
        if (s.deadline() && (r < 30 || (ending && r < 300))) {
            return timeout();
        }
        if (r < 100 && sent_ < kPrompts && !s.shuttingDown() && !unconfigured_) {
            return prompt();
        }
        if (r < 130 && sent_ < kPrompts && !s.shuttingDown() && s.queue().size() < kMaxWaiting) {
            return queue();
        }
        if (r < 160) {
            return take("stop", "", 0, "", s.stop(now_));
        }
        if (r < 170 && alive && !ending) {
            if (rng.chance(2)) {
                return take("broken", "", 0, "", s.refused("line 3: not JSON", now_));
            }
            return take("broken", "", 0, "", s.closed("output", now_));
        }
        if (alive && (r < 185 || (ending && r < 500))) {
            return exit(rng);
        }
        if (r < 190 && !s.shuttingDown()) {
            return take("shutdown", "", 0, "", s.shutdown(now_));
        }
        if ((r < 194 || (s.shuttingDown() && !alive)) && restarts_ < kMaxRestarts) {
            return restart();
        }
        if (r < 215 && switches_ < kMaxSwitches) {
            const std::size_t which = rng.below(4);
            return which == 0 ? setting(rng) : which == 1 ? account(rng) : which == 2 ? place(rng) : configure();
        }
        if (alive) {
            return message(rng);
        }
        if (sent_ < kPrompts && !s.shuttingDown() && !unconfigured_) {
            return prompt();
        }
        if (unconfigured_ && switches_ < kMaxSwitches) {
            return configure();
        }
        if (s.deadline()) {
            return timeout();
        }
        return restarts_ < kMaxRestarts && restart();
    }

    const std::vector<std::string>& records() const { return records_; }
    std::size_t sent() const { return sent_; }
    std::size_t queued() const { return queued_; }
    std::size_t taken() const { return taken_; }
    std::size_t stops() const { return stops_.size(); }
    std::size_t restarts() const { return restarts_; }
    std::size_t written() const { return written_.size(); }
    std::size_t count(Effect::What w) const { return counts_[static_cast<int>(w)]; }

private:
    // The session's model and effort, and its account, as the store keeps them: a restart starts from them.
    Config configured() const {
        Config c = config();
        c.model = model_;
        c.effort = effort_;
        c.account = account_;
        c.cwd = dir_;
        c.here = here_;
        if (unconfigured_) {
            c.command.clear();
        }
        return c;
    }

    // The drivers file loses the friend's driver, or gives it back (the server's Drivers::showResolved): no prompt
    // is sent to the supervisor while it has none, as the server refuses it for want of a driver.
    bool configure() {
        ++switches_;
        unconfigured_ = !unconfigured_;
        Config c = configured();
        return take("configure", "", 0, unconfigured_ ? "none" : "driver", sup_->configure(c.command, c.cwd,
            c.configured, now_));
    }

    bool place(Rng& rng) {
        ++switches_;
        dir_ = rng.chance(2) ? kWork : kWorkD;
        here_ = rng.below(4) != 0;
        return take("place", "", 0, dirName(dir_), sup_->setPlace(dir_, here_, now_));
    }

    bool setting(Rng& rng) {
        ++switches_;
        const char* models[] = {"", "m", "n"};
        model_ = models[rng.below(3)];
        effort_ = rng.chance(2) ? "high" : "";
        return take("setting", "", 0, settingName(model_, effort_), sup_->setOptions(model_, effort_, now_));
    }

    bool account(Rng& rng) {
        ++switches_;
        const char* accounts[] = {"", "x", "y"};
        account_ = accounts[rng.below(3)];
        return take("account", "", 0, account_, sup_->setAccount(account_, now_));
    }

    bool prompt() {
        ++sent_;
        return take("prompt", "", 0, "", sup_->prompt("p" + std::to_string(sent_), now_));
    }

    bool queue() {
        ++sent_;
        const std::string text = "p" + std::to_string(sent_);
        committedQueue_.push_back(text);
        ++queued_;
        return take("queue", "", 0, "", sup_->queued(text, now_));
    }

    bool timeout() {
        const Millis at = *sup_->deadline();
        if (at > now_ + 1) {
            now_ = at - 1;
            Output early = sup_->tick(now_);
            if (!early.effects.empty() || !early.commands.empty() || early.durable || sup_->deadline() != at) {
                fail("a tick one millisecond before the deadline did something");
            }
        }
        now_ = at;
        return take("timeout", "", 0, "", sup_->tick(now_));
    }

    bool exit(Rng& rng) {
        const char* how[] = {"exited with status 0", "exited with status 1", "was killed by signal 9"};
        Output out = sup_->exited(how[rng.below(3)], now_);
        --procs_;
        toldModel_.clear();
        toldEffort_.clear();
        return take("exit", "", 0, "", std::move(out));
    }

    bool restart() {
        ++restarts_;
        procs_ = 0;     // the old server's driver reads the end of its input and exits
        toldModel_.clear();
        toldEffort_.clear();
        sync::DriverState state = committed_;
        if (fault_ == Fault::Turn) {
            state.nextTurn = 1;
        }
        if (fault_ == Fault::Resume) {
            state.session.clear();
        }
        std::vector<std::string> queue = committedQueue_;
        if (fault_ == Fault::Queue) {
            queue.clear();
        }
        Config c = configured();
        if (fault_ == Fault::Setting) {
            c.model.clear();
            c.effort.clear();
        }
        if (fault_ == Fault::Account) {
            c.account.clear();
        }
        if (fault_ == Fault::Dir) {
            state.sessionDir.clear();
        }
        Output out;
        sup_.emplace(std::move(c), state, now_, &out, std::move(queue));
        return take("restart", "", 0, "", std::move(out));
    }

    // The turns a driver may name here (the model's Turns): about the open one, and the last and next written.
    std::uint64_t someTurn(Rng& rng) const {
        const std::uint64_t open = sup_->state().openTurn;
        const std::uint64_t next = sup_->state().nextTurn;
        std::vector<std::uint64_t> turns;
        for (std::uint64_t t : {open - 1, open, open + 1, next - 1, next}) {
            if (t >= 1 && t <= next) {
                turns.push_back(t);
            }
        }
        return turns[rng.below(turns.size())];
    }

    bool message(Rng& rng) {
        using namespace nexus::driver;
        const Phase phase = sup_->phase();
        const std::uint64_t open = sup_->state().openTurn;
        const std::size_t r = rng.below(100);
        if (phase == Phase::Starting) {
            if (r < 92) {
                return say(Hello{1, "walk"}, rng);
            }
            if (r < 95) {
                return say(Hello{2, "walk"}, rng);
            }
        } else if (phase == Phase::Idle && r < 90) {
            if (r < 45) {
                return say(Session{rng.chance(2) ? "a" : "b"}, rng);
            }
            return say(Credential{Billing::Subscription, "max"}, rng);
        } else if ((phase == Phase::Working || phase == Phase::Stopping) && r < 95) {
            if (r < 8) {
                return say(Session{rng.chance(2) ? "a" : "b"}, rng);
            }
            if (r < 12) {
                return say(Credential{Billing::Subscription, "max"}, rng);
            }
            if (r < 14) {
                return say(Error{"it failed", "try again", open, ErrorCode::Harness}, rng);
            }
            if (r < 15) {
                return say(Error{"not signed in", "sign in", open, ErrorCode::NoCredential}, rng);
            }
            if (r < 50) {
                return say(Text{open, rng.chance(2) ? "x" : "y"}, rng);
            }
            if (r < 65) {
                return say(TextEnd{open}, rng);
            }
            if (r < 75) {
                return say(Tool{open, "Bash", "ls"}, rng);
            }
            const Outcome outcomes[] = {Outcome::Ok, Outcome::Ok, Outcome::Stopped, Outcome::Error};
            return say(Done{open, outcomes[rng.below(4)], 0.01, std::nullopt}, rng);
        }
        // Anything, rarely: what the protocol allows elsewhere and what it does not.
        switch (rng.below(9)) {
            case 0: return say(Session{rng.chance(2) ? "a" : "b"}, rng);
            case 1: return say(Credential{rng.chance(3) ? Billing::ApiKey : Billing::Subscription, "max"}, rng);
            case 2: return say(Credential{rng.chance(3) ? Billing::None : Billing::Subscription, ""}, rng);
            case 3: return say(Error{"it failed", "try again", std::nullopt, rng.chance(4) ? ErrorCode::NoCredential :
                rng.chance(2) ? ErrorCode::SessionNotFound : ErrorCode::Other}, rng);
            case 4: return say(Error{"it failed", "try again", open != 0 && rng.chance(2) ? open : someTurn(rng),
                ErrorCode::Harness}, rng);
            case 5: return say(Hello{1, "walk"}, rng);
            case 6: return say(Text{someTurn(rng), "z"}, rng);
            case 7: return say(TextEnd{someTurn(rng)}, rng);
            default: return say(Done{someTurn(rng), Outcome::Ok, std::nullopt, std::nullopt}, rng);
        }
    }

    bool say(const driver::DriverMessage& m, Rng&) {
        using namespace nexus::driver;
        std::string kind;
        std::uint64_t t = 0;
        std::string v;
        if (const Hello* h = std::get_if<Hello>(&m)) {
            kind = "hello";
            v = std::to_string(h->protocol);
        } else if (const Credential* c = std::get_if<Credential>(&m)) {
            kind = "credential";
            v = std::string(billingName(c->billing));
        } else if (const Session* s = std::get_if<Session>(&m)) {
            kind = "session";
            v = s->id;
        } else if (const Text* x = std::get_if<Text>(&m)) {
            kind = "text";
            t = x->turn;
        } else if (const TextEnd* e = std::get_if<TextEnd>(&m)) {
            kind = "text-end";
            t = e->turn;
        } else if (const Tool* tool = std::get_if<Tool>(&m)) {
            kind = "tool";
            t = tool->turn;
        } else if (const Done* d = std::get_if<Done>(&m)) {
            kind = "done";
            t = d->turn;
            v = std::string(outcomeName(d->outcome));
        } else if (const Error* e2 = std::get_if<Error>(&m)) {
            kind = "error";
            t = e2->turn.value_or(0);
            v = e2->code == ErrorCode::NoCredential ? "no-credential" : "";
        }
        return take("message", kind, t, v, sup_->message(m, now_));
    }

    bool take(const char* op, const std::string& kind, std::uint64_t t, const std::string& v, Output out) {
        carryOut(out);
        records_.push_back(record(op, kind, t, v, out));
        return true;
    }

    // As the server does: the durable state committed with the effects, then the commands.
    void carryOut(const Output& out) {
        if (out.durable) {
            committed_ = sup_->state();
        }
        // The store's queue: the hub put a queued prompt at its end before the supervisor saw it; the output's
        // dequeues take from its head. The supervisor's list must be the same.
        if (out.taken > committedQueue_.size()) {
            fail("the supervisor took more queued prompts than the queue holds");
        }
        committedQueue_.erase(committedQueue_.begin(), committedQueue_.begin() + static_cast<std::ptrdiff_t>(out.taken));
        taken_ += out.taken;
        if (committedQueue_ != sup_->queue() && fault_ != Fault::Queue) {
            fail("the supervisor's queue is not the store's");
        }
        for (const Effect& e : out.effects) {
            ++counts_[static_cast<int>(e.what)];
        }
        for (const Command& c : out.commands) {
            if (c.kind == Command::Kind::Start) {
                ++procs_;
                resumed_.clear();
                toldModel_.clear();     // a driver starts on the harness's defaults
                toldEffort_.clear();
                for (std::size_t i = 0; i + 1 < c.argv.size(); ++i) {
                    if (c.argv[i] == "--resume") {
                        resumed_ = c.argv[i + 1];
                    }
                }
            } else if (c.kind == Command::Kind::Write) {
                const auto d = driver::decodeServerMessage(std::string_view(c.line).substr(0, c.line.size() - 1));
                if (!d.message) {
                    fail("the supervisor wrote a line that does not decode: " + d.error);
                }
                if (const driver::Send* send = std::get_if<driver::Send>(&*d.message)) {
                    const bool sum = send->prompt.rfind(kSummary, 0) == 0;
                    const std::string prompt = sum ? send->prompt.substr(std::string(kSummary).size()) : send->prompt;
                    written_.push_back({promptId(prompt), send->turn, settingName(toldModel_, toldEffort_), sum});
                } else if (const driver::Settings* set = std::get_if<driver::Settings>(&*d.message)) {
                    // As the driver applies it: a field left out (empty) stays as it is.
                    if (!set->model.empty()) {
                        toldModel_ = set->model;
                    }
                    if (!set->effort.empty()) {
                        toldEffort_ = set->effort;
                    }
                } else if (const driver::Stop* stop = std::get_if<driver::Stop>(&*d.message)) {
                    stops_.push_back(stop->turn);
                }
            }
        }
    }

    std::string record(const char* op, const std::string& kind, std::uint64_t t, const std::string& v,
                       const Output& out) const {
        const Supervisor& s = *sup_;
        std::ostringstream o;
        // A setting's value is a record; every other value a string.
        const std::string value = std::string(op) == "setting" ? v : str(v);
        o << "[op |-> " << str(op) << ", kind |-> " << str(kind) << ", t |-> " << t << ", v |-> " << value
          << ", sent |-> " << sent_ << ", restarts |-> " << restarts_ << ", phase |-> " << str(phaseName(s.phase()))
          << ", stage |-> " << str(stageName(s.stage())) << ", open |-> " << s.state().openTurn << ", prompt |-> "
          << promptId(s.openPrompt()) << ", next |-> " << s.state().nextTurn << ", waiting |-> <<";
        for (std::size_t i = 0; i < s.state().waiting.size(); ++i) {
            o << (i ? ", " : "") << promptId(s.state().waiting[i]);
        }
        o << ">>, queue |-> <<";
        for (std::size_t i = 0; i < s.queue().size(); ++i) {
            o << (i ? ", " : "") << promptId(s.queue()[i]);
        }
        o << ">>, session |-> " << str(s.state().session) << ", gathered |-> " << (s.gathering() ? "TRUE" : "FALSE")
          << ", finished |-> " << (s.finished() ? "TRUE" : "FALSE") << ", failed |-> " << s.failedStarts()
          << ", blocked |-> " << (s.held() ? "TRUE" : "FALSE") << ", nocred |-> "
          << (s.noCredential() ? "TRUE" : "FALSE") << ", down |-> "
          << (s.shuttingDown() ? "TRUE" : "FALSE") << ", procs |-> " << procs_ << ", written |-> <<";
        for (std::size_t i = 0; i < written_.size(); ++i) {
            o << (i ? ", " : "") << "[p |-> " << written_[i].p << ", t |-> " << written_[i].t << ", v |-> "
              << written_[i].v << ", sum |-> " << (written_[i].sum ? "TRUE" : "FALSE") << "]";
        }
        o << ">>, stops |-> <<";
        for (std::size_t i = 0; i < stops_.size(); ++i) {
            o << (i ? ", " : "") << stops_[i];
        }
        o << ">>, fx |-> <<";
        for (std::size_t i = 0; i < out.effects.size(); ++i) {
            const Effect& e = out.effects[i];
            const bool prompt = e.what == Effect::What::Prompt;
            o << (i ? ", " : "") << "[k |-> " << str(effectKind(e.what)) << ", t |-> " << e.turn << ", p |-> "
              << (prompt ? promptId(e.text) : 0) << "]";
        }
        o << ">>, resumed |-> " << str(resumed_) << ", setting |-> " << settingName(s.model(), s.effort())
          << ", told |-> " << settingName(toldModel_, toldEffort_)
          << ", account |-> " << str(s.account()) << ", drv |-> " << str(s.driverAccount()) << ", retiring |-> "
          << (s.retiring() ? "TRUE" : "FALSE") << ", crashed |-> " << (s.crashed() ? "TRUE" : "FALSE")
          << ", unconf |-> " << (s.unconfigured() ? "TRUE" : "FALSE")
          << ", activity |-> " << str(activityName(activity())) << ", dir |-> " << str(dirName(s.cwd()))
          << ", here |-> " << (s.here() ? "TRUE" : "FALSE") << ", ddir |-> " << str(dirName(s.driverCwd())) << ", sdir |-> "
          << str(dirName(s.state().sessionDir)) << ", summary |-> " << (s.state().summary ? "TRUE" : "FALSE")
          << ", switches |-> " << switches_ << "]";
        return o.str();
    }

    // The code's activity; with the activity fault, a crash reads as idle, as a cursor that forgot it would show.
    sync::Activity activity() const {
        const sync::Activity a = sup_->activity();
        if (fault_ == Fault::Activity && a == sync::Activity::Down && !sup_->held()) {
            return sync::Activity::Idle;
        }
        return a;
    }

    Fault fault_;
    Millis now_ = 1000;
    sync::DriverState committed_;
    std::optional<Supervisor> sup_;
    std::size_t procs_ = 0;
    std::size_t sent_ = 0;
    std::size_t restarts_ = 0;
    std::vector<std::string> committedQueue_;   // the tab's queue as the store holds it
    struct Written {
        std::size_t p;
        std::uint64_t t;
        std::string v;
        bool sum;
    };
    std::vector<Written> written_;
    std::size_t switches_ = 0;
    bool unconfigured_ = false;         // the friend has no driver configured
    std::string model_;
    std::string effort_;
    std::string account_;
    std::string dir_ = kWork;   // the session's place, as the accounts hold it: a restart starts from it
    bool here_ = true;
    std::vector<std::uint64_t> stops_;          // the turn of every stop written
    std::size_t queued_ = 0;
    std::size_t taken_ = 0;
    std::string resumed_;
    std::string toldModel_;     // what the driver alive was told, from the settings lines written to it
    std::string toldEffort_;
    std::size_t counts_[7] = {};
    std::vector<std::string> records_;
};

bool parse(const char* s, std::uint64_t& v) {
    char* end = nullptr;
    v = std::strtoull(s, &end, 10);
    return end != s && *end == '\0';
}

int walk(std::uint64_t seed, std::uint64_t steps, Fault fault, const char* path) {
    Rng rng{seed};
    Walk w(fault);
    for (std::uint64_t i = 0; i < steps && w.step(rng); ++i) {
    }
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "supervisor_walk: cannot write %s\n", path);
        return 1;
    }
    const char* faults[] = {"", ", faulty turn", ", faulty resume", ", faulty queue", ", faulty setting",
                            ", faulty account", ", faulty activity", ", faulty dir"};
    out << "---- MODULE SupervisorTraceData ----\n";
    out << "\\* Written by tests/server/supervisor_walk: seed " << seed << ", " << steps << " steps"
        << faults[static_cast<int>(fault)] << ".\n";
    out << "TracePrompts == " << w.sent() << "\n";
    out << "TraceMaxWaiting == " << kMaxWaiting << "\n";
    out << "TraceMaxFailedStarts == " << kMaxFailed << "\n";
    out << "TraceMaxRestarts == " << kMaxRestarts << "\n";
    out << "TraceSessions == {\"a\", \"b\"}\n";
    out << "TraceModels == {\"m\", \"n\"}\n";
    out << "TraceEfforts == {\"high\"}\n";
    out << "TraceAccounts == {\"x\", \"y\"}\n";
    out << "TraceDirs == {\"d\"}\n";
    out << "TraceMaxSwitches == " << kMaxSwitches << "\n";
    out << "Trace == <<\n";
    const auto& records = w.records();
    for (std::size_t k = 0; k < records.size(); ++k) {
        out << "  " << records[k] << (k + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "supervisor_walk: writing %s failed\n", path);
        return 1;
    }
    std::printf("supervisor_walk: seed %llu, %llu steps%s: %zu records, %zu prompts (%zu queued, %zu taken), "
                "%zu written, %zu stops, %zu replies, "
                "%zu tools, %zu stopped, %zu cut, %zu notices, %zu restarts -> %s\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(steps),
                faults[static_cast<int>(fault)], records.size(), w.sent(), w.queued(), w.taken(), w.written(), w.stops(),
                w.count(Effect::What::Reply),
                w.count(Effect::What::Tool), w.count(Effect::What::Stopped), w.count(Effect::What::Cut),
                w.count(Effect::What::Notice), w.restarts(), path);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> rest;
    Fault fault = Fault::None;
    int faults = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--faulty-turn") {
            fault = Fault::Turn;
            ++faults;
        } else if (a == "--faulty-resume") {
            fault = Fault::Resume;
            ++faults;
        } else if (a == "--faulty-setting") {
            fault = Fault::Setting;
            ++faults;
        } else if (a == "--faulty-account") {
            fault = Fault::Account;
            ++faults;
        } else if (a == "--faulty-activity") {
            fault = Fault::Activity;
            ++faults;
        } else if (a == "--faulty-dir") {
            fault = Fault::Dir;
            ++faults;
        } else if (a == "--faulty-queue") {
            fault = Fault::Queue;
            ++faults;
        } else {
            rest.push_back(a);
        }
    }
    std::uint64_t seed = 0, steps = 0;
    if (rest.size() != 3 || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) || faults > 1) {
        std::fprintf(stderr, "usage: supervisor_walk <seed> <steps> [--faulty-turn | --faulty-resume | "
                     "--faulty-queue | --faulty-setting | --faulty-account | --faulty-activity | --faulty-dir] "
                     "<out.tla>\n");
        return 2;
    }
    return walk(seed, steps, fault, rest[2].c_str());
}
