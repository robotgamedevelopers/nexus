// Trace walk: drives the secret dialog's code (src/sync/secrets.h, through src/sync/hub.h and src/server/drivers.h
// over a MemoryStore) with a seeded random walk and writes the trace as SecretsTraceData.tla, for TLC to check against
// models/Secrets.tla (models/SecretsTrace.tla). No real files, processes, clock or sockets: the time is the walk's
// counter, the drivers run over an in-memory Drivers::Host harness, the values' files are the server's AccountFiles
// over MemFs, and the values are fake ones the walk makes up.
//
//   secrets_walk <seed> <steps> [--format 3|4]
//       [--faulty-store-late | --faulty-echo | --faulty-driver | --faulty-driver-echo] <out.tla>
//
// --format is the format of the windows that take requests and of their actions (3 unless given): CI walks both, so
// both the hand-written codec and schema's tables carry the values the model checks.
//
// The pieces are the real ones, wired as nexus-server and the windows wire them: Drivers over an in-memory fake
// harness (Drivers::Host) starting a driver per AI session, writing `server` (`features: ["secrets"]`) after each
// `hello`, reading `ask-secret` from the driver's standard output in Drivers::serve, holding `secret` answers while the
// step's batch is held (Drivers::hold) and flushing them to the driver's standard input on Drivers::release; the hub's
// askSecret, apply, expireSecrets, retrySecrets and dropSecrets (a restart ends the old Drivers and Hub and starts new
// ones over the same store); the server's AccountFiles writing each value (server/secret_files.h) and its SecretLines
// sending each session answer to Drivers::tellSecret and each log line to server.log (server/secret_lines.h); each
// window's state is the hub's Sessions sent as a diff in the window's format (the walk's, 2 for the old window, which
// never receives a request) and applied to the window's copy, then given to its SecretPrompt; each answer or cancel is
// an Action encoded by Actions in that format and parsed back by Actions::parse before the hub applies it. Each
// session asker's reply in the trace is built solely from the `secret` line decoded off its driver's standard input
// (driver::LineReader and decodeServerMessage).
//
// The one-chance rule (models/Secrets.tla ReceivedNeverDropped): the walk's disk fails and heals (`diskfail`,
// `diskheal`), and while it fails the keeper wrapped around AccountFiles fails a write, or lets it land and reports it
// unconfirmed; the desk holds the value and the walk runs its retry when its time comes (`retry`). One paste in five
// is a value not of a token's shape (refused malformed, and nexus asks again: an `ask` by nexus). Each record also
// holds the disk, its failures so far, the value the server received for each request (`taken`), the value held for
// each, and the values lost at a restart.
//
// The bytes each place received are what the trace says of a value there: a log line, a `secret` line read on a
// driver's standard input, a session line (and any entry of the store) or a diff sent to a window that holds a value
// (tests/leak_scan.h: the value, a run of 8 of its bytes, its hex or its base64) is written with that value's token in
// its record, so TLC sees the leak in the code (Secrets.tla NoSecretInLog, NoSecretToAsker, NoSecretInTranscript,
// NoSecretToWindows). Every other line written to a driver's standard input, every driver command-line argument, an
// action's description and the hub's refusals fail the walk at once if they carry a value.
//
// The faults show the binding can fail:
//   --faulty-store-late   an answer for a request no longer open is written to the account's file anyway
//   --faulty-echo         a stored answer's log line carries the value (SecretDesk::injectEchoFault)
//   --faulty-driver       a session asker's `secret` line is dropped before Drivers::tellSecret, so its driver's
//                         standard input never receives it (rejected by TraceMatches)
//   --faulty-driver-echo  a stored answer's `secret` line written through Drivers::tellSecret carries the value on the
//                         driver's standard input (rejected by CodeNoSecretToAsker)

#include <errno.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accounts/registry.h"
#include "driver/protocol.h"
#include "leak_scan.h"
#include "server/drivers.h"
#include "server/secret_files.h"
#include "server/secret_lines.h"
#include "store/fs.h"
#include "sync/actions.h"
#include "sync/hub.h"
#include "sync/secrets.h"
#include "sync/sessions.h"

namespace {

using namespace nexus;

const std::vector<std::string> kAccts{"a1", "a2"};
const std::vector<std::string> kClients{"c1", "c2", "c3"};
const std::string kOld = "c3";             // a window from before secret requests (state format 2)
constexpr std::uint64_t kMaxRequests = 16;
constexpr std::uint64_t kMaxRestarts = 2;
constexpr std::uint64_t kMaxFailures = 4;
const char* const kNone = "none";
const char* const kWhat = "oauth-token";

// The fake values, made up here: never a real token. Each is a token of the model (v1, v2; b1, not of the shape of a
// token: two words, so the server refuses it "malformed").
const std::vector<std::string> kValues{"fakeWk7Hq2Lone", "fakeWk9Zx4Ptwo", "fakeWk3Rb bad"};
const std::vector<std::string> kTokens{"v1", "v2", "b1"};

std::uint64_t rngState = 0;

// The format of the windows that take requests and of their actions (--format): 3, the first with secret requests, or
// 4, schema's tables, where a value rides as table bytes through the wiping allocator and the wiped read region. The
// old window is format 2 either way.
std::uint32_t walkFormat = sync::kSecretsFormat;
std::uint64_t next() {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return rngState * 2685821657736338717ULL;
}
std::size_t pick(std::size_t n) { return static_cast<std::size_t>(next() % n); }

std::string q(const std::string& s) { return "\"" + s + "\""; }

// A value's token as the trace writes it: a model value (v1, v2: SecretsTraceData's constants, made model values by
// SecretsTrace.cfg, as the model checks' configs make theirs), or the string "none". A model value, unlike a string,
// compares with the ids beside it in a record (Secrets.tla NoValueIn).
std::string val(const std::string& token) { return token == kNone ? q(token) : token; }

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "secrets_walk: %s\n", what.c_str());
    std::exit(1);
}

