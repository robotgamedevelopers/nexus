// Trace walk: drives nexus-server's pass order and its real collaborators with a seeded random walk and writes the
// trace as LoopPassTraceData.tla, for TLC to check against models/LoopPass.tla (models/LoopPassTrace.tla).
//
//   pass_walk <seed> <steps> [--fault <name> | --faulty-<name>] <out.tla>
//
// Fault names (each must reach the broken state in the real code and be rejected by TLC):
//   release-before-flush  Drivers::release runs before DurableStore::flush
//   state-before-flush    link state staged before DurableStore::flush
//   tick-before-flush     links tick after Apply and before DurableStore::flush
//   no-hold               batch applied without Drivers::hold
//   release-on-fail       Drivers::release and state run after DurableStore::flush failed
//
// Collaborators driven directly (no sockets, OS processes, real filesystem or wall clock):
//   - PassOrder and take_pass_step (src/server/pass.h), the dispatch Server::run runs each pass through
//   - DurableStore (src/server/durable.h) over Store, Journal and MemFs (src/store/), with fail_at and crash(plan)
//   - sync::Hub (src/sync/hub.h) and Drivers / Supervisor (src/server/drivers.h) over an in-memory Drivers::Host
//     whose driver stdin bytes are decoded by driver::ServerReader (src/driver/protocol.h)
//   - two windows' FakeSender/FakeReceiver links (tests/sync/fake_mosh.h) and sync::Applier (src/sync/actions.h)

#include <errno.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accounts/registry.h"
#include "driver/protocol.h"
#include "fake_mosh.h"
#include "server/drivers.h"
#include "server/durable.h"
#include "server/pass.h"
#include "store/fs.h"
#include "store/store.h"
#include "sync/actions.h"
#include "sync/hub.h"
#include "sync/sessions.h"

namespace {

using namespace nexus;
using namespace nexus::server;
using test::FakeReceiver;
using test::FakeSender;
using test::Packet;
using test::Rng;

constexpr std::size_t kClients = 2;
constexpr sync::SessionId kSession = 1;
constexpr Millis kFarDeadlineMs = 1000000000ULL;

std::string clientName(std::size_t c) {
    return "w" + std::to_string(c + 1);
}

std::string makePrompt(std::size_t c, std::uint64_t n) {
    return "p:" + std::to_string(c + 1) + ":" + std::to_string(n);
}

std::pair<std::size_t, std::uint64_t> parsePrompt(const std::string& s) {
    if (s.rfind("p:", 0) != 0) {
        std::fprintf(stderr, "pass_walk: unexpected prompt text '%s'\n", s.c_str());
        std::exit(1);
    }
    const std::size_t second = s.find(':', 2);
    if (second == std::string::npos) {
        std::fprintf(stderr, "pass_walk: malformed prompt text '%s'\n", s.c_str());
        std::exit(1);
    }
    const std::size_t c = static_cast<std::size_t>(std::strtoull(s.substr(2, second - 2).c_str(), nullptr, 10));
    const std::uint64_t n = std::strtoull(s.substr(second + 1).c_str(), nullptr, 10);
    return {c, n};
}

std::string tlaPrompt(const std::pair<std::size_t, std::uint64_t>& p) {
    return "<<" + std::to_string(p.first) + ", " + std::to_string(p.second) + ">>";
}

std::string tlaBool(bool b) {
    return b ? "TRUE" : "FALSE";
}

const char* actionKindName(sync::ActionKind k) {
    switch (k) {
    case sync::ActionKind::Send: return "Send";
    case sync::ActionKind::Queue: return "Queue";
    case sync::ActionKind::Stop: return "Stop";
    default: return "Send";
    }
}

std::string tlaEntry(const sync::SessionEntry& e) {
    if (e.kind == EntryKind::Person) {
        return "[k |-> \"P\", t |-> 0, p |-> " + tlaPrompt(parsePrompt(e.text)) + "]";
    }
    if (e.kind == EntryKind::AI) {
        if (e.text.rfind("reply:", 0) != 0) {
            std::fprintf(stderr, "pass_walk: unexpected AI entry '%s'\n", e.text.c_str());
            std::exit(1);
        }
        const std::uint64_t t = std::strtoull(e.text.substr(6).c_str(), nullptr, 10);
        return "[k |-> \"F\", t |-> " + std::to_string(t) + ", p |-> <<0, 0>>]";
    }
    if (e.text == "Stopped.") {
        return "[k |-> \"K\", t |-> 0, p |-> <<0, 0>>]";
    }
    if (e.text.rfind("The reply was cut short", 0) == 0) {
        return "[k |-> \"X\", t |-> 0, p |-> <<0, 0>>]";
    }
    std::fprintf(stderr, "pass_walk: unexpected notice entry '%s'\n", e.text.c_str());
    std::exit(1);
}

std::string tlaSeq(const std::vector<std::string>& items) {
    std::string out = "<<";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += items[i];
    }
    out += ">>";
    return out;
}

