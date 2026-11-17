// Unit tests of a terminal tab's label in ssh (src/remote.h): an ssh command line's destination, its match to a fleet
// machine, the machine list's parsing and loading through a fake command runner, and the label across sessions. Pure
// logic: no OS, no clock, no process, no network.

#include <cstdio>
#include <string>
#include <vector>

#include "remote.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

using Argv = std::vector<std::string>;

const std::vector<Machine> kFleet = {{"hetzner2", "100.64.0.2"}, {"space", "space.local"}, {"captain", ""}};

void findsTheDestination() {
    CHECK(sshDestination(Argv{"ssh", "hetzner2"}) == "hetzner2");
    CHECK(sshDestination(Argv{"ssh", "space"}) == "space");
    CHECK(sshDestination(Argv{"/usr/bin/ssh", "-p", "22", "nova@hetzner2.tailbd3740.ts.net"}) ==
        "nova@hetzner2.tailbd3740.ts.net");
    // -J's value is the jump host; the destination follows it.
    CHECK(sshDestination(Argv{"ssh", "-J", "a", "b"}) == "b");
    // Options with values, apart and attached, and flags grouped before a value.
    CHECK(sshDestination(Argv{"ssh", "-o", "StrictHostKeyChecking=no", "-i", "/k/id", "-l", "nova", "space"}) ==
        "space");
    CHECK(sshDestination(Argv{"ssh", "-p2222", "-oBatchMode=yes", "space"}) == "space");
    CHECK(sshDestination(Argv{"ssh", "-tt", "-4vp", "22", "space", "ls", "-l"}) == "space");
    CHECK(sshDestination(Argv{"ssh", "-L", "8080:localhost:80", "-N", "space"}) == "space");
    CHECK(sshDestination(Argv{"ssh", "--", "space"}) == "space");
    // No host: no destination.
    CHECK(sshDestination(Argv{"ssh"}).empty());
    CHECK(sshDestination(Argv{"ssh", "-p", "22"}).empty());
    CHECK(sshDestination(Argv{"ssh", "--"}).empty());
    // Options that only print.
    CHECK(sshDestination(Argv{"ssh", "-G", "space"}).empty());
    CHECK(sshDestination(Argv{"ssh", "-V"}).empty());
    CHECK(sshDestination(Argv{"ssh", "-Q", "cipher"}).empty());
    // Not ssh.
    CHECK(sshDestination(Argv{"-zsh"}).empty());
    CHECK(sshDestination(Argv{"vim", "space"}).empty());
    CHECK(sshDestination(Argv{"sshd", "space"}).empty());
    CHECK(sshDestination(Argv{}).empty());
}

void findsTheHost() {
    CHECK(sshHost("space") == "space");
    CHECK(sshHost("nova@space") == "space");
    CHECK(sshHost("a@b@space") == "space");
    CHECK(sshHost("ssh://nova@space:2222") == "space");
    CHECK(sshHost("ssh://[fe80::1]:22") == "fe80::1");
    CHECK(sshHost("fe80::1") == "fe80::1");
    CHECK(sshHost("").empty());
}