// The token of the value `text` gives away (leak_scan.h), or empty.
std::string leaked(const std::string& text) {
    static const std::vector<test::LeakScan> scans{test::LeakScan(kValues[0]), test::LeakScan(kValues[1]),
                                                   test::LeakScan(kValues[2])};
    for (std::size_t i = 0; i < scans.size(); ++i) {
        if (scans[i].in(text)) {
            return kTokens[i];
        }
    }
    return std::string();
}

// Fails if `text`, which `where` names, gives a value away: for the places the model has no variable for.
void clean(const std::string& text, const std::string& where) {
    if (!leaked(text).empty()) {
        fail("a value reached " + where);
    }
}

// The field of a record for text that may hold a value: the token of the value it gives away, else `field` quoted.
std::string orToken(const std::string& bytes, const std::string& field) {
    const std::string t = leaked(bytes);
    return t.empty() ? "\"" + field + "\"" : t;
}

// The token of a value, or of what was pasted for it (with a line break or spaces at its ends).
std::string tokenOf(const std::string& pasted) {
    const std::size_t first = pasted.find_first_not_of(" \t\r\n");
    const std::size_t last = pasted.find_last_not_of(" \t\r\n");
    const std::string value = first == std::string::npos ? std::string() : pasted.substr(first, last - first + 1);
    for (std::size_t i = 0; i < kValues.size(); ++i) {
        if (kValues[i] == value) {
            return kTokens[i];
        }
    }
    fail("a value the walk never typed: " + std::to_string(pasted.size()) + " bytes");
}

// One `secret` line decoded off a fake driver's standard input, with the raw line bytes it arrived in.
struct DriverAnswer {
    pid_t pid = -1;
    std::uint64_t ask = 0;
    bool stored = false;
    std::string reason;
    std::string rawLine;
};

struct AskOrigin {
    sync::SessionId session = 0;
    std::uint64_t requestId = 0;
};

// In-memory stand-in for the AI sessions' driver processes (Drivers::Host): each spawned driver writes `hello` on
// its standard output, checks that `server` (`features: ["secrets"]`) arrives on its standard input before `send`,
// writes `ask-secret` lines when the walk asks as that session, and decodes every line arriving on its standard input
// with driver::LineReader and decodeServerMessage.
class FakeHarness final : public server::Drivers::Host {
public:
    pid_t spawn(const std::vector<std::string>& argv, int* in, int* out, std::string* why) override {
        if (argv.empty()) {
            *why = "empty driver argv";
            return -1;
        }
        for (const std::string& a : argv) {
            clean(a, "a driver's command line");
        }
        const pid_t pid = nextPid_++;
        const int inFd = nextFd_++;
        const int outFd = nextFd_++;
        Proc p;
        p.pid = pid;
        p.inWriteFd = inFd;
        p.outReadFd = outFd;
        const driver::Encoded hello = driver::encode(driver::Hello{driver::kProtocolVersion, "claude", {"limit"}});
        if (!hello.error.empty()) {
            fail("could not encode fake driver hello: " + hello.error);
        }
        p.outBuf = hello.line;
        procs_.emplace(pid, std::move(p));
        inFdToPid_[inFd] = pid;
        outFdToPid_[outFd] = pid;
        *in = inFd;
        *out = outFd;
        return pid;
    }

    ssize_t write(int fd, const char* data, std::size_t size, int* err) override {
        auto mapIt = inFdToPid_.find(fd);
        if (mapIt == inFdToPid_.end()) {
            *err = EBADF;
            return -1;
        }
        auto it = procs_.find(mapIt->second);
        if (it == procs_.end() || it->second.inClosed || it->second.dead) {
            *err = EPIPE;
            return -1;
        }
        Proc& p = it->second;
        std::vector<driver::LineReader::Line> lines;
        p.inLines.feed(std::string_view(data, size), lines);
        for (const driver::LineReader::Line& l : lines) {
            if (!l.error.empty()) {
                fail("malformed line written to driver stdin: " + l.error);
            }
            const driver::Decoded<driver::ServerMessage> dec = driver::decodeServerMessage(l.text);
            if (!dec.error.empty() || !dec.message) {
                fail("undecodable server message on driver stdin: " + dec.error);
            }
            if (const auto* info = std::get_if<driver::ServerInfo>(&*dec.message)) {
                clean(l.text, "a driver's server line");
                if (p.sawSend) {
                    fail("driver received `server` after `send`");
                }
                if (std::find(info->features.begin(), info->features.end(), "secrets") == info->features.end()) {
                    fail("driver `server` line did not advertise `secrets`");
                }
                p.sawSecretsInfo = true;
            } else if (std::holds_alternative<driver::Send>(*dec.message)) {
                clean(l.text, "a driver's send line");
                if (!p.sawSecretsInfo) {
                    fail("driver received `send` before `server` (`secrets`)");
                }
                p.sawSend = true;
            } else if (const auto* ans = std::get_if<driver::SecretAnswer>(&*dec.message)) {
                if (!p.sawSecretsInfo) {
                    fail("driver received `secret` before `server` (`secrets`)");
                }
                answers_.push_back(DriverAnswer{p.pid, ans->ask, ans->stored, ans->reason, l.text});
                ++totalAnswers_;
            } else {
                clean(l.text, "a driver's standard input");
            }
        }
        *err = 0;
        return static_cast<ssize_t>(size);
    }

    ssize_t read(int fd, char* data, std::size_t size, int* err) override {
        auto mapIt = outFdToPid_.find(fd);
        if (mapIt == outFdToPid_.end()) {
            *err = EBADF;
            return -1;
        }
        auto it = procs_.find(mapIt->second);
        if (it == procs_.end()) {
            *err = 0;
            return 0;
        }
        Proc& p = it->second;
        if (!p.outBuf.empty()) {
            const std::size_t n = std::min(size, p.outBuf.size());
            std::memcpy(data, p.outBuf.data(), n);
            p.outBuf.erase(0, n);
            *err = 0;
            return static_cast<ssize_t>(n);
        }
        if (p.outClosed || p.dead) {
            *err = 0;
            return 0;
        }
        *err = EAGAIN;
        return -1;
    }