std::string tlaClientMap(const std::vector<std::string>& vals) {
    std::string out = "(";
    for (std::size_t c = 0; c < vals.size(); ++c) {
        if (c > 0) {
            out += " @@ ";
        }
        out += std::to_string(c + 1) + " :> " + vals[c];
    }
    out += ")";
    return out;
}

// In-memory Drivers::Host: captures every Send and Stop actually written to driver pipes via driver::ServerReader.
class FakeDriverHost : public Drivers::Host {
public:
    struct Sent {
        std::uint64_t turn = 0;
        std::pair<std::size_t, std::uint64_t> prompt{0, 0};
    };

    struct Proc {
        pid_t pid = -1;
        int inFd = -1;
        int outFd = -1;
        bool inOpen = true;
        bool outOpen = true;
        bool alive = true;
        bool reaped = false;
        driver::ServerReader reader;
        std::string outBuf;
        std::optional<std::uint64_t> activeTurn;
    };

    pid_t spawn(const std::vector<std::string>& argv, int* in, int* out, std::string* why) override {
        if (argv.empty()) {
            *why = "no program";
            return -1;
        }
        const pid_t pid = nextPid_++;
        const int inFd = nextFd_++;
        const int outFd = nextFd_++;
        Proc p;
        p.pid = pid;
        p.inFd = inFd;
        p.outFd = outFd;
        procs_[pid] = std::move(p);
        fdToPid_[inFd] = {pid, true};
        fdToPid_[outFd] = {pid, false};
        *in = inFd;
        *out = outFd;
        return pid;
    }

    ssize_t write(int fd, const char* data, std::size_t size, int* err) override {
        auto it = fdToPid_.find(fd);
        if (it == fdToPid_.end()) {
            *err = EBADF;
            return -1;
        }
        Proc& p = procs_.at(it->second.first);
        if (!p.inOpen || !p.alive) {
            *err = EPIPE;
            return -1;
        }
        std::vector<driver::ServerReader::Item> items;
        p.reader.feed(std::string_view(data, size), items);
        for (const driver::ServerReader::Item& item : items) {
            if (!item.message) {
                std::fprintf(stderr, "pass_walk: server wrote invalid driver line: %s\n", item.error.c_str());
                std::exit(1);
            }
            if (const auto* s = std::get_if<driver::Send>(&*item.message)) {
                sends_.push_back(Sent{s->turn, parsePrompt(s->prompt)});
                p.activeTurn = s->turn;
            } else if (const auto* st = std::get_if<driver::Stop>(&*item.message)) {
                stops_.push_back(st->turn);
            }
        }
        *err = 0;
        return static_cast<ssize_t>(size);
    }

    ssize_t read(int fd, char* data, std::size_t size, int* err) override {
        auto it = fdToPid_.find(fd);
        if (it == fdToPid_.end()) {
            *err = EBADF;
            return -1;
        }
        Proc& p = procs_.at(it->second.first);
        if (!p.outBuf.empty()) {
            const std::size_t n = std::min(size, p.outBuf.size());
            std::memcpy(data, p.outBuf.data(), n);
            p.outBuf.erase(0, n);
            *err = 0;
            return static_cast<ssize_t>(n);
        }
        if (p.alive) {
            *err = EAGAIN;
            return -1;
        }
        *err = 0;
        return 0;
    }

    void close(int fd) override {
        auto it = fdToPid_.find(fd);
        if (it == fdToPid_.end()) {
            return;
        }
        Proc& p = procs_.at(it->second.first);
        if (it->second.second) {
            p.inOpen = false;
            p.alive = false;
        } else {
            p.outOpen = false;
        }
    }

    void killGroup(pid_t pid, int) override {
        auto it = procs_.find(pid);
        if (it != procs_.end()) {
            it->second.alive = false;
        }
    }

    bool exited(pid_t pid) override {
        auto it = procs_.find(pid);
        return it != procs_.end() && !it->second.alive && !it->second.reaped;
    }

    std::string reap(pid_t pid) override {
        auto it = procs_.find(pid);
        if (it != procs_.end()) {
            it->second.reaped = true;
        }
        return "exited with status 0";
    }

    Proc* find(pid_t pid) {
        auto it = procs_.find(pid);
        return it == procs_.end() ? nullptr : &it->second;
    }

    void crashServer() {
        procs_.clear();
        fdToPid_.clear();
    }

    const std::vector<Sent>& sends() const { return sends_; }
    const std::vector<std::uint64_t>& stops() const { return stops_; }

private:
    pid_t nextPid_ = 100;
    int nextFd_ = 10;
    std::map<pid_t, Proc> procs_;
    std::map<int, std::pair<pid_t, bool>> fdToPid_;
    std::vector<Sent> sends_;
    std::vector<std::uint64_t> stops_;
};

struct Window {
    FakeSender<sync::Actions> up;
    FakeReceiver<sync::Sessions> down;
    sync::Sessions shown;
    std::vector<sync::Action> made;
    std::vector<Packet> upFlight;
};

