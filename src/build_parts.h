// What this build of the client holds, with no other header needed: iOS and iPadOS (platform_ios.mm, cmake/ios.cmake)
// start no processes, as the system gives an app none. So there the terminal tabs (a local shell), the sprint board's
// own lookup (running nova-config), an attachment's upload and the link's ssh (child processes) are left out of the
// build, by design, rather than built to fail: the window has no Command-T (View::terminals unset), opens the sprint
// board at the address the server sends (board.h offer), says so when a file is attached, and reaches nexus-server
// through its in-app ssh (link/inapp_ssh.h).

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#define NEXUS_PROCESSES 0
#define NEXUS_MACOS 0
#elif defined(__APPLE__)
#define NEXUS_PROCESSES 1
#define NEXUS_MACOS 1
#else
#define NEXUS_PROCESSES 1
#define NEXUS_MACOS 0
#endif
