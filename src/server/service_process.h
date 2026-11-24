#pragma once
#include "server/service_manifest.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace nexus::server {

// Each helper observes the server's lifetime socket and holds its service lease until its process group exits.
class ServiceProcesses {
public:
    ServiceProcesses(std::string self, std::function<std::string()> leasePath);
    ~ServiceProcesses();
    ServiceProcesses(const ServiceProcesses&) = delete;
    ServiceProcesses& operator=(const ServiceProcesses&) = delete;

    void configure(const std::vector<ManagedService>& services, std::uint64_t nowMs);
    void tick(std::uint64_t nowMs);
    int waitMs(std::uint64_t nowMs) const;
    std::string status() const;
    void stop();

private:
    struct Slot {
        std::optional<ManagedService> desired;
        pid_t helper = -1;
        int control = -1;
        std::uint64_t next = 0;
        std::uint64_t started = 0;
        std::uint64_t dueWall = 0;
        std::uint64_t claim = 0;
        unsigned failures = 0;
        bool running = false;
        bool stopping = false;
    };
    bool start(Slot& slot, std::uint64_t now);
    void schedule(Slot& slot, std::uint64_t now);
    static void end(Slot& slot);
    static void retry(Slot& slot, std::uint64_t nowMs);
    std::string self_;
    std::function<std::string()> leasePath_;
    std::map<std::string, Slot> slots_;
};

// Private subprocess entry point. Descriptor zero is the supervising server's lifetime socket.
int runServiceChild(int argc, char** argv);

} // namespace nexus::server
