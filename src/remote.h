// A terminal tab's label while its shell runs ssh: the fleet machine the session reaches, by its name in nova-config's
// machine record, or the host as ssh was given it. Pure: no OS. The foreground process is read in foreground.h.
//
// The signal is the terminal's own foreground process, not the title the program sets (OSC 0/2): while ssh is in the
// foreground the tab shows where it goes, whatever the remote shell, vim or htop set as the title, and when ssh ends the
// tab shows the label it had before the session. A nested ssh on the remote machine is not seen: the tab names the
// machine the local ssh reaches.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "friends.h"

namespace nexus {

// One fleet machine: its name and its record address (nova-config machine list).
struct Machine {
    std::string name;
    std::string address;
};

struct MachineList {
    std::vector<Machine> machines;
    std::string error;          // empty when `machines` came from the command; otherwise one line for stderr
};

// Reads `nova-config machine list --json`: {"result": {"status": "ok"}, "items": [{"fields": {"name", "address"}}]}.
MachineList parseMachineList(std::string_view json);

// The command that lists the machines: novaConfigCommand with `machine list --json`.
Command machineListCommand(const char* override, const char* shell);

// Runs `command` with `run` and parses what it prints. Every error names the command, what happened and what to do.
MachineList loadMachines(const Command& command, const CommandRunner& run);

// loadMachines with machineListCommand(the client config's nova-config, SHELL) and runCommand.
MachineList loadMachines();

// The destination of an ssh command line, `argv` as the process has it (argv[0] first), exactly as given: the first
// argument that is not an option or an option's value. Empty if argv[0] is not ssh (by its file name), if there is no
// destination, or if the options ask ssh only to print something (-G, -Q, -V) rather than to connect.
std::string sshDestination(const std::vector<std::string>& argv);

// The host of a destination: without `ssh://`, `user@` and `:port`.
std::string sshHost(std::string_view destination);

// The label for an ssh destination: the name of the fleet machine it reaches, or the host as given. A destination
// reaches a machine when its host, ignoring case and a trailing dot, is the machine's name, `<name>.<tailnet>.ts.net`
// for any one-label tailnet, or the machine's address. Empty for an empty destination.
std::string remoteLabel(std::string_view destination, const std::vector<Machine>& machines);

// How long after an ssh session ends the titles that arrive are still taken as the session's, in seconds. The process
// is seen gone before the terminal has read the last of its output, which may set a title. A guess, not measured: the
// tail is read within milliseconds, and a title the shell sets of its own in this time is the one it had before.
constexpr double kSessionTailSeconds = 0.5;

// The label of one terminal tab across ssh sessions (models/TabLabel.tla). `observe` is called once a frame, with the
// remote label of the terminal's foreground process (empty when it is not ssh), the title the program set now, and the
// frame's time in seconds; `label` is called whenever the tab is drawn, with the title then.
//
// While a session is observed the label is the session's remote label. When it ends, a title the remote side left
// behind is not the tab's: the label is the title from the last observation before the session, until the title
// changes after the session's tail.
class TabLabel {
public:
    void observe(const std::string& remote, const std::string& title, double time);
    std::string label(const std::string& title) const;

    // The state, for the model's trace check.
    const std::string& remote() const { return remote_; }
    const std::string& before() const { return before_; }
    const std::string& after() const { return after_; }
    bool masking() const { return masking_; }
    bool tail() const { return tail_; }

private:
    std::string remote_;        // the session's remote label; empty outside a session
    std::string before_;        // the title at the last observation outside a session that was the tab's own
    std::string after_;         // while `masking`, the title the session left
    bool masking_ = false;      // a session ended and the title is still one it left
    bool tail_ = false;         // while `masking`, the session's tail: titles that arrive are the session's
    double tailEnds_ = 0;       // when the tail ends
};

} // namespace nexus
