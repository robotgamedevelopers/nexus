// The parent, command launcher and fixture are separate processes. Every path belongs
// to the fixture, including the lock that detects two running copies of a service.
#include "server/service_process.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nexus::server::ManagedService;
using nexus::server::ServiceProcesses;

namespace {
constexpr char argument[] = "two words; $HOME * 'quoted' $(false)";

void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

class Temp {
public:
    Temp() {
        char pattern[] = "/tmp/nexus-service-test-XXXXXX";
        require(::mkdtemp(pattern), "could not make temporary directory");
        path = fs::canonical(pattern);
    }
    std::function<std::string()> leasePath() const {
        return [this] { return (path / "service-leases").string(); };
    }
    ~Temp() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

std::string contents(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}

int starts(const fs::path& path) {
    const auto records = contents(path / "starts");
    return static_cast<int>(std::count(records.begin(), records.end(), '\n'));
}

bool fixtureStopped(const fs::path& path) {
    const int fd = ::open((path / "active").c_str(), O_RDWR);
    if (fd < 0) return false;
    const bool stopped = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    ::close(fd);
    return stopped;
}

template<class Predicate>
bool eventually(Predicate satisfied, std::chrono::milliseconds budget = 4000ms) {
    const auto until = std::chrono::steady_clock::now() + budget;
    do {
        if (satisfied()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < until);
    return false;
}

// A second flock, acquired by the fixture itself, is independent of the server's
// ownership lock. An overlapping start leaves a permanent witness, even if it exits.
int fixture(const fs::path& dir, const std::string& mode, const char* arg) {
    const int guard = ::open((dir / "active").c_str(), O_CREAT | O_RDWR, 0600);
    if (guard < 0) return 70;
    if (::flock(guard, LOCK_EX | LOCK_NB) < 0) {
        std::ofstream(dir / "overlap") << "started while earlier group still held the lock\n";
        return 71;
    }
    if (!std::getenv("NEXUS_FIXTURE_VALUE") || std::string(std::getenv("NEXUS_FIXTURE_VALUE")) != argument ||
        std::getenv("NEXUS_TEST_PARENT_ONLY") || arg != std::string(argument) || fs::current_path() != dir) {
        std::ofstream(dir / "bad-launch") << "wrong environment, arguments, or working directory\n";
        return 72;
    }
    std::cout << "fixture stdout\n" << std::flush;
    std::cerr << "fixture stderr\n";
    if (mode == "descendant") {
        // The leader exits on TERM. Its child ignores TERM and keeps the witness
        // lock, so reaping the leader alone cannot make the replacement safe.
        const pid_t child = ::fork();
        if (child < 0) return 73;
        if (child == 0) {
            ::signal(SIGTERM, SIG_IGN);
            std::ofstream(dir / "descendant-ready") << ::getpid() << '\n';
            // A bounded lifetime even if the implementation under test breaks.
            ::sleep(15);
            ::_exit(0);
        }
        if (!eventually([&] { return fs::exists(dir / "descendant-ready"); })) return 74;
    }
    std::ofstream(dir / "starts", std::ios::app) << mode << '\n';
    if (mode == "quick") return 19;
    eventually([&] { return fs::exists(dir / "release"); }, 15000ms);
    return 0;
}

ManagedService spec(const std::string& self, const fs::path& dir, const std::string& mode) {
    ManagedService service;
    service.name = "fixture";
    service.argv = {self, "fixture", dir.string(), mode, argument};
    service.env = {{"HOME", dir.string()}, {"PATH", "/usr/bin:/bin"}, {"NEXUS_FIXTURE_VALUE", argument}};
    service.cwd = dir.string();
    service.log = (dir / "service.log").string();
    return service;
}

void pump(ServiceProcesses& processes, std::uint64_t now) {
    processes.tick(now);
    std::this_thread::sleep_for(10ms);
}

void keepAlive(const std::string& self) {
    Temp temp;
    ServiceProcesses services(self, temp.leasePath());
    std::uint64_t now = 10000;
    services.configure({spec(self, temp.path, "quick")}, now);
    require(eventually([&] { services.tick(now); return starts(temp.path) == 1; }), "keepalive first start");
    for (int i = 0; i < 15; ++i) pump(services, now);
    require(starts(temp.path) == 1, "restarted repeatedly without advancing clock");
    require(eventually([&] { now += 1000; services.tick(now); return starts(temp.path) >= 2; }),
            "failed keepalive did not retry");
    services.configure({spec(self, temp.path, "holding")}, now);
    require(eventually([&] {
        now += 100; services.tick(now); return contents(temp.path / "starts").find("holding") != std::string::npos;
    }), "changed argv did not replace old service");
    const auto log = contents(temp.path / "service.log");
    require(log.find("fixture stdout") != std::string::npos && log.find("fixture stderr") != std::string::npos,
            "missing child stdout or stderr");
    services.configure({}, now);
    require(eventually([&] { services.tick(now += 10000); return fixtureStopped(temp.path); }),
            "removing the assignment left its child running");
    const int count = starts(temp.path);
    for (int i = 0; i < 4; ++i) pump(services, now += 100000);
    require(starts(temp.path) == count && !fs::exists(temp.path / "overlap"), "removed service restarted or overlapped");
    services.stop();
}

void scheduled(const std::string& self, const char* tracePath = nullptr) {
    Temp temp;
    auto service = spec(self, temp.path, "scheduled");
    service.everySeconds = 5;
    ServiceProcesses services(self, temp.leasePath());
    std::ofstream trace;
    if (tracePath) {
        trace.open(tracePath);
        require(trace.good(), "could not open trace file");
        trace << "---- MODULE ManagedServiceTraceData ----\nTrace == <<\n";
    }
    auto sample = [&](const char* action) {
        if (!tracePath) return;
        const int count = starts(temp.path);
        const bool live = fs::exists(temp.path / "active") && !fixtureStopped(temp.path);
        const int groups = fs::exists(temp.path / "overlap") ? 2 : static_cast<int>(live);
        trace << (std::string(action) == "init" ? "" : ",\n")
              << "[op |-> \"" << action << "\", starts |-> " << count << ", groups |-> " << groups << "]";
    };
    sample("init");
    services.configure({service}, 0);
    sample("timer");
    for (int i = 0; i < 8; ++i) pump(services, 4999);
    sample("observe");
    require(starts(temp.path) == 0, "periodic service ran before first interval");
    sample("due");
    require(eventually([&] { services.tick(5000); return starts(temp.path) == 1; }), "periodic first run");
    sample("launch");
    for (int i = 1; i <= 12; ++i) pump(services, 5000 + i * 5000);
    sample("observe");
    require(starts(temp.path) == 1 && !fs::exists(temp.path / "overlap"), "periodic service overlapped");
    std::ofstream(temp.path / "release") << "done\n";
    require(eventually([&] {
        services.tick(65000);
        return fixtureStopped(temp.path) && services.status().find(" waiting\n") != std::string::npos;
    }), "supervisor did not collect the completed service");
    sample("exit");
    fs::remove(temp.path / "release");
    for (int i = 0; i < 8; ++i) pump(services, 69999);
    sample("observe");
    require(starts(temp.path) == 1, "periodic service restarted before the completion interval");
    sample("due");
    require(eventually([&] { services.tick(70000); return starts(temp.path) >= 2; }),
            "periodic service did not run again after completion");
    sample("launch");
    sample("off");
    services.stop();
    sample("exit");
    if (tracePath) {
        trace << "\n>>\n====\n";
        trace.close();
        require(!trace.fail(), "could not write trace file");
    }
}

void scheduledHandoff(const std::string& self) {
    Temp temp;
    auto service = spec(self, temp.path, "scheduled");
    service.everySeconds = 1;
    {
        ServiceProcesses first(self, temp.leasePath());
        first.configure({service}, 0);
        require(first.waitMs(0) > 0, "new schedule had no initial deadline");
        // The first due record is installed with link(), then its temporary
        // name is unlinked. Restore both links left by a crash between them.
        const auto due = temp.path / "service-leases" / "fixture.due";
        fs::create_hard_link(due, due.string() + ".32767");
    }
    // No server was running when the first interval expired. A fresh manager
    // must see that deadline immediately, even with its own new monotonic clock.
    std::this_thread::sleep_for(1100ms);
    constexpr std::uint64_t now = 10000;
    ServiceProcesses stale(self, temp.leasePath());
    stale.configure({service}, now);
    require(stale.waitMs(now) == 0, "handoff reset the first scheduled deadline");
    {
        ServiceProcesses active(self, temp.leasePath());
        active.configure({service}, now);
        require(eventually([&] { active.tick(now); return starts(temp.path) == 1; }),
                "restored deadline never launched its service");
        active.stop();
    }
    // Both managers had read the same due period. The first already dispatched
    // and stopped; its process group and flock have gone. The durable claim
    // alone must keep a delayed second manager from running this interval again.
    stale.tick(now);
    require(stale.status().find(" starting\n") != std::string::npos, "second manager did not attempt the stale claim");
    require(eventually([&] {
        stale.tick(now);
        return stale.status().find(" waiting\n") != std::string::npos;
    }), "second manager failed to defer its stale claim");
    require(starts(temp.path) == 1 && !fs::exists(temp.path / "overlap"), "claimed interval ran twice");
}

class OwnedChild {
public:
    explicit OwnedChild(pid_t child) : child_(child) {}
    ~OwnedChild() { stop(); }
    void stop() {
        if (child_ <= 0) return;
        ::kill(child_, SIGKILL);
        while (::waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
        child_ = -1;
    }
private:
    pid_t child_;
};

void abruptExit(const std::string& self) {
    Temp temp;
    const pid_t child = ::fork();
    require(child >= 0, "could not fork fixture owner");
    if (child == 0) {
        ServiceProcesses owner(self, temp.leasePath());
        owner.configure({spec(self, temp.path, "descendant")}, 10000);
        for (;;) pump(owner, 10000);
    }
    OwnedChild owner(child);
    require(eventually([&] { return starts(temp.path) == 1; }), "old service failed to start");
    owner.stop();
    ServiceProcesses replacement(self, temp.leasePath());
    std::uint64_t now = 30000;
    replacement.configure({spec(self, temp.path, "holding")}, now);
    require(eventually([&] {
        // Speed up restart backoff. The orphan still has to be killed and reaped on the actual clock.
        now += 500;
        replacement.tick(now);
        return contents(temp.path / "starts").find("holding") != std::string::npos;
    }, 8000ms), "replacement failed to take over after owner crash");
    require(!fs::exists(temp.path / "overlap"), "replacement overlapped with the previous service's descendant");
    replacement.stop();
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "service-child") {
        if (std::getenv("NEXUS_FIXTURE_VALUE")) return 74;  // service settings apply only to its own executable
        return nexus::server::runServiceChild(argc, argv);
    }
    if (argc == 5 && std::string(argv[1]) == "fixture") return fixture(argv[2], argv[3], argv[4]);
    try {
        const bool trace = argc == 3 && std::string(argv[1]) == "--trace";
        require(argc == 1 || trace, "usage: service_process_functional [--trace FILE]");
        require(::setenv("NEXUS_TEST_PARENT_ONLY", "private-to-parent", 1) == 0, "could not set parent fixture environment");
        const auto self = fs::canonical(argv[0]).string();
        if (trace) {
            scheduled(self, argv[2]);
            return 0;
        }
        keepAlive(self);
        scheduled(self);
        scheduledHandoff(self);
        abruptExit(self);
    } catch (const std::exception& error) {
        std::cerr << "service_process_functional: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
