// nexus-mock: a real nexus-server with fake AIs and fake chats, for driving the clients (docs/mock.md).
//
//   nexus-mock start [--dir DIR] [--ais N | --names A,B,...] [--sign-in] [--instant]
//       makes the mock afresh in DIR (/tmp/nexus-mock unless given; an earlier mock there is stopped and removed, and
//       a DIR that is not a mock's is refused), starts its server, seeds each AI's conversation through the real link,
//       and prints the command a client runs to reach it. --ais takes how many AIs (default 5; 0 is the state with no
//       AIs; the presets are 1, 5, 12 and 30), --names the names themselves. --sign-in adds Ida, whose account has no
//       login, to show the sign-in screen; no other AI has an account. --instant writes the replies to new prompts at
//       once rather than a word at a time, for tests.
//   nexus-mock stop [--dir DIR]
//       stops the mock's server and removes DIR
//   nexus-mock command [--dir DIR]
//       prints the command a client runs to reach the mock (its --server-command)
//   nexus-mock state --out FILE [--dir DIR]
//       connects as a client and writes the server's state to FILE, which `nexus --screenshot PNG --state FILE`
//       draws as the window shows it (a screenshot of the mock in the window with no display or ssh needed)
//
// Everything is under DIR: the server's HOME (DIR/home) and --dir (DIR/server), the AIs' scripts (DIR/scripts) and
// working directories (DIR/work), a HOME for a window run against the mock (DIR/client), and the client command
// (DIR/server-command). Nothing outside DIR is read or written: no account, credential, sign-in or real AI.

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "command.h"
#include "mock.h"
#include "mock_link.h"
#include "sync/actions.h"
#include "sync/sessions.h"

#ifndef NEXUS_MOCK_SERVER
#error "NEXUS_MOCK_SERVER names the nexus-server the mock runs (tests/mock/CMakeLists.txt)"
#endif

