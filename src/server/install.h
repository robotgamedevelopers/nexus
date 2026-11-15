// Setting nexus up from a person's fork of it, and changing it from there: `nexus-server setup`, `doctor` and
// `rebuild` (SETUP.md). An AI runs these; none asks the person anything, needs an administrator or shows a window.
//
//   setup     places this build where it runs from (the server at ~/.nexus/bin/nexus-server, the Claude driver at
//             ~/.nexus/drivers/claude, on macOS the window at ~/Applications/nexus.app), writes what a first install
//             lacks (the drivers file's driver line, the window's `server local`), keeps the server running across
//             logout and reboot as a per-user service (a launchd agent on macOS, a systemd user unit on Linux), and
//             brings the running server to the placed build (server/takeover.h) with no session lost. Every step
//             checks what is there first, so a second run changes nothing.
//   doctor    says what is missing or wrong, one line each, with the command that fixes it.
//   rebuild   builds the fork (cmake, and the Claude driver with npm), runs the fast tests, and only if they pass runs
//             the new build's setup. A build or a test that fails changes nothing that runs.
//
// The fork and its build directory are the ones this program was built from (compiled in). Everything placed is under
// HOME; nothing is written outside it but the service's own registration (launchctl, systemctl --user).

#pragma once

#include <string>
#include <vector>

#include "server/server.h"

namespace nexus::server {

// Where upstream nexus is: the remote `upstream` of a person's fork.
inline const char* const kUpstreamUrl = "https://github.com/robotgamedevelopers/nexus.git";

// What is placed where, under HOME.
struct Placement {
    std::string home;           // HOME
    std::string top;            // ~/.nexus
    std::string bin;            // ~/.nexus/bin
    std::string server;         // ~/.nexus/bin/nexus-server
    std::string driver;         // ~/.nexus/drivers/claude
    std::string client;         // ~/.nexus/client
    std::string app;            // macOS: ~/Applications/nexus.app
    std::string localBin;       // ~/.local/bin/nexus-server: a link to the placed server, for a login shell over ssh
    std::string serviceName;    // the launchd label or systemd unit: one per HOME
    std::string serviceFile;    // its plist or unit file
};

// The placement under `home`. `userHome` is the user's home from the password database: a HOME other than it (a
// throwaway one, a second install) gets a service of its own, named for a hash of HOME.
Placement placement(const std::string& home, const std::string& userHome);

// The launchd agent's plist: `program serve --dir <dir>`, with HOME and PATH, started at login and again after any exit
// but a clean one (a takeover exits kTakeoverExit), its output to `log`.
std::string launchdPlist(const std::string& label, const std::string& program, const std::string& dir,
                         const std::string& log, const std::string& home, const std::string& path);
// The systemd user unit: the same, Restart=on-failure, wanted by default.target.
std::string systemdUnit(const std::string& program, const std::string& dir, const std::string& home,
                        const std::string& path);

// The window's config with `server local` added when it names no server; `text` unchanged when it does.
std::string withLocalServer(const std::string& text);

// A drivers file with the Claude driver: `node` running the placed driver's main, on the account `main`.
std::string firstDriversFile(const std::string& node, const std::string& driverMain);
// `text` with every path into the fork's own driver (`<source>/drivers/claude/dist/`) moved to the placed one
// (`<placed>/dist/`), so that no friend runs from the fork while it is being built; unchanged when there is none.
std::string movedDriverPaths(const std::string& text, const std::string& source, const std::string& placed);

// The help of `setup`, `doctor`, `rebuild` and `takeover` (`nexus-server help <topic>`, or the verb with --help): what
// it does, step by step, its options, what it prints and its exit statuses, for an AI reading it cold. Empty for any
// other topic. SETUP.md says the same at more length.
std::string verbHelp(const std::string& topic);

// `nexus-server setup [--no-service] [--wait <seconds>]`, `doctor`, and `rebuild [--no-tests]`: their exit statuses.
// `self` is this program's own path.
int setup(const Paths& p, const std::string& self, const std::vector<std::string>& args);
int doctor(const Paths& p, const std::string& self);
int rebuild(const Paths& p, const std::string& self, const std::vector<std::string>& args);

} // namespace nexus::server
