// Trace walk: steering a turn that runs (docs/driver-protocol.md, "Steering a turn"), the server's real supervisor
// (src/server/supervisor.h) and a driver's real side of it (src/driver/steering.h, which the fake driver plays) over
// two pipes of encoded protocol lines, with the harness behind the driver. A seeded random walk writes
// SteeringTraceData.tla, for TLC to check that it is a behaviour of models/Steering.tla (models/SteeringTrace.tla).
// No processes, no clock: the walk plays the person, the pipes, the harness's moments and the driver's exit.
//
//   steering_walk <seed> <steps> <mode> <out.tla>
//
// mode is ok, or a planted fault for the tests that show the check can fail:
//   silent         the driver takes steers in at a tool-call boundary without writing its queue
//   early-follow   the driver takes a follow-up in at a tool-call boundary
//   write-settled  the server writes a steer or follow-up into a turn after reading its settled (the walk plays that
//                  server's line itself; the real supervisor makes it a prompt)
//
// A faulty mode must show its fault: a walk in which it never fires (no steer pending at a tool-call boundary, say)
// is run again with the same features and another random stream until it fires, and the walk fails if none of 1000
// does. The trace data names the seed and the attempt that was written.
//
// The seed picks the features the driver's hello names: below 1000, all three; from 1000, seed % 5 picks one of the
// five sets the model allows (a few named seeds take the other four). The steps, each a Steering.tla action,
// recorded with every variable of the model after it:
//   prompt, steer, follow   the person writes the next message ("m1", "m2", ...) as a prompt, a steer or a follow-up
//   stop                    the person stops the turn that runs
//   srecv                   the server reads the driver's next line
//   seen                    the server sees the driver's exit (once it has read all it wrote)
//   drecv                   the driver reads the server's next line
//   tools, boundary         a model call ends with tool calls; they finish (a tool-call boundary)
//   runend                  a model run ends with something pending: it goes in and the run goes on
//   retry                   a model run ends and another follows by itself (a retry, a compaction)
//   settle, finish          nothing left: settled (to a driver that names it); then the done. The walk offers the
//                           driver a settle at any model run's end; one it refuses (something pending) is nosettle
//   exit                    the driver exits

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "driver/protocol.h"
#include "driver/steering.h"
#include "server/supervisor.h"

namespace {

using namespace nexus;
using namespace nexus::driver;
using nexus::server::Phase;
using nexus::server::Supervisor;

constexpr std::size_t kMaxIds = 80;
constexpr std::size_t kMaxWaiting = 4;            // the supervisor's waiting limit in the walk, so that it is reached
constexpr std::uint64_t kPlantedIds = 1000000;    // write-settled: the planted server's ids, above the real ones
constexpr std::size_t kMaxStarts = 3;
constexpr int kMaxTools = 2;
constexpr int kMaxRetries = 1;

const std::vector<std::vector<std::string>> kFeatureSets = {
    {}, {"settled"}, {"steer", "settled"}, {"follow-up", "settled"}, {"steer", "follow-up", "settled"}};

const std::vector<std::string>& featuresFor(std::uint64_t seed) {
    if (seed >= 1000) {
        return kFeatureSets[seed % kFeatureSets.size()];
    }
    return kFeatureSets[4];
}

// splitmix64
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

[[noreturn]] void fail(const std::string& why) {
    std::fprintf(stderr, "steering_walk: %s\n", why.c_str());
    std::exit(1);
}

std::string seq(const std::vector<std::uint64_t>& v) {
    std::string s = "<<";
    for (std::size_t i = 0; i < v.size(); ++i) {
        s += (i ? ", " : "") + std::to_string(v[i]);
    }
    return s + ">>";
}

std::string set(const std::set<std::uint64_t>& v) {
    std::string s = "{";
    bool first = true;
    for (const std::uint64_t x : v) {
        s += (first ? "" : ", ") + std::to_string(x);
        first = false;
    }
    return s + "}";
}

std::string boolean(bool b) {
    return b ? "TRUE" : "FALSE";
}

std::string quote(const std::string& s) {
    return "\"" + s + "\"";
}

// The person's message id in a text "m<id>".
std::uint64_t idOf(const std::string& text) {
    if (text.size() < 2 || text[0] != 'm') {
        fail("a prompt that is not one the walk wrote: " + text);
    }
    return std::strtoull(text.c_str() + 1, nullptr, 10);
}

class Walk {
public:
    Walk(std::uint64_t seed, std::uint64_t attempt, std::string mode)
        : rng_{seed * 1000003 + attempt}, mode_(std::move(mode)) {
        features_ = featuresFor(seed);
        server::Config c;
        c.name = "the AI";
        c.command = {"driver"};
        c.cwd = "/work";
        c.configured = "the drivers file";
        c.limited = false;
        c.maxFailedStarts = 1000;
        c.maxWaiting = kMaxWaiting;
        server::Output out;
        sup_.emplace(c, sync::DriverState{}, 0, &out, std::vector<std::string>{});
        apply(out);
        record("init");
    }