void matchesAFleetMachine() {
    CHECK(remoteLabel("hetzner2", kFleet) == "hetzner2");
    CHECK(remoteLabel("nova@hetzner2.tailbd3740.ts.net", kFleet) == "hetzner2");
    CHECK(remoteLabel("HETZNER2.tailbd3740.ts.net.", kFleet) == "hetzner2");
    CHECK(remoteLabel("100.64.0.2", kFleet) == "hetzner2");
    CHECK(remoteLabel("glenn@space.local", kFleet) == "space");
    CHECK(remoteLabel("ssh://nova@space:22", kFleet) == "space");
    CHECK(remoteLabel("captain", kFleet) == "captain");
    // An unknown host is shown as given, without its user.
    CHECK(remoteLabel("glenn@example.com", kFleet) == "example.com");
    CHECK(remoteLabel("hetzner3", kFleet) == "hetzner3");
    // Not a machine's tailnet name: two labels between, or none.
    CHECK(remoteLabel("hetzner2.a.b.ts.net", kFleet) == "hetzner2.a.b.ts.net");
    CHECK(remoteLabel("hetzner2..ts.net", kFleet) == "hetzner2..ts.net");
    CHECK(remoteLabel("hetzner2.ts.net", kFleet) == "hetzner2.ts.net");
    // A machine with no address matches no empty host.
    CHECK(remoteLabel("", kFleet).empty());
    CHECK(remoteLabel("nova@", kFleet).empty());
    // The whole chain, as the terminal runs it.
    CHECK(remoteLabel(sshDestination(Argv{"ssh", "space"}), kFleet) == "space");
    CHECK(remoteLabel(sshDestination(Argv{"ssh", "-p", "22", "nova@hetzner2.tailbd3740.ts.net"}), kFleet) ==
        "hetzner2");
    CHECK(remoteLabel(sshDestination(Argv{"-zsh"}), kFleet).empty());
}

void parsesTheMachineList() {
    MachineList list = parseMachineList(R"({"result":{"verb":"machine list","status":"ok","exit":0},
        "facts":{"kind":"machine","rows":3},"items":[
        {"kind":"machine","fields":{"name":"space","address":"100.64.0.9","user":"nova","cores":8}},
        {"kind":"machine","fields":{"name":"","address":"x"}},
        {"kind":"machine","fields":{"name":"bare"}}]})");
    CHECK(list.error.empty());
    CHECK(list.machines.size() == 2);
    CHECK(list.machines.size() == 2 && list.machines[0].name == "space" && list.machines[0].address == "100.64.0.9");
    CHECK(list.machines.size() == 2 && list.machines[1].name == "bare" && list.machines[1].address.empty());
    CHECK(!parseMachineList("not json").error.empty());
    CHECK(!parseMachineList(R"({"result":{"status":"refused"},"items":[]})").error.empty());
    CHECK(!parseMachineList(R"({"result":{"status":"ok"}})").error.empty());
    CHECK(parseMachineList(R"({"result":{"status":"ok"},"items":[]})").error.empty());
}

void loadsTheMachineList() {
    const Command command = machineListCommand("/opt/nova-config", nullptr);
    CHECK(command.program == "/opt/nova-config");
    CHECK((command.arguments == std::vector<std::string>{"machine", "list", "--json"}));
    int timeout = 0;
    MachineList ok = loadMachines(command, [&](const Command&, int seconds) {
        timeout = seconds;
        CommandResult r;
        r.end = CommandResult::End::Exited;
        r.out = R"({"result":{"status":"ok"},"items":[{"fields":{"name":"space","address":""}}]})";
        return r;
    });
    CHECK(timeout == kMachineListTimeoutSeconds);
    CHECK(ok.error.empty() && ok.machines.size() == 1);
    MachineList failed = loadMachines(command, [](const Command&, int) {
        CommandResult r;
        r.end = CommandResult::End::Exited;
        r.code = 2;
        r.err = "no store: set NOVA_PG_DSN\n";
        return r;
    });
    CHECK(failed.machines.empty());
    CHECK(failed.error.find("machine list --json") != std::string::npos);
    CHECK(failed.error.find("no store: set NOVA_PG_DSN") != std::string::npos);
    CHECK(failed.error.find("host as typed") != std::string::npos);
}

// Observations a frame apart (1/60 s), from a clock that starts at 1000.
struct Frames {
    TabLabel l;
    double time = 1000;
    void observe(const std::string& remote, const std::string& title) {
        l.observe(remote, title, time);
        time += 1.0 / 60;
    }
    void wait(double seconds) { time += seconds; }
};