    void close(int fd) override {
        if (auto inIt = inFdToPid_.find(fd); inIt != inFdToPid_.end()) {
            if (auto pIt = procs_.find(inIt->second); pIt != procs_.end()) {
                pIt->second.inClosed = true;
            }
            inFdToPid_.erase(inIt);
        }
        if (auto outIt = outFdToPid_.find(fd); outIt != outFdToPid_.end()) {
            if (auto pIt = procs_.find(outIt->second); pIt != procs_.end()) {
                pIt->second.outClosed = true;
            }
            outFdToPid_.erase(outIt);
        }
    }

    void killGroup(pid_t pid, int sig) override {
        if (auto it = procs_.find(pid); it != procs_.end()) {
            it->second.dead = true;
            it->second.killedSig = sig;
        }
    }

    bool exited(pid_t pid) override {
        auto it = procs_.find(pid);
        return it != procs_.end() && it->second.dead;
    }

    std::string reap(pid_t pid) override {
        int sig = 9;
        if (auto it = procs_.find(pid); it != procs_.end()) {
            sig = it->second.killedSig != 0 ? it->second.killedSig : 9;
            procs_.erase(it);
        }
        return "was ended by signal " + std::to_string(sig);
    }

    bool ready(pid_t pid) const {
        auto it = procs_.find(pid);
        return it != procs_.end() && !it->second.dead && it->second.sawSecretsInfo && it->second.sawSend;
    }

    std::uint64_t emitAskSecret(pid_t pid, sync::SessionId session, std::uint64_t expectedReqId,
                                const std::string& account, const std::string& what, const std::string& why) {
        auto it = procs_.find(pid);
        if (it == procs_.end() || it->second.dead) {
            fail("cannot emit ask-secret from dead driver " + std::to_string(pid));
        }
        const std::uint64_t ask = ++it->second.nextAsk;
        const driver::Encoded enc = driver::encode(driver::AskSecret{ask, account, what, why});
        if (!enc.error.empty()) {
            fail("could not encode ask-secret: " + enc.error);
        }
        asks_[{pid, ask}] = AskOrigin{session, expectedReqId};
        it->second.outBuf += enc.line;
        ++totalAsks_;
        return ask;
    }

    AskOrigin lookupAsk(pid_t pid, std::uint64_t ask) const {
        auto it = asks_.find({pid, ask});
        if (it == asks_.end()) {
            fail("driver " + std::to_string(pid) + " received `secret` for unknown ask " + std::to_string(ask));
        }
        return it->second;
    }

    bool hasAnswers() const { return !answers_.empty(); }

    std::vector<DriverAnswer> takeAnswers() {
        std::vector<DriverAnswer> out = std::move(answers_);
        answers_.clear();
        return out;
    }

    std::size_t totalAsks() const { return totalAsks_; }
    std::size_t totalAnswers() const { return totalAnswers_; }

private:
    struct Proc {
        pid_t pid = -1;
        int inWriteFd = -1;
        int outReadFd = -1;
        std::string outBuf;
        bool inClosed = false;
        bool outClosed = false;
        bool dead = false;
        int killedSig = 0;
        driver::LineReader inLines;
        bool sawSecretsInfo = false;
        bool sawSend = false;
        std::uint64_t nextAsk = 0;
    };

    pid_t nextPid_ = 100;
    int nextFd_ = 10;
    std::map<pid_t, Proc> procs_;
    std::map<int, pid_t> inFdToPid_;
    std::map<int, pid_t> outFdToPid_;
    std::map<std::pair<pid_t, std::uint64_t>, AskOrigin> asks_;
    std::vector<DriverAnswer> answers_;
    std::size_t totalAsks_ = 0;
    std::size_t totalAnswers_ = 0;
};

// The accounts' files: the server's AccountFiles, over MemFs. Each value it writes durably is counted. While the walk's
// disk fails (Secrets.tla DiskFails), a write either fails before the file is touched, or reaches the file and has its
// durability unconfirmed (the directory's sync failed): the two ways AccountFiles reports a write that is not durable,
// each of whose own paths secrets_test files_crash covers on MemFs.
struct Keeper : sync::SecretKeeper {
    store::MemFs fs;
    server::AccountFiles files{fs, "accounts"};
    std::size_t writes = 0;
    bool failing = false;
    sync::Kept keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                    std::string* why) override {
        if (failing) {
            if (pick(2) == 0) {
                *why = "the disk is full (the walk's)";
                return sync::Kept::Failed;
            }
            files.keep(account, what, value, why);
            *why = "the sync of its directory failed (the walk's)";
            return sync::Kept::Unconfirmed;
        }
        const sync::Kept kept = files.keep(account, what, value, why);
        if (kept == sync::Kept::Durable) {
            ++writes;
        }
        return kept;
    }
    // The bytes of `account`'s file, or none.
    std::optional<std::string> file(const std::string& account) {
        store::Bytes b;
        if (!fs.read_all(files.path(account, kWhat), b).is_ok()) {
            return std::nullopt;
        }
        return std::string(b.begin(), b.end());
    }
};

// The server's SecretLines wired to Drivers::tellSecret, as nexus-server wires it (src/server/server.cpp). Answers for
// nexus itself (session 0, which has no driver) are kept in nexusReplies; answers for session askers go to
// Drivers::tellSecret and are read back only from each driver's standard input in FakeHarness.
struct Events : server::SecretLines {
    server::Drivers* drivers = nullptr;
    Keeper* keeper = nullptr;
    std::string fault;
    std::size_t* faults = nullptr;
    std::vector<sync::SecretReply> nexusReplies;    // session 0 only
    std::vector<sync::SecretNote> notes;
    std::vector<sync::SecretLog> log;
    std::vector<std::string> logLines;              // the server.log line of each
    std::string logLine;

    Events()
        : server::SecretLines([this](sync::SessionId session, const std::string& line) {
                                  if (drivers != nullptr) {
                                      drivers->tellSecret(session, line);
                                  }
                              },
                              [this](const std::string& line) { logLine = line; }) {}

