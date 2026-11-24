#include "foreground.h"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__)
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

int foregroundGroup(int shell) {
    kinfo_proc info{};
    if (shell <= 0 || !processInfo(shell, &info) || info.kp_eproc.e_tpgid <= 0) {
        return -1;
    }
    return info.kp_eproc.e_tpgid;
}

std::vector<std::string> foregroundArguments(int shell) {
    return processArguments(foregroundGroup(shell));
}

std::vector<std::string> processArguments(int leader) {
    if (leader <= 0) {
        return {};
    }
    int argmax = 0;
    size_t size = sizeof(argmax);
    int mibMax[2] = {CTL_KERN, KERN_ARGMAX};
    if (sysctl(mibMax, 2, &argmax, &size, nullptr, 0) != 0 || argmax <= 0)
        return {};
    // KERN_PROCARGS2: argc, the executable's path, padding, then argc arguments, each ended by a NUL, then the
    // environment.
    // Kept between calls, one a thread: it is as large as the system's argument limit.
    static thread_local std::vector<char> buffer;
    if (buffer.size() < static_cast<std::size_t>(argmax))
        buffer.resize(static_cast<std::size_t>(argmax));
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

std::vector<std::string> foregroundArguments(int) {
    return {};
}

int foregroundGroup(int) {
    return -1;
}

std::vector<std::string> processArguments(int) {
    return {};
}

#endif

} // namespace nexus