class LoopWalk : private PassSteps {
public:
    LoopWalk(std::uint64_t seed, PassOrder::Fault fault) : rng_{seed ? seed : 1}, fault_(fault), pass_(fault) {
        storeOpts_.compact_bytes = 320;
        limits_.queue = 10000;
        limits_.queueBytes = 1u << 20;
        limits_.tail.entries = 10000;
        limits_.tail.bytes = 1u << 20;

        openServer();
        sync::Action ensure;
        ensure.kind = sync::ActionKind::Ensure;
        ensure.aiName = "ada";
        if (!hub_->apply(ensure) || hub_->failed()) {
            std::fprintf(stderr, "pass_walk: failed to create initial session\n");
            std::exit(1);
        }
        windows_.resize(kClients);
        upReceiver_.resize(kClients);
        applier_.resize(kClients);
        downSender_.resize(kClients);
        for (std::size_t c = 0; c < kClients; ++c) {
            downSender_[c].current = hub_->state();
            const Packet p = downSender_[c].send(rng_, true);
            windows_[c].down.receive(p);
            windows_[c].shown = windows_[c].down.latest();
        }
        record("[op |-> \"init\"");
    }

    void run(std::size_t steps) {
        if (fault_ != PassOrder::Fault::None) {
            runFaulty(steps);
            return;
        }
        for (std::size_t s = 0; s < steps; ++s) {
            runOneStep(false);
        }
        drain();
    }

    std::size_t reached() const { return reached_; }

    void writeModule(const std::string& path) const {
        std::size_t maxActs = 1;
        for (const Window& w : windows_) {
            maxActs = std::max(maxActs, w.made.size());
        }
        std::ofstream out(path);
        if (!out) {
            std::fprintf(stderr, "pass_walk: cannot write %s\n", path.c_str());
            std::exit(1);
        }
        out << "------------------------- MODULE LoopPassTraceData -------------------------\n";
        out << "EXTENDS Integers, Sequences, TLC\n\n";
        out << "TraceClients == 1.." << kClients << "\n";
        out << "TraceMaxActs == " << maxActs << "\n";
        out << "TraceMaxCrashes == " << std::max<std::size_t>(1, crashes_) << "\n";
        out << "TraceMaxFails == " << std::max<std::size_t>(1, fails_) << "\n\n";
        out << "Trace == <<\n";
        for (std::size_t i = 0; i < records_.size(); ++i) {
            out << "  " << records_[i] << (i + 1 < records_.size() ? ",\n" : "\n");
        }
        out << ">>\n";
        out << "=============================================================================\n";
        if (!out.flush()) {
            std::fprintf(stderr, "pass_walk: flush failed on %s\n", path.c_str());
            std::exit(1);
        }
    }

private:
    Timing timing() const {
        return Timing{kFarDeadlineMs, kFarDeadlineMs, kFarDeadlineMs, kFarDeadlineMs};
    }

    void openServer() {
        std::string why;
        if (!store_.open(fs_, "/store", &why, storeOpts_)) {
            std::fprintf(stderr, "pass_walk: store open failed: %s\n", why.c_str());
            std::exit(1);
        }
        Drivers::Options opts;
        opts.table = "/drivers";
        opts.home = "/home";
        opts.timing = timing();
        opts.clock = [this] { return now_; };
        opts.machine = "box";
        opts.host = &host_;
        opts.readTable = [](std::string* text, std::string*) {
            *text = "driver claude /fake/driver\nai ada claude /work\n";
            return true;
        };
        opts.quiet = true;
        drivers_ = std::make_unique<Drivers>(store_, std::move(opts));
        hub_ = std::make_unique<sync::Hub>(store_, limits_, drivers_.get());
        drivers_->attach(*hub_);
        accounts::Registry reg;
        reg.ais.push_back(accounts::AI{"ada", {}, std::string()});
        hub_->useAccounts(reg, nullptr);
        if (!drivers_->start(now_)) {
            std::fprintf(stderr, "pass_walk: drivers start failed\n");
            std::exit(1);
        }
    }

    void makeClientAction(std::size_t c, std::optional<sync::ActionKind> forced = std::nullopt) {
        Window& w = windows_[c];
        sync::Action a;
        a.session = kSession;
        if (forced) {
            a.kind = *forced;
        } else {
            const std::size_t pick = rng_.below(10);
            a.kind = pick < 5 ? sync::ActionKind::Send : pick < 8 ? sync::ActionKind::Queue : sync::ActionKind::Stop;
        }
        if (a.kind != sync::ActionKind::Stop) {
            a.text = makePrompt(c, w.made.size() + 1);
        }
        a.num = w.up.current.add(a);
        w.made.push_back(a);
        w.upFlight.push_back(w.up.send(rng_));
        if (rng_.chance(3)) {
            w.upFlight.push_back(w.up.send(rng_, true));
        }
        record("[op |-> \"act\", c |-> " + std::to_string(c + 1) + ", kind |-> \"" + actionKindName(a.kind) + "\"");
    }