    void answered(const sync::SecretReply& reply) override {
        if (reply.asker.session == 0) {
            server::SecretLines::answered(reply);
            nexusReplies.push_back(reply);
            return;
        }
        if (fault == "driver") {
            ++*faults;
            return;     // dropped before Drivers::tellSecret: the driver's standard input never receives it
        }
        if (fault == "driver-echo" && reply.stored && keeper != nullptr && drivers != nullptr) {
            if (const std::optional<std::string> secret = keeper->file(reply.account)) {
                std::string line = driver::encode(driver::SecretAnswer{reply.asker.ask, true, {}}).line;
                const std::size_t closeBrace = line.rfind("}\n");
                if (closeBrace == std::string::npos) {
                    fail("unexpected encoded secret line shape");
                }
                line.replace(closeBrace, 2, ",\"debug\":\"" + *secret + "\"}\n");
                drivers->tellSecret(reply.asker.session, line);
                ++*faults;
                return;
            }
        }
        server::SecretLines::answered(reply);
    }

    void logged(const sync::SecretLog& line) override {
        logLine.clear();
        server::SecretLines::logged(line);
        log.push_back(line);
        logLines.push_back(logLine);
    }

    void noted(const sync::SecretNote& note) override { notes.push_back(note); }
};

struct Window {
    std::string name;
    bool old = false;
    bool conn = true;
    sync::Sessions got;             // the state as the window last received it
    sync::SecretPrompt prompt;
    std::string leak;               // the token of a value a diff sent to it gave away, if one did
};

struct Flight {
    std::string wire;               // the action as it travels: Actions in the walk's format
    std::uint64_t id = 0;
    bool secret = false;
    std::string token;              // the value's token, or none for a cancel
};

struct Req {
    std::string st = "unused";
    std::string acct = kNone;
    std::string asker = kNone;
};

struct Walk {
    sync::MemoryStore store;
    FakeHarness harness;
    Keeper keeper;
    Events events;
    std::unique_ptr<server::Drivers> drivers;
    std::unique_ptr<sync::Hub> hub;
    std::uint64_t now = 1000;
    std::vector<Window> windows;
    std::vector<Flight> up;
    std::map<std::uint64_t, Req> req;           // what each id was asked as, and its state as last reported
    std::map<std::uint64_t, std::size_t> stores;
    std::set<std::string> replies;              // every answer so far, as model records
    std::set<std::uint64_t> answeredIds;
    std::vector<std::string> transcript;        // this step's lines, as model records
    std::vector<std::string> log;
    std::uint64_t restarts = 0;
    std::uint64_t failures = 0;
    std::map<std::uint64_t, std::string> taken;   // the token of each value the server took (Secrets.tla taken)
    std::map<std::uint64_t, std::string> heldTok; // the token of each value the server holds while its state is holding
    std::set<std::uint64_t> lost;               // the requests whose held value went with a restart
    std::uint64_t num = 0;
    std::uint64_t driverNum = 0;
    std::map<sync::SessionId, std::string> askerNames{{0, "nexus"}};
    bool echoFault = false;

    Walk(const std::string& faultyMode, std::size_t* faultCount) {
        events.keeper = &keeper;
        events.fault = faultyMode;
        events.faults = faultCount;
        for (const std::string& c : kClients) {
            Window w;
            w.name = c;
            w.old = c == kOld;
            w.got.setFormat(w.old ? 2 : walkFormat);
            windows.push_back(std::move(w));
        }
        // Two AIs' sessions, whose AIs ask through their drivers.
        {
            sync::Hub bootstrap(store);
            for (const char* f : {"f", "g"}) {
                std::string why;
                sync::Action a;
                a.num = ++num;
                a.kind = sync::ActionKind::Ensure;
                a.aiName = f;
                bootstrap.apply(a, &why, "walk");
            }
        }
        for (sync::SessionId id : store.ids()) {
            askerNames[id] = "s" + std::to_string(id);
        }
        if (askerNames.size() != 3 || askerNames.count(1) == 0 || askerNames.count(2) == 0) {
            fail("the askers' sessions are not 1 and 2");
        }
        start();
    }

    ~Walk() { stopServer(); }

    void stopServer() {
        events.drivers = nullptr;
        hub.reset();
        drivers.reset();
    }

    // The server's start: Drivers and Hub over the store, with accounts, the secret dialog, and each AI session's
    // driver started past its `hello` and `server` (`features: ["secrets"]`) into an open turn.
    void start() {
        server::Drivers::Options opts;
        opts.table = "drivers";
        opts.home = "/home/walk";
        opts.clock = [this] { return static_cast<server::Millis>(now); };
        opts.machine = "walk";
        opts.secrets = true;
        opts.host = &harness;
        opts.quiet = true;
        opts.readTable = [](std::string* text, std::string*) {
            *text = "driver claude /fake/driver\n"
                    "ai f claude /work/f\n"
                    "ai g claude /work/g\n";
            return true;
        };
        drivers = std::make_unique<server::Drivers>(store, std::move(opts));
        hub = std::make_unique<sync::Hub>(store, sync::Limits{}, drivers.get());
        drivers->attach(*hub);
        events.drivers = drivers.get();
        configure();
        if (!drivers->start(static_cast<server::Millis>(now))) {
            fail("Drivers::start failed");
        }
        ensureDriverTurns();
    }

    void ensureDriverTurns() {
        for (sync::SessionId id : {sync::SessionId(1), sync::SessionId(2)}) {
            sync::Action a;
            a.num = ++driverNum;
            a.kind = sync::ActionKind::Send;
            a.session = id;
            a.text = "work";
            std::string why;
            if (!hub->apply(a, &why, "walk-drivers")) {
                fail("driver turn send refused: " + why);
            }
        }
        bool changed = false;
        if (!drivers->serve(static_cast<server::Millis>(now), &changed)) {
            fail("Drivers::serve failed at start");
        }
        for (sync::SessionId id : {sync::SessionId(1), sync::SessionId(2)}) {
            const pid_t p = drivers->pid(id);
            if (p <= 0 || !harness.ready(p)) {
                fail("session " + std::to_string(id) + " has no ready driver after start");
            }
        }
    }