    void run(std::size_t steps) {
        for (std::size_t i = 0; i < steps; ++i) {
            std::vector<std::string> ops = enabled();
            if (ops.empty()) {
                break;
            }
            const std::string op = pick(ops);
            record(take(op));
            if (mode_ == "write-settled" && fired_) {
                break;      // the planted line is the divergence; what follows it only breaks the protocol further
            }
        }
    }

    // In a faulty mode, whether the fault changed what the driver did.
    bool fired() const { return fired_; }

    // A weighted choice, so that turns are steered: an exit or a stop ends the turn and what was written into it, so
    // each is drawn a sixth as often as a driver's or server's own step; a steer or follow-up is drawn mostly while a
    // turn runs, and a prompt or a settle (which ends the turn's chance of one) seldom; at the waiting limit the person
    // writes seldom, so that the messages are not all spent on refusals.
    std::string pick(const std::vector<std::string>& ops) {
        const bool working = sup_->phase() == Phase::Working;
        std::size_t held = sup_->state().waiting.size();
        for (const Supervisor::Written& w : sup_->written()) {
            held += sup_->taken(w.id) ? 0 : 1;
        }
        const bool full = held >= kMaxWaiting;
        auto weight = [working, full](const std::string& op) -> std::size_t {
            if (op == "exit" || op == "stop") {
                return 1;
            }
            if (full && (op == "prompt" || op == "steer" || op == "follow")) {
                return 1;       // at the waiting limit: now and then, to see it refused
            }
            if (op == "steer" || op == "follow") {
                return working ? 8 : 1;
            }
            if (op == "prompt" || op == "settle") {
                return 2;
            }
            return 6;
        };
        std::size_t total = 0;
        for (const std::string& op : ops) {
            total += weight(op);
        }
        std::size_t r = rng_.below(total);
        for (const std::string& op : ops) {
            if (r < weight(op)) {
                return op;
            }
            r -= weight(op);
        }
        return ops.back();
    }

    void write(const std::string& path, std::uint64_t seed, std::uint64_t attempt) const {
        std::ofstream out(path);
        out << "---- MODULE SteeringTraceData ----\n"
            << "\\* Written by tests/driver/steering_walk.cpp, seed " << seed << ", attempt " << attempt << ", mode "
            << mode_ << ".\n"
            << "EXTENDS Naturals, Sequences\n"
            << "TraceMaxIds == " << kMaxIds << "\nTraceMaxStarts == " << kMaxStarts << "\nTraceMaxTools == "
            << kMaxTools << "\nTraceMaxRetries == " << kMaxRetries << "\nTraceMaxWaiting == " << kMaxWaiting
            << "\nTrace == <<\n";
        for (std::size_t i = 0; i < trace_.size(); ++i) {
            out << trace_[i] << (i + 1 < trace_.size() ? ",\n" : "\n");
        }
        out << ">>\n====\n";
        if (!out) {
            fail("cannot write " + path);
        }
    }

private:
    struct Driver {
        bool alive = false;
        Steering st;
        std::string run = "idle";
        int tools = 0;
        int retries = 0;
        bool broken = false;
    };

