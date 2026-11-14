// No core file of nexus holds a secret (docs/secrets.md): the window holds a value while its dialog is open and its
// action is in flight, and nexus-server while it writes the value's file. Called first thing in each main: a crash then
// writes no core (RLIMIT_CORE 0, soft and hard, so no child of the process raises it again), and on Linux the process
// is not dumpable (PR_SET_DUMPABLE 0), which also keeps other processes of the same user from reading its memory
// through ptrace or /proc. Windows has no core files; its crash reports are the system's.

#pragma once

#if !defined(_WIN32)
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace nexus {

// False when the system refused (the caller says so and goes on: a refusal leaves the process as it was).
inline bool noCoreDumps() {
#if defined(_WIN32)
    return true;
#else
    bool ok = true;
    struct rlimit none{};
    if (::setrlimit(RLIMIT_CORE, &none) != 0) {
        ok = false;
    }
#if defined(__linux__)
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        ok = false;
    }
#endif
    return ok;
#endif
}

} // namespace nexus