namespace {

using namespace nexus;
namespace fs = std::filesystem;

const char* const kMarker = "nexus-mock";
constexpr int kStallSeconds = 60;

struct Paths {
    std::string dir, home, server, scripts, work, client, bin, command;
    explicit Paths(const std::string& d)
        : dir(d), home(d + "/home"), server(d + "/server"), scripts(d + "/scripts"), work(d + "/work"),
          client(d + "/client"), bin(d + "/bin"), command(d + "/server-command") {}
};

int fail(const std::string& why) {
    std::fprintf(stderr, "nexus-mock: %s\n", why.c_str());
    return 1;
}

bool writeFile(const std::string& path, const std::string& text, std::string* why) {
    std::ofstream out(path, std::ios::trunc);
    out << text;
    out.close();
    if (!out) {
        *why = "cannot write " + path;
        return false;
    }
    return true;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool makeDir(const std::string& path, std::string* why) {
    if (::mkdir(path.c_str(), 0700) != 0) {
        *why = "cannot make " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool isMock(const Paths& p) {
    return readFile(p.dir + "/" + kMarker).rfind(kMarker, 0) == 0;
}

// Stops the server in the mock at `p`, if one answers there. False, with why, when one answered and did not stop.
bool stopServer(const Paths& p, std::string* why) {
    if (::access((p.server + "/server.sock").c_str(), F_OK) != 0)
        return true;
    const Command c{"/usr/bin/env", {"HOME=" + p.home, NEXUS_MOCK_SERVER, "stop", "--dir", p.server},
        "nexus-server stop --dir " + p.server};
    const CommandResult r = runCommand(c, 30);
    if (r.end == CommandResult::End::Exited && (r.code == 0 || r.err.find("no server answers") != std::string::npos))
        return true;
    *why = "the mock's server in " + p.server + " did not stop: " + r.err;
    return false;
}

// Stops and removes the mock at `p`; nothing there is nothing to do. A directory that is not a mock's is refused.
bool removeMock(const Paths& p, std::string* why) {
    std::error_code ec;
    if (!fs::exists(p.dir, ec))
        return true;
    if (!isMock(p)) {
        *why = p.dir + " is not a mock's directory (it has no " + kMarker + " file), so it is left as it is; give "
            "--dir a directory of its own";
        return false;
    }
    if (!stopServer(p, why))
        return false;
    fs::remove_all(p.dir, ec);
    if (ec) {
        *why = "cannot remove " + p.dir + ": " + ec.message();
        return false;
    }
    return true;
}

// The files the server starts from: the AIs' scripts and working directories, its drivers file, config, accounts and
// AIs, and the client command.
bool writeMock(const Paths& p, const mock::Options& o, std::string* serverCommand, std::string* why) {
    if (!makeDir(p.dir, why) || !writeFile(p.dir + "/" + kMarker, std::string(kMarker) + " 1\n", why))
        return false;
    for (const std::string* d : {&p.home, &p.server, &p.scripts, &p.work, &p.client, &p.bin})
        if (!makeDir(*d, why))
            return false;
    if (!makeDir(p.server + "/nexus-config", why))
        return false;
    std::vector<std::string> all = o.ais;
    if (o.signIn)
        all.push_back(mock::kSignInName);
    for (const std::string& name : all) {
        if (!makeDir(p.work + "/" + name, why) ||
                !writeFile(p.scripts + "/" + name + ".script", mock::scriptFor(name, o.instant), why))
            return false;
    }
    // The FAKE claude in-app sign-in runs (its name, claude-<mode>, says what it does: tests/server/fake_claude.cpp):
    // `silent` hands its made-up URL to nobody, so a sign-in started from a client waits, then ends; it never reaches a
    // real login, page or network.
    const std::string claude = p.bin + "/claude-silent";
    std::error_code ec;
    fs::create_symlink(NEXUS_MOCK_FAKE_CLAUDE, claude, ec);
    if (ec) {
        *why = "cannot link " + claude + ": " + ec.message();
        return false;
    }
    const accounts::Registry registry = mock::registryFor(o, p.work);
    if (!writeFile(p.server + "/drivers", mock::driversFor(o, NEXUS_MOCK_FAKE_DRIVER, p.scripts, p.work), why) ||
            !writeFile(p.server + "/config", mock::serverConfigFor(o, claude), why) ||
            !writeFile(p.server + "/nexus-config/accounts.json", accounts::accountsText(registry), why) ||
            !writeFile(p.server + "/nexus-config/friends.json", accounts::friendsText(registry), why))
        return false;
    return mock::serverCommandFor(NEXUS_MOCK_SERVER, p.home, p.server, serverCommand, why) &&
        writeFile(p.command, *serverCommand + "\n", why);
}

// The session of AI `name` in the server's state, or null.
const sync::SessionView* sessionOf(const mock::Client& c, const std::string& name) {
    return c.session->state().first(name);
}

bool busy(const sync::SessionView& v) {
    return v.activity == sync::Activity::Busy || v.activity == sync::Activity::Working;
}

// Plays each AI's seeded conversation (mock::seedFor) through the real link and server, every AI at once, each turn
// after the one before it has ended.
bool seed(mock::Client& c, const mock::Options& o, std::string* why) {
    for (const std::string& name : o.ais) {
        sync::Action a;
        a.kind = sync::ActionKind::Ensure;
        a.friendName = name;
        c.session->add(a);
    }
    if (o.signIn) {
        sync::Action a;
        a.kind = sync::ActionKind::Ensure;
        a.friendName = mock::kSignInName;
        c.session->add(a);
    }
    const bool started = c.until([&] {
        if (!c.session->state().friendsKnown())
            return false;
        for (const std::string& name : o.ais)
            if (sessionOf(c, name) == nullptr)
                return false;
        return true;
    }, kStallSeconds, why);
    if (!started) {
        *why = "the server never showed every AI's session: " + *why;
        return false;
    }
    struct Seeding {
        std::string name;
        std::vector<mock::SeedTurn> turns;
        std::size_t next = 0;           // the turn to send next
        bool open = false;              // a turn sent and not yet ended
        bool stopSent = false;
        std::uint64_t before = 0;       // the transcript's length when the open turn was sent
    };
    std::vector<Seeding> seeding;
    for (std::size_t i = 0; i < o.ais.size(); ++i)
        seeding.push_back(Seeding{o.ais[i], mock::seedFor(o.ais[i], i)});
    const bool seeded = c.until([&] {
        bool all = true;
        for (Seeding& s : seeding) {
            const sync::SessionView* v = sessionOf(c, s.name);
            if (v == nullptr)
                return false;
            if (s.open) {
                const mock::SeedTurn& t = s.turns[s.next - 1];
                if (t.ends == mock::SeedTurn::Ends::Stopped && !s.stopSent && busy(*v) && !v->entries.empty() &&
                        v->entries.back()->text.find(t.stopAfter) != std::string::npos) {
                    sync::Action stop;
                    stop.kind = sync::ActionKind::Stop;
                    stop.session = v->id;
                    c.session->add(stop);
                    s.stopSent = true;
                }
                // Ended: the prompt and at least one entry after it are in, nothing runs, and the newest is closed.
                if (v->total() >= s.before + 2 && !busy(*v) && v->queue.empty() &&
                        (v->entries.empty() || v->entries.back()->closed))
                    s.open = false;
            }
            if (!s.open && s.next < s.turns.size()) {
                sync::Action send;
                send.kind = sync::ActionKind::Send;
                send.session = v->id;
                send.text = s.turns[s.next].prompt;
                s.before = v->total();
                s.stopSent = false;
                c.session->add(send);
                s.open = true;
                ++s.next;
            }
            all = all && !s.open && s.next == s.turns.size() && v->activity == sync::Activity::Idle;
        }
        return all && c.session->allAcknowledged();
    }, kStallSeconds, why);
    if (!seeded) {
        *why = "seeding the AIs' conversations stopped: " + *why;
        return false;
    }
    return true;
}

void printClients(const Paths& p, const std::string& serverCommand) {
    std::printf("nexus-mock: serving in %s\n\n"
        "A client reaches it as any server, running this on this machine instead of `nexus-server new`:\n\n"
        "  %s\n\n"
        "The Mac window, as a window of its own (its HOME is the mock's, so it shares nothing with yours):\n\n"
        "  HOME=%s <build>/nexus.app/Contents/MacOS/nexus --server localhost --server-command \"%s\"\n\n"
        "From another machine, --server <this machine> with the same --server-command. Stop it with:\n\n"
        "  nexus-mock stop --dir %s\n",
        p.dir.c_str(), serverCommand.c_str(), p.client.c_str(), serverCommand.c_str(), p.dir.c_str());
}

int start(const Paths& p, const mock::Options& o) {
    std::string why;
    std::string serverCommand;
    if (!removeMock(p, &why) || !writeMock(p, o, &serverCommand, &why))
        return fail(why);
    mock::Client c;
    if (!c.connect(serverCommand, "nexus-mock-seeder", &why))
        return fail("the mock's server did not start: " + why + " (its log: " + p.server + "/server.log)");
    if (!seed(c, o, &why)) {
        c.close();
        return fail(why + " (the server's log: " + p.server + "/server.log)");
    }
    c.close();
    printClients(p, serverCommand);
    return 0;
}

// Connects as a client, takes a session with each AI as a window does, and writes the state the server sends to `out`.
int writeState(const Paths& p, const std::string& out) {
    if (!isMock(p))
        return fail(p.dir + " holds no mock; start one with nexus-mock start --dir " + p.dir);
    std::string command = readFile(p.command);
    while (!command.empty() && command.back() == '\n')
        command.pop_back();
    mock::Client c;
    std::string why;
    if (!c.connect(command, "nexus-mock-state", &why))
        return fail(why);
    bool ensured = false;
    const bool ok = c.until([&] {
        const sync::Sessions& s = c.session->state();
        if (!s.friendsKnown())
            return false;
        if (!ensured) {
            for (const sync::ServerFriend& f : s.friends()) {
                sync::Action a;
                a.kind = sync::ActionKind::Ensure;
                a.friendName = f.name;
                c.session->add(a);
            }
            ensured = true;
        }
        for (const sync::ServerFriend& f : s.friends())
            if (s.first(f.name) == nullptr)
                return false;
        return c.session->allAcknowledged();
    }, kStallSeconds, &why);
    const std::string text = ok ? sync::stateFile(c.session->state()) : std::string();
    c.close();
    if (!ok)
        return fail("the server's state never settled: " + why);
    return writeFile(out, text, &why) ? 0 : fail(why);
}

int usage() {
    std::fprintf(stderr, "usage: nexus-mock start [--dir DIR] [--ais N | --names A,B,...] [--sign-in] [--instant]\n"
                         "       nexus-mock stop [--dir DIR]\n"
                         "       nexus-mock command [--dir DIR]\n"
                         "       nexus-mock state --out FILE [--dir DIR]\n"
                         "DIR is /tmp/nexus-mock unless given; see docs/mock.md\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return usage();
    const std::string verb = argv[1];
    std::string dir = "/tmp/nexus-mock";
    mock::Options o;
    std::string why;
    bool named = false;
    bool counted = false;
    std::string out;
    if (!mock::aisFor(5, &o.ais, &why))
        return fail(why);
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (verb == "start" && a == "--ais" && i + 1 < argc) {
            const std::string n = argv[++i];
            if (n.empty() || n.size() > 4 || n.find_first_not_of("0123456789") != std::string::npos)
                return fail("--ais takes a whole number of AIs, as 0, 1, 5, 12 or 30");
            if (!mock::aisFor(std::stoul(n), &o.ais, &why))
                return fail(why);
            counted = true;
        } else if (verb == "start" && a == "--names" && i + 1 < argc) {
            if (!mock::aisFrom(argv[++i], &o.ais, &why))
                return fail("--names: " + why);
            named = true;
        } else if (verb == "state" && a == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (verb == "start" && a == "--sign-in") {
            o.signIn = true;
        } else if (verb == "start" && a == "--instant") {
            o.instant = true;
        } else {
            return usage();
        }
    }
    if (named && counted)
        return fail("give --ais or --names, not both");
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    if (dir.empty() || dir.front() != '/')
        return fail("--dir takes an absolute path");
    const Paths p(dir);
    if (verb == "start")
        return start(p, o);
    if (verb == "stop")
        return removeMock(p, &why) ? 0 : fail(why);
    if (verb == "state")
        return out.empty() ? usage() : writeState(p, out);
    if (verb == "command") {
        if (!isMock(p))
            return fail(p.dir + " holds no mock; start one with nexus-mock start --dir " + p.dir);
        std::printf("%s", readFile(p.command).c_str());
        return 0;
    }
    return usage();
}