    std::vector<std::string> enabled() const {
        std::vector<std::string> ops;
        const Supervisor& s = *sup_;
        if (kinds_.size() < kMaxIds) {
            ops.insert(ops.end(), {"prompt", "steer", "follow"});
        }
        if (s.phase() == Phase::Working) {
            ops.push_back("stop");
        }
        if (!d2s_.empty()) {
            ops.push_back("srecv");
        }
        if (s.phase() != Phase::None && !d_.alive && d2s_.empty()) {
            ops.push_back("seen");
        }
        if (d_.alive) {
            if (!s2d_.empty()) {
                ops.push_back("drecv");
            }
            if (d_.run == "model" && d_.tools < kMaxTools) {
                ops.push_back("tools");
            }
            if (d_.run == "tools") {
                ops.push_back("boundary");
            }
            if (d_.run == "model" && d_.st.pending()) {
                ops.push_back("runend");
            }
            if (d_.run == "model" && d_.retries < kMaxRetries) {
                ops.push_back("retry");
            }
            if (d_.run == "model") {
                ops.push_back("settle");    // the driver's code refuses it while something is pending (nosettle)
            }
            if (d_.run == "settling") {
                ops.push_back("finish");
            }
            if (starts_ < kMaxStarts) {
                ops.push_back("exit");
            }
        }
        return ops;
    }

    // Takes the step, and returns the op to record (the op, or nosettle for a settle the driver refused).
    std::string take(const std::string& op) {
        Supervisor& s = *sup_;
        if (op == "prompt" || op == "steer" || op == "follow") {
            const std::uint64_t id = kinds_.size() + 1;
            kinds_.push_back(op == "prompt" ? "prompt" : op);
            const std::string text = "m" + std::to_string(id);
            const bool named = std::find(features_.begin(), features_.end(),
                op == "steer" ? "steer" : "follow-up") != features_.end();
            if (mode_ == "write-settled" && op != "prompt" && named && s.phase() == Phase::Working &&
                    s.settledTurn()) {
                // The planted server: written into the settled turn as the supervisor would without its check.
                const std::uint64_t written = kPlantedIds + id;
                persons_[written] = id;
                s2d_.push_back(op == "steer" ? encode(Steer{s.state().openTurn, written, text}).line
                                             : encode(FollowUp{s.state().openTurn, written, text}).line);
                fired_ = true;
                return op;
            }
            pendingPerson_ = id;
            apply(op == "prompt" ? s.prompt(text, 0) : op == "steer" ? s.steer(text, 0) : s.followUp(text, 0));
            pendingPerson_ = 0;
            // Neither waiting, nor written into the turn, nor the turn's prompt: refused at the waiting limit.
            const auto& waiting = s.state().waiting;
            const bool held = std::find(waiting.begin(), waiting.end(), text) != waiting.end() ||
                std::any_of(s.written().begin(), s.written().end(),
                    [&](const Supervisor::Written& w) { return w.text == text; }) ||
                (s.state().openTurn != 0 && s.openPrompt() == text);
            if (!held) {
                refused_.insert(id);
            }
        } else if (op == "stop") {
            apply(s.stop(0));
        } else if (op == "srecv") {
            const std::string line = d2s_.front();
            d2s_.pop_front();
            const Decoded<DriverMessage> m = decodeDriverMessage(std::string_view(line).substr(0, line.size() - 1));
            if (!m.message) {
                fail("the driver side wrote a line that does not decode: " + m.error);
            }
            apply(s.message(*m.message, 0));
        } else if (op == "seen") {
            for (const Supervisor::Written& w : s.written()) {
                if (!s.taken(w.id)) {
                    cut_.insert(person(w.id));
                }
            }
            if (s.state().openTurn != 0) {
                cut_.insert(idOf(s.openPrompt()));
            }
            s2d_.clear();       // what the dead driver never read goes with its pipe
            apply(s.exited("exited with status 1", 0));
        } else if (op == "drecv") {
            drecv();
        } else if (op == "tools") {
            d_.run = "tools";
            ++d_.tools;
        } else if (op == "boundary") {
            Steering::Given g = d_.st.boundary();
            if (mode_ == "early-follow") {
                Steering::Given f = d_.st.runEnd();
                g.items.insert(g.items.end(), f.items.begin(), f.items.end());
                if (f.report) {
                    g.report = f.report;
                }
                fired_ = fired_ || !f.items.empty();
            }
            if (mode_ == "silent" && g.report) {
                fired_ = true;
            }
            give(g, mode_ != "silent");
            d_.run = "model";
        } else if (op == "runend") {
            give(d_.st.runEnd(), true);
        } else if (op == "retry") {
            ++d_.retries;
        } else if (op == "settle") {
            const std::optional<Settled> settled = d_.st.settle();
            if (!d_.st.settling()) {
                if (settled) {
                    fail("the driver refused a settle but wrote settled");
                }
                return "nosettle";
            }
            if (settled) {
                driverWrites(*settled);
            }
            d_.run = "settling";
        } else if (op == "finish") {
            driverWrites(Done{d_.st.turn(), d_.st.aborted() ? Outcome::Stopped : Outcome::Ok, std::nullopt,
                std::nullopt});
            d_.st.done();
            d_.run = "idle";
        } else if (op == "exit") {
            d_.alive = false;
        }
        return op;
    }