// The owner's case: a new terminal, `ssh space`, the tab reads "space" while the session runs whatever the remote side
// sets as the title, and its usual label after exit.
void theLabelFollowsTheSession() {
    Frames f;
    f.observe("", "sh");
    CHECK(f.l.label("sh") == "sh");
    f.observe("space", "sh");
    CHECK(f.l.label("sh") == "space");
    // The remote shell, vim or htop set titles: the tab still names the machine.
    CHECK(f.l.label("nova@space: ~") == "space");
    f.observe("space", "vim notes.txt");
    CHECK(f.l.label("htop") == "space");
    // The session ends with the remote side's title still set: the tab shows its label from before.
    f.observe("", "nova@space: ~");
    CHECK(f.l.label("nova@space: ~") == "sh");
    // In the session's tail, a last title from the remote side arrives: still the label from before, whatever it is.
    CHECK(f.l.label("nova@space: bye") == "sh");
    f.observe("", "nova@space: bye");
    CHECK(f.l.label("nova@space: bye") == "sh");
    f.wait(kSessionTailSeconds);
    f.observe("", "nova@space: bye");
    CHECK(f.l.label("nova@space: bye") == "sh");
    CHECK(f.l.masking() && !f.l.tail());
    // A new title, set after the tail, is the tab's again.
    f.observe("", "make");
    CHECK(f.l.label("make") == "make");
    CHECK(!f.l.masking());
    f.observe("", "make");
    CHECK(f.l.before() == "make");
}

void aSessionThatLeavesTheTitleAloneChangesNothing() {
    Frames f;
    f.observe("", "zsh");
    f.observe("hetzner2", "zsh");
    f.observe("", "zsh");
    CHECK(f.l.label("zsh") == "zsh");
    f.wait(kSessionTailSeconds);
    f.observe("", "zsh");
    CHECK(f.l.label("zsh") == "zsh");
    f.observe("", "other");
    CHECK(f.l.label("other") == "other");
    CHECK(!f.l.masking());
}

void aSecondSessionKeepsTheFirstLabel() {
    Frames f;
    f.observe("", "sh");
    f.observe("space", "sh");
    f.observe("", "nova@space: ~");
    f.wait(kSessionTailSeconds);
    f.observe("", "nova@space: ~");
    // Back in, before anything set a title: the label to return to is still "sh", not the remote's.
    f.observe("hetzner2", "nova@space: ~");
    CHECK(f.l.label("x") == "hetzner2");
    f.observe("", "root@hetzner2");
    CHECK(f.l.label("root@hetzner2") == "sh");
    // Straight back in, within the first session's tail.
    f.observe("space", "root@hetzner2");
    // One session straight into another, between two observations: the label is the new one.
    f.observe("captain", "root@hetzner2");
    CHECK(f.l.label("y") == "captain");
    f.observe("", "y");
    CHECK(f.l.label("y") == "sh");
}

// ssh set its title before the first frame that saw it run: the label to return to is the one from before.
void aTitleSetBeforeTheSessionIsSeenIsNotTheTabs() {
    Frames f;
    f.observe("", "sh");
    f.observe("space", "nova@remote: ~");
    CHECK(f.l.label("nova@remote: ~") == "space");
    f.observe("", "nova@remote: left");
    CHECK(f.l.label("nova@remote: left") == "sh");
    // A session seen at the very first observation has no label of the tab's to return to.
    Frames first;
    first.observe("space", "nova@remote: ~");
    first.observe("", "nova@remote: left");
    CHECK(first.l.label("nova@remote: left").empty());
}

} // namespace

int main() {
    findsTheDestination();
    findsTheHost();
    matchesAFleetMachine();
    parsesTheMachineList();
    loadsTheMachineList();
    theLabelFollowsTheSession();
    aSessionThatLeavesTheTitleAloneChangesNothing();
    aSecondSessionKeepsTheFirstLabel();
    aTitleSetBeforeTheSessionIsSeenIsNotTheTabs();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("remote label unit tests passed\n");
    return 0;
}