    // PassSteps overrides, called by take_pass_step(pass_, *this, &why).
    void tick() override {
        for (std::size_t c = 0; c < kClients; ++c) {
            windows_[c].up.acknowledge(upReceiver_[c].latestNum());
            const Packet p = downSender_[c].send(rng_, true);
            windows_[c].down.receive(p);
            if (!(windows_[c].down.latest() == sync::Sessions())) {
                windows_[c].shown = windows_[c].down.latest();
            }
        }
    }

    PassOrder::Outcome wait(std::string*) override {
        if (draining_) {
            return PassOrder::Outcome::Ok;
        }
        if (fault_ != PassOrder::Fault::None) {
            if (windows_[0].made.empty()) {
                makeClientAction(0, sync::ActionKind::Send);
            }
            return PassOrder::Outcome::Ok;
        }
        const std::size_t count = rng_.below(3);
        for (std::size_t i = 0; i < count; ++i) {
            makeClientAction(rng_.below(kClients));
        }
        return PassOrder::Outcome::Ok;
    }

    PassOrder::Outcome hold(bool holdDrivers, std::string*) override {
        applyChanged_ = false;
        store_.beginBatch();
        if (holdDrivers) {
            drivers_->hold();
        }
        return PassOrder::Outcome::Ok;
    }