    void give(const Steering::Given& g, bool report) {
        for (const Steering::Item& item : g.items) {
            hlog_.push_back(idOf(item.text));
        }
        if (g.report && report) {
            driverWrites(*g.report);
        }
    }

    void drecv() {
        const std::string line = s2d_.front();
        s2d_.pop_front();
        const Decoded<ServerMessage> m = decodeServerMessage(std::string_view(line).substr(0, line.size() - 1));
        if (!m.message) {
            fail("the supervisor wrote a line that does not decode: " + m.error);
        }
        Steering::Took took;
        if (const Send* send = std::get_if<Send>(&*m.message)) {
            took = d_.st.send(*send);
            if (took.what == Steering::Took::What::Taken) {
                hlog_.push_back(idOf(send->prompt));
                d_.run = "model";
                d_.tools = 0;
                d_.retries = 0;
            }
        } else if (const Steer* steer = std::get_if<Steer>(&*m.message)) {
            took = d_.st.take(*steer);
        } else if (const FollowUp* follow = std::get_if<FollowUp>(&*m.message)) {
            took = d_.st.take(*follow);
        } else if (const Stop* stop = std::get_if<Stop>(&*m.message)) {
            took = d_.st.stop(*stop);
            if (took.what == Steering::Took::What::Taken) {
                d_.run = "settling";
            }
        } else {
            fail("the supervisor wrote a line the walk does not expect: " + line);
        }
        if (took.what == Steering::Took::What::Refused) {
            d_.broken = true;
            std::fprintf(stderr, "steering_walk: the driver side refused: %s\n", took.why.c_str());
        }
        if (took.report) {
            driverWrites(*took.report);
        }
        if (took.settled) {
            driverWrites(*took.settled);
        }
    }

    void driverWrites(const DriverMessage& m) {
        const Encoded e = encode(m);
        if (!e.error.empty()) {
            fail("the driver side wrote a message that does not encode: " + e.error);
        }
        d2s_.push_back(e.line);
    }

    // The supervisor's output: its commands carried out at once (a start gives a fresh driver, which says hello).
    void apply(const server::Output& out) {
        for (const server::Command& c : out.commands) {
            if (c.kind == server::Command::Kind::Start) {
                ++starts_;
                d_ = Driver{};
                d_.alive = true;
                d_.st.name(features_);
                s2d_.clear();
                d2s_.clear();
                driverWrites(Hello{kProtocolVersion, "walk", features_});
            } else if (c.kind == server::Command::Kind::Write) {
                s2d_.push_back(c.line);
                const Decoded<ServerMessage> m =
                    decodeServerMessage(std::string_view(c.line).substr(0, c.line.size() - 1));
                if (m.message && (std::holds_alternative<Steer>(*m.message) ||
                                  std::holds_alternative<FollowUp>(*m.message))) {
                    if (pendingPerson_ == 0) {
                        fail("a steer or follow-up written with no message of the person's in hand");
                    }
                    const std::uint64_t written = std::holds_alternative<Steer>(*m.message)
                        ? std::get<Steer>(*m.message).id : std::get<FollowUp>(*m.message).id;
                    persons_[written] = pendingPerson_;
                }
            } else {
                serverBroke_ = true;     // the supervisor ended the driver: a rule broken, in a walk that breaks none
            }
        }
    }