    void configure() {
        accounts::Registry r;
        for (const std::string& a : kAccts) {
            r.accounts.push_back(accounts::Account{a, "claude"});
        }
        // Both AIs linked to both accounts: their AIs may ask for either.
        for (const char* f : {"f", "g"}) {
            r.ais.push_back(accounts::AI{f, kAccts, kAccts[0]});
        }
        hub->useAccounts(r, nullptr);
        if (echoFault) {
            hub->injectSecretEchoFault();
        }
        hub->secretsWith(&keeper, &events, [this] {
            return std::any_of(windows.begin(), windows.end(), [](const Window& w) { return w.conn && !w.old; });
        }, [this] { return now; });
    }

    void holdBatch() {
        if (harness.hasAnswers()) {
            fail("uncollected driver secret answer before step");
        }
        drivers->hold();
    }

    void releaseBatch() {
        if (harness.hasAnswers()) {
            fail("a secret line reached a driver's standard input while Drivers::holding() was true");
        }
        drivers->release(static_cast<server::Millis>(now));
    }

    std::uint64_t nextId() const { return store.applied(sync::kSecretIdsClient) + 1; }

    // The step's effects, as the model's records, each from the bytes its place received: a record whose bytes give a
    // value away carries that value's token, which the model's NoSecretIn* properties refuse.
    void take() {
        transcript.clear();
        log.clear();
        for (const sync::SecretNote& n : events.notes) {
            transcript.push_back("[id |-> " + std::to_string(n.id) + ", what |-> " + orToken(n.text, n.what) +
                                 ", acct |-> " + q(n.account) + "]");
        }
        // Every entry the store holds, every session line included: one that gives a value away is a session line
        // with it.
        for (sync::SessionId id : store.ids()) {
            for (const sync::EntryRef& e : store.find(id)->transcript) {
                const std::string t = leaked(e->text);
                if (!t.empty()) {
                    transcript.push_back("[id |-> 0, what |-> " + t + ", acct |-> " + q(kNone) + "]");
                }
            }
        }
        for (std::size_t i = 0; i < events.log.size(); ++i) {
            const sync::SecretLog& l = events.log[i];
            log.push_back("[id |-> " + std::to_string(l.id) + ", event |-> " +
                          orToken(events.logLines[i] + "\n" + l.line(), l.event) + "]");
        }
        // nexus's own sign-in asks (session 0, no driver process).
        for (const sync::SecretReply& r : events.nexusReplies) {
            if (!answeredIds.insert(r.id).second) {
                fail("request " + std::to_string(r.id) + " was answered twice");
            }
            replies.insert("[id |-> " + std::to_string(r.id) + ", asker |-> " + q(askerNames.at(0)) +
                           ", outcome |-> " + q(r.stored ? "stored" : "refused") + ", reason |-> " +
                           orToken(r.reason, r.stored ? kNone : r.reason) + "]");
        }
        // AI session asks (sessions 1 and 2): built exclusively from the `secret` lines decoded off each driver's
        // standard input pipe.
        for (const DriverAnswer& ans : harness.takeAnswers()) {
            const AskOrigin origin = harness.lookupAsk(ans.pid, ans.ask);
            if (!answeredIds.insert(origin.requestId).second) {
                fail("request " + std::to_string(origin.requestId) + " was answered twice on driver stdin");
            }
            replies.insert("[id |-> " + std::to_string(origin.requestId) + ", asker |-> " +
                           q(askerNames.at(origin.session)) + ", outcome |-> " +
                           q(ans.stored ? "stored" : "refused") + ", reason |-> " +
                           orToken(ans.rawLine + "\n" + ans.reason, ans.stored ? kNone : ans.reason) + "]");
        }
        events.nexusReplies.clear();
        events.notes.clear();
        events.log.clear();
        events.logLines.clear();
        // Each request's state, as the hub reports it; one it no longer knows (from before a restart) keeps the last.
        for (auto& [id, r] : req) {
            if (const std::optional<sync::SecretState> s = hub->secrets().stateOf(id)) {
                static const char* const names[] = {"open", "stored", "refused", "dropped", "holding", "expired"};
                r.st = names[static_cast<int>(*s)];
            }
        }
        // The server holds a value only while its request is holding.
        for (auto it = heldTok.begin(); it != heldTok.end();) {
            it = req[it->first].st == "holding" ? std::next(it) : heldTok.erase(it);
        }
    }

    // A function of the ids to tokens, with "none" for an id not in `m`.
    static std::string byId(const std::map<std::uint64_t, std::string>& m) {
        std::string out = "(";
        for (std::uint64_t id = 1; id <= kMaxRequests; ++id) {
            auto it = m.find(id);
            out += std::string(id == 1 ? "" : " @@ ") + std::to_string(id) + " :> " +
                val(it == m.end() ? std::string(kNone) : it->second);
        }
        return out + ")";
    }

    // A diff sent to the window that gave a value away is a request in its view holding that value.
    std::string viewOf(const Window& w) const {
        std::string out = "{";
        bool first = true;
        for (const sync::SecretRequest& r : w.prompt.view()) {
            out += std::string(first ? "" : ", ") + "[id |-> " + std::to_string(r.id) + ", acct |-> " + q(r.account) +
                ", asker |-> " + q(askerNames.at(r.asker)) + "]";
            first = false;
        }
        if (!w.leak.empty()) {
            out += std::string(first ? "" : ", ") + "[id |-> 0, acct |-> " + w.leak + ", asker |-> " + q(kNone) + "]";
        }
        return out + "}";
    }

    std::string dialogOf(const Window& w) const {
        const sync::SecretRequest* r = w.prompt.showing();
        const std::string& typed = w.prompt.typed().bytes();
        return "[id |-> " + std::to_string(r != nullptr ? r->id : 0) + ", typed |-> " +
            val(typed.empty() ? std::string(kNone) : tokenOf(typed)) + "]";
    }

    std::string doneOf(const Window& w) const {
        std::string out = "{";
        bool first = true;
        for (std::uint64_t id : w.prompt.answered()) {
            out += std::string(first ? "" : ", ") + std::to_string(id);
            first = false;
        }
        return out + "}";
    }