    PassOrder::Outcome apply(std::string* why) override {
        const std::size_t sendsBefore = host_.sends().size();
        if (store_.failed()) {
            bool drvChanged = false;
            if (!drivers_->serve(now_, &drvChanged)) {
                *why = "store failed before apply";
                applyFailClient_ = 0;
                applyFailTo_ = 0;
                return PassOrder::Outcome::Failed;
            }
        }

        // 1. Serve live driver (Hello or Done).
        if (const Supervisor* sup = drivers_->supervisor(kSession)) {
            FakeDriverHost::Proc* proc = host_.find(drivers_->pid(kSession));
            const Phase ph = sup->phase();
            const bool wantDriver = draining_ || fault_ != PassOrder::Fault::None || rng_.chance(2);
            if (proc != nullptr && ph == Phase::Starting && wantDriver) {
                proc->outBuf += driver::encode(driver::Hello{1, "claude", {"limit"}}).line;
                proc->outBuf +=
                    driver::encode(driver::Credential{driver::Billing::Subscription, "max"}).line;
                bool drvChanged = false;
                if (!drivers_->serve(now_, &drvChanged)) {
                    *why = "store failed in driver hello";
                    applyFailClient_ = 0;
                    applyFailTo_ = 0;
                    return PassOrder::Outcome::Failed;
                }
                if (drvChanged) {
                    applyChanged_ = true;
                }
                if (fault_ == PassOrder::Fault::NoHold && host_.sends().size() > sendsBefore &&
                    store_.unflushed() > 0) {
                    ++reached_;
                }
                record("[op |-> \"driver_hello\"");
            } else if (proc != nullptr && (ph == Phase::Working || ph == Phase::Stopping) &&
                       proc->activeTurn && *proc->activeTurn == sup->state().openTurn && wantDriver) {
                const std::uint64_t turn = *proc->activeTurn;
                if (ph == Phase::Working) {
                    proc->outBuf += driver::encode(driver::Text{turn, "reply:" + std::to_string(turn)}).line;
                    proc->outBuf += driver::encode(driver::Done{turn, driver::Outcome::Ok, {}, {}}).line;
                } else {
                    proc->outBuf += driver::encode(driver::Done{turn, driver::Outcome::Stopped, {}, {}}).line;
                }
                proc->activeTurn.reset();
                bool drvChanged = false;
                if (!drivers_->serve(now_, &drvChanged)) {
                    *why = "store failed in driver done";
                    applyFailClient_ = 0;
                    applyFailTo_ = 0;
                    return PassOrder::Outcome::Failed;
                }
                if (drvChanged) {
                    applyChanged_ = true;
                }
                if (fault_ == PassOrder::Fault::NoHold && host_.sends().size() > sendsBefore &&
                    store_.unflushed() > 0) {
                    ++reached_;
                }
                record("[op |-> \"driver_done\"");
            } else {
                bool drvChanged = false;
                if (!drivers_->serve(now_, &drvChanged)) {
                    *why = "store failed in drivers serve";
                    applyFailClient_ = 0;
                    applyFailTo_ = 0;
                    return PassOrder::Outcome::Failed;
                }
            }
        } else {
            bool drvChanged = false;
            if (!drivers_->serve(now_, &drvChanged)) {
                *why = "store failed in drivers serve";
                applyFailClient_ = 0;
                applyFailTo_ = 0;
                return PassOrder::Outcome::Failed;
            }
        }

        // 2. Receive from windows' links and apply in order.
        for (std::size_t c = 0; c < kClients; ++c) {
            Window& w = windows_[c];
            if (draining_ && applier_[c].applied() < w.made.size()) {
                w.upFlight.clear();
                w.upFlight.push_back(w.up.send(rng_, true));
            }
            if (w.upFlight.empty()) {
                continue;
            }
            if (!draining_ && fault_ == PassOrder::Fault::None && rng_.chance(3)) {
                continue;
            }
            const std::size_t idx = (draining_ || fault_ != PassOrder::Fault::None) ? w.upFlight.size() - 1
                                                                                    : rng_.below(w.upFlight.size());
            const Packet pkt = w.upFlight[idx];
            if (draining_ || fault_ != PassOrder::Fault::None || !rng_.chance(4)) {
                w.upFlight.erase(w.upFlight.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            const std::uint64_t rcvdBefore = upReceiver_[c].latest().next() - 1;
            upReceiver_[c].receive(pkt);
            const std::vector<sync::Action> parsed = sync::Actions::parse(upReceiver_[c].remoteDiff());
            const std::vector<sync::Action> taken = applier_[c].apply(parsed);
            const std::uint64_t rcvdNow = upReceiver_[c].latest().next() - 1;
            const std::uint64_t appNow = applier_[c].applied();

            if (!taken.empty()) {
                const std::size_t preActSends = host_.sends().size();
                for (const sync::Action& a : taken) {
                    std::string refused;
                    hub_->apply(a, &refused, clientName(c));
                    if (hub_->failed()) {
                        *why = "store failed in hub apply";
                        applyFailClient_ = c + 1;
                        applyFailTo_ = appNow;
                        return PassOrder::Outcome::Failed;
                    }
                    applyChanged_ = true;
                }
                if (fault_ == PassOrder::Fault::NoHold && host_.sends().size() > preActSends &&
                    store_.unflushed() > 0) {
                    ++reached_;
                }
                record("[op |-> \"apply\", c |-> " + std::to_string(c + 1) + ", to |-> " + std::to_string(appNow));
            } else if (rcvdNow > rcvdBefore) {
                record("[op |-> \"recv\", c |-> " + std::to_string(c + 1) + ", to |-> " + std::to_string(rcvdNow));
            }
        }
        return applyChanged_ ? PassOrder::Outcome::Changed : PassOrder::Outcome::Unchanged;
    }

    PassOrder::Outcome flush(std::string* why) override {
        if (!draining_ && fault_ == PassOrder::Fault::ReleaseOnFail && store_.unflushed() > 0 &&
            drivers_->unwritten(kSession) > 0 && !injectedFail_) {
            fs_.fail_at(1);
            injectedFail_ = true;
        } else if (!draining_ && fault_ == PassOrder::Fault::None && !store_.failed() && store_.unflushed() > 0 &&
                   rng_.chance(8)) {
            // 1: fail before log append; 2: fail after log append before log sync (leaves volatile batch to tear!);
            // 4: fail inside compaction after journal sync already made the batch durable (next pass hits ApplyFail!).
            const std::size_t mode = rng_.below(3);
            fs_.fail_at(mode == 0 ? 1 : mode == 1 ? 2 : 4);
        }
        const bool ok = store_.flush(why);
        fs_.fail_at(0);
        return ok ? PassOrder::Outcome::Ok : PassOrder::Outcome::Failed;
    }

    void release() override {
        const std::size_t sendsBefore = host_.sends().size();
        drivers_->release(now_);
        if (host_.sends().size() > sendsBefore) {
            if (fault_ == PassOrder::Fault::ReleaseBeforeFlush && store_.batching() && store_.unflushed() > 0) {
                ++reached_;
            }
            if (fault_ == PassOrder::Fault::ReleaseOnFail && flushFailedInPass_) {
                ++reached_;
            }
        }
    }

    void state() override {
        for (std::size_t c = 0; c < kClients; ++c) {
            downSender_[c].current = hub_->state();
        }
        if (fault_ == PassOrder::Fault::StateBeforeFlush && store_.batching() && store_.unflushed() > 0) {
            ++reached_;
        }
    }

    void crashAndRecover() {
        const std::uint64_t durBefore = store_.durableSeq();
        const std::size_t planPick = rng_.below(3);
        store::Rng storeRng(rng_.next());
        const store::MemFs::CrashPlan plan = planPick == 0 ? fs_.plan_all(false)
                                           : planPick == 1 ? fs_.plan_all(true)
                                                           : fs_.plan(storeRng);
        hub_.reset();
        drivers_.reset();
        host_.crashServer();
        {
            DurableStore empty;
            store_ = std::move(empty);
        }
        fs_.fail_at(0);
        fs_.crash(plan);

        std::string why;
        if (!store_.open(fs_, "/store", &why, storeOpts_)) {
            std::fprintf(stderr, "pass_walk: crash recovery open failed: %s\n", why.c_str());
            std::exit(1);
        }
        const std::uint64_t kept = store_.durableSeq() - durBefore;

        Drivers::Options opts;
        opts.table = "/drivers";
        opts.home = "/home";
        opts.timing = timing();
        opts.clock = [this] { return now_; };
        opts.machine = "box";
        opts.host = &host_;
        opts.readTable = [](std::string* text, std::string*) {
            *text = "driver claude /fake/driver\nai ada claude /work\n";
            return true;
        };
        opts.quiet = true;
        drivers_ = std::make_unique<Drivers>(store_, std::move(opts));
        hub_ = std::make_unique<sync::Hub>(store_, limits_, drivers_.get());
        drivers_->attach(*hub_);
        accounts::Registry reg;
        reg.ais.push_back(accounts::AI{"ada", {}, std::string()});
        hub_->useAccounts(reg, nullptr);
        if (!drivers_->start(now_)) {
            std::fprintf(stderr, "pass_walk: crash recovery drivers start failed\n");
            std::exit(1);
        }

        // Each window reconnects from acked[c] + 1; the server opens its Applier at disk.applied[c].
        for (std::size_t c = 0; c < kClients; ++c) {
            Window& w = windows_[c];
            const std::uint64_t acked = w.up.acknowledgedState().next() - 1;
            const std::uint64_t next = acked + 1;
            const std::uint64_t applied = store_.applied(clientName(c));

            w.up = FakeSender<sync::Actions>(sync::Actions(next));
            w.upFlight.clear();
            for (std::size_t i = static_cast<std::size_t>(acked); i < w.made.size(); ++i) {
                w.up.current.add(w.made[i]);
                w.upFlight.push_back(w.up.send(rng_, i == static_cast<std::size_t>(acked)));
            }
            upReceiver_[c] = FakeReceiver<sync::Actions>(sync::Actions(next));
            applier_[c] = sync::Applier(applied);
            downSender_[c] = FakeSender<sync::Sessions>();
            downSender_[c].current = hub_->state();
            w.down = FakeReceiver<sync::Sessions>();
        }

        if (!passFailed_) {
            ++crashes_;
        }
        pass_ = PassOrder(fault_);
        passFailed_ = false;
        flushFailedInPass_ = false;
        applyChanged_ = false;
        record("[op |-> \"crash\", kept |-> " + std::to_string(kept));
    }

    void runOneStep(bool forceNoCrash) {
        if (passFailed_) {
            crashAndRecover();
            return;
        }
        if (!forceNoCrash && fault_ == PassOrder::Fault::None && rng_.chance(14)) {
            crashAndRecover();
            return;
        }
        if (pass_.next() == PassOrder::Step::Done) {
            pass_ = PassOrder(fault_);
            flushFailedInPass_ = false;
        }
        const PassOrder::Step before = pass_.next();
        lastNonFailedStep_ = before;
        std::string why;
        const PassOrder::Step ran = take_pass_step(pass_, *this, &why);
        const bool nowFailed = pass_.next() == PassOrder::Step::Failed;
        passFailed_ = nowFailed;

        switch (ran) {
        case PassOrder::Step::Tick:
            if (fault_ == PassOrder::Fault::TickBeforeFlush && store_.batching() && store_.unflushed() > 0) {
                const std::uint64_t ack0 = windows_[0].up.acknowledgedState().next() - 1;
                if (ack0 > diskApplied0()) {
                    ++reached_;
                }
            }
            record("[op |-> \"tick\"");
            break;
        case PassOrder::Step::Wait:
            break;
        case PassOrder::Step::Hold:
        case PassOrder::Step::HoldUnheld:
            record("[op |-> \"hold\"");
            break;
        case PassOrder::Step::Apply:
            if (nowFailed) {
                ++fails_;
                record("[op |-> \"apply_fail\", c |-> " + std::to_string(applyFailClient_) + ", to |-> " +
                       std::to_string(applyFailTo_));
            } else {
                record("[op |-> \"applied\"");
            }
            break;
        case PassOrder::Step::Flush:
            if (nowFailed || !why.empty()) {
                ++fails_;
                flushFailedInPass_ = true;
                record("[op |-> \"flush_fail\"");
            } else {
                record("[op |-> \"flush\"");
            }
            break;
        case PassOrder::Step::Release:
            record("[op |-> \"release\"");
            break;
        case PassOrder::Step::State:
            record("[op |-> \"state\"");
            break;
        case PassOrder::Step::Done:
        case PassOrder::Step::Stopped:
        case PassOrder::Step::Failed:
            break;
        }
    }

    void runFaulty(std::size_t maxSteps) {
        const std::size_t budget = std::max<std::size_t>(maxSteps, 32);
        for (std::size_t i = 0; i < budget && reached_ == 0; ++i) {
            runOneStep(true);
        }
        // Finish the current pass after reaching the fault so post-fault steps record cleanly.
        for (std::size_t i = 0; i < 8 && pass_.next() != PassOrder::Step::Done && !passFailed_; ++i) {
            runOneStep(true);
        }
    }

    bool settled() const {
        if (passFailed_ || pass_.next() != PassOrder::Step::Done || store_.unflushed() != 0) {
            return false;
        }
        const sync::StoredSession* s = store_.find(kSession);
        if (s == nullptr || s->driver.openTurn != 0 || !s->driver.waiting.empty() || !s->queue.empty()) {
            return false;
        }
        for (std::size_t c = 0; c < kClients; ++c) {
            const std::uint64_t acked = windows_[c].up.acknowledgedState().next() - 1;
            if (acked != windows_[c].made.size() || store_.applied(clientName(c)) != windows_[c].made.size()) {
                return false;
            }
            if (!(windows_[c].shown == hub_->state())) {
                return false;
            }
        }
        return true;
    }

    void drain() {
        draining_ = true;
        for (int guard = 0; guard < 200 && !settled(); ++guard) {
            if (passFailed_ || store_.failed()) {
                crashAndRecover();
                continue;
            }
            runOneStep(true);
        }
    }

    int currentStepIdx() const {
        const PassOrder::Step s = pass_.next() == PassOrder::Step::Failed ? lastNonFailedStep_ : pass_.next();
        switch (s) {
        case PassOrder::Step::Tick:
        case PassOrder::Step::Done:
        case PassOrder::Step::Stopped:
            return 1;
        case PassOrder::Step::Wait:
        case PassOrder::Step::Hold:
        case PassOrder::Step::HoldUnheld:
            return 2;
        case PassOrder::Step::Apply:
            return 3;
        case PassOrder::Step::Flush:
            return 4;
        case PassOrder::Step::Release:
            return 5;
        case PassOrder::Step::State:
            return 6;
        case PassOrder::Step::Failed:
            return 3;
        }
        return 1;
    }

    std::string currentDrvPhase() const {
        const Supervisor* sup = drivers_ ? drivers_->supervisor(kSession) : nullptr;
        if (sup == nullptr) {
            return "\"none\"";
        }
        switch (sup->phase()) {
        case Phase::None:
        case Phase::Ending: return "\"none\"";
        case Phase::Starting: return "\"starting\"";
        case Phase::Idle: return "\"idle\"";
        case Phase::Working: return "\"working\"";
        case Phase::Stopping: return "\"stopping\"";
        }
        return "\"none\"";
    }

    static std::string formatStoreView(const DurableStore& st) {
        const sync::StoredSession* s = st.find(kSession);
        std::vector<std::string> tr;
        std::vector<std::string> q;
        std::vector<std::string> w;
        std::uint64_t open = 0;
        std::uint64_t next = 1;
        if (s != nullptr) {
            for (const sync::EntryRef& e : s->transcript) {
                tr.push_back(tlaEntry(*e));
            }
            for (const std::string& p : s->queue) {
                q.push_back(tlaPrompt(parsePrompt(p)));
            }
            for (const std::string& p : s->driver.waiting) {
                w.push_back(tlaPrompt(parsePrompt(p)));
            }
            open = s->driver.openTurn;
            next = s->driver.nextTurn;
        }
        std::vector<std::string> app;
        for (std::size_t c = 0; c < kClients; ++c) {
            app.push_back(std::to_string(st.applied(clientName(c))));
        }
        return "[tr |-> " + tlaSeq(tr) + ", queue |-> " + tlaSeq(q) + ", waiting |-> " + tlaSeq(w) +
               ", open |-> " + std::to_string(open) + ", next |-> " + std::to_string(next) +
               ", applied |-> " + tlaClientMap(app) + "]";
    }

    static std::string formatSnap(const sync::Sessions& sessions) {
        const sync::SessionView* v = sessions.find(kSession);
        std::vector<std::string> tr;
        std::vector<std::string> q;
        if (v != nullptr) {
            for (const sync::EntryRef& e : v->entries) {
                tr.push_back(tlaEntry(*e));
            }
            for (const std::string& p : v->queue) {
                q.push_back(tlaPrompt(parsePrompt(p)));
            }
        }
        return "[tr |-> " + tlaSeq(tr) + ", queue |-> " + tlaSeq(q) + "]";
    }

    std::string diskStoreView(std::uint64_t* c0Applied = nullptr) const {
        store::MemFs copy(fs_);
        copy.fail_at(0);
        copy.crash(copy.plan_all(false));
        DurableStore disk;
        std::string why;
        if (!disk.open(copy, "/store", &why, storeOpts_)) {
            std::fprintf(stderr, "pass_walk: diskStoreView failed: %s\n", why.c_str());
            std::exit(1);
        }
        if (c0Applied != nullptr) {
            *c0Applied = disk.applied(clientName(0));
        }
        return formatStoreView(disk);
    }

    std::uint64_t diskApplied0() const {
        std::uint64_t out = 0;
        diskStoreView(&out);
        return out;
    }

    void record(const std::string& head) {
        const std::string diskText = diskStoreView();
        std::vector<std::string> actsMap;
        std::vector<std::string> rcvdMap;
        std::vector<std::string> appMap;
        std::vector<std::string> ackMap;
        for (std::size_t c = 0; c < kClients; ++c) {
            std::vector<std::string> kinds;
            for (const sync::Action& a : windows_[c].made) {
                kinds.push_back(std::string("\"") + actionKindName(a.kind) + "\"");
            }
            actsMap.push_back(tlaSeq(kinds));
            rcvdMap.push_back(std::to_string(upReceiver_[c].latest().next() - 1));
            appMap.push_back(std::to_string(applier_[c].applied()));
            ackMap.push_back(std::to_string(windows_[c].up.acknowledgedState().next() - 1));
        }

        std::vector<std::string> sends;
        for (const FakeDriverHost::Sent& s : host_.sends()) {
            sends.push_back("[turn |-> " + std::to_string(s.turn) + ", prompt |-> " + tlaPrompt(s.prompt) + "]");
        }
        std::vector<std::string> stops;
        for (std::uint64_t t : host_.stops()) {
            stops.push_back(std::to_string(t));
        }

        const bool chg = pass_.next() == PassOrder::Step::Apply ? applyChanged_ : pass_.changed();
        std::ostringstream os;
        os << head
           << ", stepIdx |-> " << currentStepIdx()
           << ", failed |-> " << tlaBool(passFailed_)
           << ", batching |-> " << tlaBool(store_.batching())
           << ", holding |-> " << tlaBool(drivers_ && drivers_->holding())
           << ", changed |-> " << tlaBool(chg)
           << ", acts |-> " << tlaClientMap(actsMap)
           << ", rcvd |-> " << tlaClientMap(rcvdMap)
           << ", applier |-> " << tlaClientMap(appMap)
           << ", acked |-> " << tlaClientMap(ackMap)
           << ", mem |-> " << formatStoreView(store_)
           << ", disk |-> " << diskText
           << ", batchLen |-> " << store_.unflushed()
           << ", drvPhase |-> " << currentDrvPhase()
           << ", drvSends |-> " << tlaSeq(sends)
           << ", drvStops |-> " << tlaSeq(stops)
           << ", linkSnap |-> " << formatSnap(downSender_[0].current)
           << ", sentSnap |-> " << formatSnap(windows_[0].shown)
           << "]";
        records_.push_back(os.str());
    }

    Rng rng_;
    PassOrder::Fault fault_ = PassOrder::Fault::None;
    PassOrder pass_{PassOrder::Fault::None};
    PassOrder::Step lastNonFailedStep_ = PassOrder::Step::Tick;
    bool passFailed_ = false;
    bool flushFailedInPass_ = false;
    bool applyChanged_ = false;
    bool draining_ = false;
    bool injectedFail_ = false;
    std::size_t applyFailClient_ = 0;
    std::uint64_t applyFailTo_ = 0;
    std::size_t crashes_ = 0;
    std::size_t fails_ = 0;
    std::size_t reached_ = 0;
    Millis now_ = 1000;

    store::MemFs fs_;
    store::StoreOptions storeOpts_;
    sync::Limits limits_;
    FakeDriverHost host_;
    DurableStore store_;
    std::unique_ptr<Drivers> drivers_;
    std::unique_ptr<sync::Hub> hub_;
    std::vector<Window> windows_;
    std::vector<FakeReceiver<sync::Actions>> upReceiver_;
    std::vector<sync::Applier> applier_;
    std::vector<FakeSender<sync::Sessions>> downSender_;
    std::vector<std::string> records_;
};

bool parseFault(const std::string& name, PassOrder::Fault* out) {
    if (name == "release-before-flush") {
        *out = PassOrder::Fault::ReleaseBeforeFlush;
        return true;
    }
    if (name == "state-before-flush") {
        *out = PassOrder::Fault::StateBeforeFlush;
        return true;
    }
    if (name == "tick-before-flush") {
        *out = PassOrder::Fault::TickBeforeFlush;
        return true;
    }
    if (name == "no-hold") {
        *out = PassOrder::Fault::NoHold;
        return true;
    }
    if (name == "release-on-fail") {
        *out = PassOrder::Fault::ReleaseOnFail;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5 && argc != 6) {
        std::fprintf(stderr, "usage: pass_walk <seed> <steps> [--fault <name> | --faulty-<name>] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const std::size_t steps = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    PassOrder::Fault fault = PassOrder::Fault::None;
    std::string outPath;
    if (argc == 4) {
        outPath = argv[3];
    } else if (argc == 5) {
        const std::string arg = argv[3];
        if (arg.rfind("--faulty-", 0) != 0 || !parseFault(arg.substr(9), &fault)) {
            std::fprintf(stderr, "pass_walk: unknown option '%s'\n", arg.c_str());
            return 2;
        }
        outPath = argv[4];
    } else {
        if (std::string(argv[3]) != "--fault" || !parseFault(argv[4], &fault)) {
            std::fprintf(stderr, "pass_walk: unknown fault '%s %s'\n", argv[3], argv[4]);
            return 2;
        }
        outPath = argv[5];
    }

    LoopWalk walk(seed, fault);
    walk.run(steps);
    if (fault != PassOrder::Fault::None && walk.reached() == 0) {
        std::fprintf(stderr, "pass_walk: fault was never exercised\n");
        return 1;
    }
    walk.writeModule(outPath);
    return 0;
}