    // The person's id of the supervisor's written id.
    std::uint64_t person(std::uint64_t written) const {
        const auto it = persons_.find(written);
        if (it == persons_.end()) {
            fail("an id the supervisor never wrote: " + std::to_string(written));
        }
        return it->second;
    }

    std::vector<std::uint64_t> persons(const std::vector<std::uint64_t>& written) const {
        std::vector<std::uint64_t> out;
        for (const std::uint64_t w : written) {
            out.push_back(person(w));
        }
        return out;
    }

    std::string message(const std::string& k, std::uint64_t t, std::uint64_t i, const std::vector<std::uint64_t>& s,
                        const std::vector<std::uint64_t>& f, bool a) const {
        return "[k |-> " + quote(k) + ", t |-> " + std::to_string(t) + ", i |-> " + std::to_string(i) + ", s |-> " +
            seq(s) + ", f |-> " + seq(f) + ", a |-> " + boolean(a) + "]";
    }

    std::string serverLine(const std::string& line) const {
        const ServerMessage m = *decodeServerMessage(std::string_view(line).substr(0, line.size() - 1)).message;
        if (const Send* x = std::get_if<Send>(&m)) {
            return message("send", x->turn, idOf(x->prompt), {}, {}, false);
        }
        if (const Steer* x = std::get_if<Steer>(&m)) {
            return message("steer", x->turn, person(x->id), {}, {}, false);
        }
        if (const FollowUp* x = std::get_if<FollowUp>(&m)) {
            return message("follow", x->turn, person(x->id), {}, {}, false);
        }
        const Stop& x = std::get<Stop>(m);
        return message("stop", x.turn, 0, {}, {}, false);
    }

    std::string driverLine(const std::string& line) const {
        const DriverMessage m = *decodeDriverMessage(std::string_view(line).substr(0, line.size() - 1)).message;
        if (std::holds_alternative<Hello>(m)) {
            return message("hello", 0, 0, {}, {}, false);
        }
        if (const Queue* x = std::get_if<Queue>(&m)) {
            return message("queue", x->turn, 0, persons(x->steer), persons(x->followUp), false);
        }
        if (const Settled* x = std::get_if<Settled>(&m)) {
            return message("settled", x->turn, 0, {}, {}, x->aborted);
        }
        const Done& x = std::get<Done>(m);
        return message("done", x.turn, 0, {}, {}, x.outcome == Outcome::Stopped);
    }