    template <class F>
    std::string byClient(F f) const {
        std::string out = "(";
        for (std::size_t i = 0; i < windows.size(); ++i) {
            out += std::string(i == 0 ? "" : " @@ ") + q(windows[i].name) + " :> " + f(windows[i]);
        }
        return out + ")";
    }

    static std::string set(const std::vector<std::string>& items) {
        std::set<std::string> s(items.begin(), items.end());
        std::string out = "{";
        bool first = true;
        for (const std::string& x : s) {
            out += std::string(first ? "" : ", ") + x;
            first = false;
        }
        return out + "}";
    }

    static std::string flight(const Flight& m) {
        return "[id |-> " + std::to_string(m.id) + ", kind |-> " + q(m.secret ? "secret" : "cancel") + ", v |-> " +
            val(m.secret ? m.token : kNone) + "]";
    }

    std::string record() {
        std::ostringstream o;
        o << "next |-> " << nextId() << ", req |-> (";
        for (std::uint64_t id = 1; id <= kMaxRequests; ++id) {
            auto it = req.find(id);
            const Req r = it == req.end() ? Req{} : it->second;
            o << (id == 1 ? "" : " @@ ") << id << " :> [st |-> " << q(r.st) << ", acct |-> " << q(r.acct)
              << ", asker |-> " << q(r.asker) << "]";
        }
        o << "), conn |-> " << byClient([](const Window& w) { return std::string(w.conn ? "TRUE" : "FALSE"); });
        o << ", view |-> " << byClient([this](const Window& w) { return viewOf(w); });
        o << ", dialog |-> " << byClient([this](const Window& w) { return dialogOf(w); });
        o << ", done |-> " << byClient([this](const Window& w) { return doneOf(w); });
        std::vector<std::string> flights;
        for (const Flight& m : up) {
            flights.push_back(flight(m));
        }
        o << ", up |-> " << set(flights) << ", file |-> (";
        for (std::size_t i = 0; i < kAccts.size(); ++i) {
            const std::optional<std::string> file = keeper.file(kAccts[i]);
            o << (i == 0 ? "" : " @@ ") << q(kAccts[i]) << " :> " << val(file ? tokenOf(*file) : std::string(kNone));
        }
        o << "), stores |-> (";
        for (std::uint64_t id = 1; id <= kMaxRequests; ++id) {
            auto it = stores.find(id);
            o << (id == 1 ? "" : " @@ ") << id << " :> " << (it == stores.end() ? 0 : it->second);
        }
        o << "), replies |-> " << set(std::vector<std::string>(replies.begin(), replies.end()))
          << ", transcript |-> " << set(transcript) << ", log |-> " << set(log) << ", restarts |-> " << restarts
          << ", disk |-> " << q(keeper.failing ? "failing" : "ok") << ", failures |-> " << failures
          << ", held |-> " << byId(heldTok) << ", taken |-> " << byId(taken) << ", lost |-> {";
        bool first = true;
        for (std::uint64_t id : lost) {
            o << (first ? "" : ", ") << id;
            first = false;
        }
        o << "}";
        return o.str();
    }

    // The window receives the server's state in its format: false when its view and dialog are as they were.
    bool sync(Window& w) {
        const std::string before = viewOf(w) + dialogOf(w);
        sync::Sessions state = hub->state();
        state.setFormat(w.got.format());
        const std::string diff = state.diff_from(w.got);
        if (const std::string t = leaked(diff); !t.empty()) {
            w.leak = t;
        }
        w.got.apply_string(diff);
        if (w.old) {
            if (!w.got.requests().empty()) {
                fail("the old window received a request");
            }
            return viewOf(w) + dialogOf(w) != before;
        }
        w.prompt.received(w.got.requests());
        return viewOf(w) + dialogOf(w) != before;
    }

    // An answer or a cancel, as the window sends it: one action, encoded in the walk's format.
    void send(sync::Action action, const std::string& token) {
        clean(sync::describe(action), "an action's description");
        sync::Actions mine(++num);
        mine.setFormat(walkFormat);
        sync::Actions base(num);
        base.setFormat(walkFormat);
        const std::uint64_t id = action.request;
        const bool secret = action.kind == sync::ActionKind::SecretAnswer;
        mine.add(std::move(action));
        up.push_back(Flight{mine.diff_from(base), id, secret, token});
    }

