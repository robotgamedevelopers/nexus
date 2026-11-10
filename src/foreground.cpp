#include "foreground.h"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace nexus {

#if defined(__APPLE__)

namespace {

bool processInfo(int pid, kinfo_proc* out) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t size = sizeof(*out);
    std::memset(out, 0, sizeof(*out));
    return sysctl(mib, 4, out, &size, nullptr, 0) == 0 && size == sizeof(*out);
}

} // namespace

std::vector<int> childProcesses() {
    std::vector<int> pids(256);
    for (;;) {
        const int n = proc_listchildpids(getpid(), pids.data(), int(pids.size() * sizeof(int)));
        if (n < 0)
            return {};
        if (std::size_t(n) < pids.size()) {
            pids.resize(std::size_t(n));
            return pids;
        }
        pids.resize(pids.size() * 2);
    }
}

int startedShell(const std::vector<int>& before, const std::vector<int>& after) {
    int found = -1;
    for (int pid : after) {
        if (pid <= 0 || std::find(before.begin(), before.end(), pid) != before.end())
            continue;
        kinfo_proc info{};
        if (!processInfo(pid, &info) || info.kp_eproc.e_tdev == NODEV || info.kp_eproc.e_pgid != pid)
            continue;
        if (found != -1)
            return -1;
        found = pid;
    }
    return found;
}

std::vector<std::string> foregroundArguments(int shell) {
    kinfo_proc info{};
    if (shell <= 0 || !processInfo(shell, &info) || info.kp_eproc.e_tpgid <= 0)
        return {};
    const int leader = info.kp_eproc.e_tpgid;
    int argmax = 0;
    size_t size = sizeof(argmax);
    int mibMax[2] = {CTL_KERN, KERN_ARGMAX};
    if (sysctl(mibMax, 2, &argmax, &size, nullptr, 0) != 0 || argmax <= 0)
        return {};
    // KERN_PROCARGS2: argc, the executable's path, padding, then argc arguments, each ended by a NUL, then the
    // environment.
    std::vector<char> buffer(static_cast<std::size_t>(argmax));
    size = buffer.size();
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, leader};
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
        return {};
    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof(argc));
    const char* p = buffer.data() + sizeof(argc);
    const char* end = buffer.data() + size;
    while (p < end && *p != 0)
        ++p;                    // the executable's path
    while (p < end && *p == 0)
        ++p;                    // its padding
    std::vector<std::string> argv;
    for (int i = 0; i < argc && p < end; ++i) {
        const char* s = p;
        while (p < end && *p != 0)
            ++p;
        argv.emplace_back(s, std::size_t(p - s));
        ++p;
    }
    return argv;
}

#else

std::vector<int> childProcesses() {
    return {};
}

int startedShell(const std::vector<int>&, const std::vector<int>&) {
    return -1;
}

std::vector<std::string> foregroundArguments(int) {
    return {};
}

#endif

} // namespace nexus