    void record(const std::string& op) {
        const Supervisor& s = *sup_;
        const Phase p = s.phase();
        const std::string drv = p == Phase::None ? "none" : p == Phase::Starting ? "starting" :
            p == Phase::Ending ? "ending" : "up";
        std::vector<std::uint64_t> waiting;
        for (const std::string& w : s.state().waiting) {
            waiting.push_back(idOf(w));
        }
        std::vector<std::uint64_t> inflight;
        for (const Supervisor::Written& w : s.written()) {
            inflight.push_back(person(w.id));
        }
        std::set<std::uint64_t> ever;
        for (const std::uint64_t w : s.everReported()) {
            ever.insert(person(w));
        }
        std::ostringstream r;
        r << "[op |-> " << quote(op) << ",\n feats |-> {";
        for (std::size_t i = 0; i < features_.size(); ++i) {
            r << (i ? ", " : "") << quote(features_[i] == "follow-up" ? "follow" : features_[i]);
        }
        r << "},\n kinds |-> <<";
        for (std::size_t i = 0; i < kinds_.size(); ++i) {
            r << (i ? ", " : "") << quote(kinds_[i]);
        }
        r << ">>,\n s |-> [drv |-> " << quote(drv) << ", starts |-> " << starts_ << ", turn |-> " << s.state().openTurn
          << ", next |-> " << s.state().nextTurn << ", prompt |-> "
          << (s.state().openTurn != 0 ? idOf(s.openPrompt()) : 0) << ", stopping |-> "
          << boolean(p == Phase::Stopping) << ", settled |-> " << boolean(s.settledTurn()) << ", waiting |-> "
          << seq(waiting) << ", inflight |-> " << seq(inflight) << ", rep |-> [steer |-> "
          << seq(persons(s.reported().steer)) << ", follow |-> " << seq(persons(s.reported().followUp))
          << "], ever |-> " << set(ever) << ", cut |-> " << set(cut_) << ", refused |-> " << set(refused_)
          << ", broken |-> "
          << boolean(serverBroke_ || p == Phase::Ending) << "],\n d |-> [alive |-> " << boolean(d_.alive)
          << ", turn |-> " << d_.st.turn() << ", last |-> " << d_.st.lastTurn() << ", lastid |-> "
          << (d_.st.lastId() == 0 ? 0 : person(d_.st.lastId())) << ", ps |-> " << seq(persons(d_.st.steerIds()))
          << ", pf |-> " << seq(persons(d_.st.followUpIds())) << ", run |-> " << quote(d_.run) << ", tools |-> "
          << d_.tools << ", retries |-> " << d_.retries << ", aborted |-> " << boolean(d_.st.aborted())
          << ", broken |-> " << boolean(d_.broken) << "],\n s2d |-> <<";
        for (std::size_t i = 0; i < s2d_.size(); ++i) {
            r << (i ? ", " : "") << serverLine(s2d_[i]);
        }
        r << ">>,\n d2s |-> <<";
        for (std::size_t i = 0; i < d2s_.size(); ++i) {
            r << (i ? ", " : "") << driverLine(d2s_[i]);
        }
        r << ">>,\n hlog |-> " << seq(hlog_) << "]";
        trace_.push_back(r.str());
    }

    Rng rng_;
    std::string mode_;
    std::vector<std::string> features_;
    std::optional<Supervisor> sup_;
    Driver d_;
    std::deque<std::string> s2d_;
    std::deque<std::string> d2s_;
    std::vector<std::string> kinds_;
    std::vector<std::uint64_t> hlog_;
    std::set<std::uint64_t> cut_;
    std::set<std::uint64_t> refused_;              // the person's messages refused at the waiting limit
    std::map<std::uint64_t, std::uint64_t> persons_;   // the supervisor's written id -> the person's id
    std::uint64_t pendingPerson_ = 0;
    std::size_t starts_ = 0;
    bool serverBroke_ = false;
    bool fired_ = false;
    std::vector<std::string> trace_;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: steering_walk <seed> <steps> <mode: ok, silent, early-follow or write-settled> "
            "<out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const std::size_t steps = std::strtoull(argv[2], nullptr, 10);
    const std::string mode = argv[3];
    if (mode != "ok" && mode != "silent" && mode != "early-follow" && mode != "write-settled") {
        std::fprintf(stderr, "steering_walk: mode %s is not ok, silent, early-follow or write-settled\n", mode.c_str());
        return 2;
    }
    for (std::uint64_t attempt = 0; attempt < 1000; ++attempt) {
        Walk walk(seed, attempt, mode);
        walk.run(steps);
        if (mode == "ok" || walk.fired()) {
            walk.write(argv[4], seed, attempt);
            return 0;
        }
    }
    std::fprintf(stderr, "steering_walk: mode %s never fired in 1000 walks from seed %llu; the walk cannot show its "
        "fault, so the check would pass for the wrong reason. Make the fault reachable in "
        "tests/driver/steering_walk.cpp\n",
        mode.c_str(), static_cast<unsigned long long>(seed));
    return 1;
}