    bool inFlight(std::uint64_t id, bool secret, const std::string& token) const {
        return std::any_of(up.begin(), up.end(), [&](const Flight& m) {
            return m.id == id && m.secret == secret && (!secret || m.token == token);
        });
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string faulty;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-store-late") == 0) {
            faulty = "store-late";
        } else if (std::strcmp(argv[i], "--faulty-echo") == 0) {
            faulty = "echo";
        } else if (std::strcmp(argv[i], "--faulty-driver") == 0) {
            faulty = "driver";
        } else if (std::strcmp(argv[i], "--faulty-driver-echo") == 0) {
            faulty = "driver-echo";
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            const unsigned long f = std::strtoul(argv[++i], nullptr, 10);
            if (f != sync::kSecretsFormat && f != sync::kTablesFormat && f != sync::kStateFormat) {
                std::fprintf(stderr, "secrets_walk: --format is %lu, not %u, %u or %u\n", f, sync::kSecretsFormat,
                    sync::kTablesFormat, sync::kStateFormat);
                return 2;
            }
            walkFormat = static_cast<std::uint32_t>(f);
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr,
            "usage: secrets_walk <seed> <steps> [--format 3|4|5] "
            "[--faulty-store-late | --faulty-echo | --faulty-driver | --faulty-driver-echo] <out.tla>\n");
        return 2;
    }
    rngState = std::strtoull(args[0].c_str(), nullptr, 10) * 0x9E3779B97F4A7C15ULL + 1;
    const long steps = std::strtol(args[1].c_str(), nullptr, 10);

    std::size_t faults = 0;
    Walk w(faulty, &faults);
    if (faulty == "echo") {
        w.echoFault = true;
        w.hub->injectSecretEchoFault();
    }
    w.take();
    std::vector<std::string> records;
    records.push_back("[op |-> \"init\", " + w.record() + "]");
    std::map<std::string, std::size_t> taken;

    for (long step = 0; step < steps; ++step) {
        ++w.now;
        std::string op;
        std::string fields;
        const std::size_t choice = pick(1000);
        Window& c = w.windows[pick(w.windows.size())];
        if (w.hub->reaskPending() && choice < 60 && w.nextId() <= kMaxRequests) {
            // nexus's own ask whose paste was refused malformed, asked again (Hub::reaskSecrets): an ask by nexus, or
            // none when one is open or held for the account by now.
            const std::uint64_t id = w.nextId();
            w.holdBatch();
            const std::optional<std::string> acct = w.hub->reaskSecrets();
            w.releaseBatch();
            if (!acct) {
                continue;
            }
            op = "ask";
            w.req[id] = Req{"open", *acct, "nexus"};
            fields = "asker |-> " + q("nexus") + ", acct |-> " + q(*acct);
        } else if (choice < 15) {
            if (w.nextId() > kMaxRequests) {
                continue;
            }
            const std::vector<sync::SessionId> askers{0, 1, 2};
            const sync::SessionId asker = askers[pick(askers.size())];
            // An AI's AI with an ask not yet answered (open, or its value held) is refused before any request is
            // made (Hub::askSecret, SecretDesk::waiting), which the model leaves out: the walk asks only from an
            // asker with none waiting.
            if (asker != 0 && w.hub->secrets().waiting(asker)) {
                continue;
            }
            op = "ask";
            // Each ask past the hub's limit for an asker (kSecretAsksPerWindow a minute) apart, so the walk never
            // meets it.
            w.now += sync::kSecretAskWindowMs / sync::kSecretAsksPerWindow + 1;
            const std::string acct = kAccts[pick(kAccts.size())];
            const std::uint64_t id = w.nextId();
            w.holdBatch();
            if (asker == 0) {
                std::string refused;
                if (!w.hub->askSecret(sync::SecretAsker{0, id, std::string()}, acct, kWhat, "the walk asks", &refused)) {
                    fail("an ask was refused: " + refused);
                }
            } else {
                const pid_t pid = w.drivers->pid(asker);
                if (pid <= 0) {
                    fail("session " + std::to_string(asker) + " has no running driver");
                }
                w.harness.emitAskSecret(pid, asker, id, acct, kWhat, "the walk asks");
                bool changed = false;
                if (!w.drivers->serve(static_cast<server::Millis>(w.now), &changed)) {
                    fail("Drivers::serve failed on ask-secret");
                }
                if (w.nextId() != id + 1) {
                    fail("driver ask-secret did not mint request " + std::to_string(id));
                }
            }
            w.releaseBatch();
            w.req[id] = Req{"open", acct, w.askerNames.at(asker)};
            fields = "asker |-> " + q(w.askerNames.at(asker)) + ", acct |-> " + q(acct);
        } else if (choice < 400) {
            if (!c.conn || !w.sync(c)) {
                continue;
            }
            op = "sync";
            fields = "c |-> " + q(c.name);
        } else if (choice < 520) {
            if (c.prompt.showing() == nullptr || !c.prompt.typed().empty()) {
                continue;
            }
            op = "type";
            // Now and then a paste not of the shape of a token (b1), which the server refuses and asks again.
            const std::size_t v = pick(5) == 0 ? 2 : pick(2);
            // A paste may bring a line break or spaces at its ends, which the dialog cuts at Enter: the field holds
            // what was pasted, and the model's dialog the value it carries.
            const char* pads[] = {"", " ", "\n", "\r\n"};
            c.prompt.type(kValues[v] + pads[pick(4)]);
            fields = "c |-> " + q(c.name) + ", v |-> " + val(kTokens[v]);
        } else if (choice < 640) {
            const sync::SecretRequest* r = c.prompt.showing();
            if (r == nullptr || c.prompt.typed().empty() ||
                    w.inFlight(r->id, true, tokenOf(c.prompt.typed().bytes()))) {
                continue;
            }
            op = "submit";
            const std::string token = tokenOf(c.prompt.typed().bytes());
            sync::Action a;
            a.kind = sync::ActionKind::SecretAnswer;
            if (!c.prompt.submit(&a.request, &a.secret)) {
                fail("a typed dialog did not submit");
            }
            if (tokenOf(a.secret.bytes()) != token) {
                fail("the value sent is not the value typed");
            }
            w.send(std::move(a), token);
            fields = "c |-> " + q(c.name);
        } else if (choice < 680) {
            const sync::SecretRequest* r = c.prompt.showing();
            if (r == nullptr || w.inFlight(r->id, false, "")) {
                continue;
            }
            op = "cancel";
            sync::Action a;
            a.kind = sync::ActionKind::SecretCancel;
            c.prompt.cancel(&a.request);
            w.send(std::move(a), kNone);
            fields = "c |-> " + q(c.name);
        } else if (choice < 690) {
            if (!c.conn) {
                continue;
            }
            op = "disconnect";
            c.conn = false;
            c.prompt.lost();
            c.got = sync::Sessions();
            c.got.setFormat(c.old ? 2 : walkFormat);
            fields = "c |-> " + q(c.name);
        } else if (choice < 700) {
            if (c.conn) {
                continue;
            }
            op = "connect";
            c.conn = true;
            fields = "c |-> " + q(c.name);
        } else if (choice < 930) {
            if (w.up.empty()) {
                continue;
            }
            op = "receive";
            const std::size_t i = pick(w.up.size());
            Flight m = w.up[i];
            w.up.erase(w.up.begin() + static_cast<std::ptrdiff_t>(i));
            const std::vector<sync::Action> actions = sync::Actions::parse(m.wire, walkFormat);
            sync::wipeBytes(m.wire);
            if (actions.size() != 1) {
                fail("an action in flight read back as " + std::to_string(actions.size()));
            }
            const std::optional<sync::SecretState> before = w.hub->secrets().stateOf(m.id);
            const bool wasOpen = before == sync::SecretState::Open;
            const std::size_t writes = w.keeper.writes;
            std::string why;
            w.holdBatch();
            w.hub->apply(actions[0], &why, "");
            w.releaseBatch();
            clean(why, "the hub's reason");
            // A value of the right shape the code took (its request open, or expired and still the newest of its
            // account, now stored or holding): the model's taken, and while holding its held.
            const std::optional<sync::SecretState> after = w.hub->secrets().stateOf(m.id);
            if (m.secret && m.token != "b1" && (wasOpen || before == sync::SecretState::Expired) &&
                    (after == sync::SecretState::Stored || after == sync::SecretState::Holding)) {
                w.taken[m.id] = m.token;
                if (after == sync::SecretState::Holding) {
                    w.heldTok[m.id] = m.token;
                }
            }
            if (faulty == "store-late" && !wasOpen && before != sync::SecretState::Expired && m.secret) {
                std::string ignored;
                w.keeper.keep(w.req[m.id].acct, kWhat, actions[0].secret, &ignored);
                ++faults;
            }
            w.stores[m.id] += w.keeper.writes - writes;
            fields = "m |-> " + Walk::flight(m);
        } else if (choice < 950) {
            const std::optional<std::uint64_t> due = w.hub->secretDeadline();
            if (!due) {
                continue;
            }
            op = "timeout";
            std::uint64_t oldest = 0;
            for (const sync::SecretRequest& r : w.hub->secrets().open()) {
                if (oldest == 0 || r.id < oldest) {
                    oldest = r.id;
                }
            }
            w.now = std::max(w.now, *due);
            w.holdBatch();
            w.hub->expireSecrets();
            w.releaseBatch();
            fields = "id |-> " + std::to_string(oldest);
        } else if (choice >= 956 && choice < 980) {
            // A held value written again, at its retry time: never past a request's deadline, so that the walk's
            // clock never expires two requests in one step.
            const std::optional<std::uint64_t> due = w.hub->secretRetryAt();
            const std::optional<std::uint64_t> deadline = w.hub->secretDeadline();
            if (!due || (deadline && *due > *deadline)) {
                continue;
            }
            op = "retry";
            w.now = std::max(w.now, *due);
            const std::size_t writes = w.keeper.writes;
            w.holdBatch();
            w.hub->retrySecrets();
            w.releaseBatch();
            if (w.events.log.size() != 1) {
                fail("a retry logged " + std::to_string(w.events.log.size()) + " lines");
            }
            const std::uint64_t id = w.events.log[0].id;
            w.stores[id] += w.keeper.writes - writes;
            fields = "id |-> " + std::to_string(id);
        } else if (choice >= 980 && choice < 990) {
            if (w.keeper.failing || w.failures >= kMaxFailures) {
                continue;
            }
            op = "diskfail";
            w.keeper.failing = true;
            ++w.failures;
        } else if (choice >= 990) {
            if (!w.keeper.failing) {
                continue;
            }
            op = "diskheal";
            w.keeper.failing = false;
        } else {
            // A restart now and then, after the first third, so that most requests run their course first.
            if (w.restarts >= kMaxRestarts || choice >= 956 || step < steps / 3) {
                continue;
            }
            op = "restart";
            for (const auto& entry : w.req) {
                if (w.hub->secrets().stateOf(entry.first) == sync::SecretState::Holding) {
                    w.lost.insert(entry.first);
                }
            }
            w.hub->dropSecrets();
            w.take();  // the dropped requests, while the old hub still reports them
            const std::vector<std::string> lines = w.log;
            w.stopServer();
            w.start();
            ++w.restarts;
            w.log = lines;
            records.push_back("[op |-> \"restart\", " + w.record() + "]");
            ++taken[op];
            continue;
        }
        const bool echo = faulty == "echo" && op == "receive" &&
            std::any_of(w.events.log.begin(), w.events.log.end(),
                        [](const sync::SecretLog& l) { return l.event.rfind("stored", 0) == 0; });
        if (echo) {
            ++faults;
        }
        w.take();
        records.push_back("[op |-> " + q(op) + (fields.empty() ? "" : ", " + fields) + ", " + w.record() + "]");
        ++taken[op];
    }

    if (faulty != "driver" && w.harness.totalAsks() > 0 && w.harness.totalAnswers() == 0) {
        fail("drivers sent ask-secret lines, yet received no secret lines on standard input");
    }

    std::ofstream out(args[2]);
    out << "---- MODULE SecretsTraceData ----\n"
           "\\* Written by tests/sync/secrets_walk: seed " << args[0] << ", " << steps << " steps"
        << (faulty.empty() ? std::string() : ", faulty " + faulty) << ".\n"
        << "EXTENDS TLC\n"
        << "CONSTANTS v1, v2, b1\n"
        << "TraceClients == {\"c1\", \"c2\", \"c3\"}\n"
        << "TraceOldClients == {\"c3\"}\n"
        << "TraceAccounts == {\"a1\", \"a2\"}\n"
        << "TraceAskers == {\"nexus\", \"s1\", \"s2\"}\n"
        << "TraceValues == {v1, v2, b1}\n"
        << "TraceBadValues == {b1}\n"
        << "TraceMaxRequests == " << kMaxRequests << "\n"
        << "TraceMaxRestarts == " << kMaxRestarts << "\n"
        << "TraceMaxFailures == " << kMaxFailures << "\n"
        << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    if (!out) {
        std::fprintf(stderr, "secrets_walk: could not write %s\n", args[2].c_str());
        return 1;
    }
    std::string coverage;
    for (const auto& [name, n] : taken) {
        coverage += " " + name + "=" + std::to_string(n);
    }
    std::printf("secrets_walk: %zu records, %zu driver asks, %zu driver stdin answers; taken:%s%s\n", records.size(),
                w.harness.totalAsks(), w.harness.totalAnswers(), coverage.c_str(),
                faulty.empty() ? "" : (", " + std::to_string(faults) + " faults played").c_str());
    if (!faulty.empty() && faults == 0) {
        std::fprintf(stderr, "secrets_walk: the faulty walk never hit its fault; try more steps\n");
        return 1;
    }
    return 0;
}
