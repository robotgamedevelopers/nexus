#include "remote.h"

#include <cctype>
#include <cstdlib>

#include "pr/json.h"

namespace nexus {

namespace {

constexpr const char* kCommand = "nova-config machine list --json";

std::string_view view(PrStringView s) {
    return s.data != nullptr ? std::string_view(s.data, s.size) : std::string_view();
}

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

std::string_view fileName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// OpenSSH's options that take a value (ssh.c, its getopt string): the value is the rest of the argument, or the next one.
bool takesValue(char c) {
    return std::string_view("bceilmopBDEFIJLOPQRSwW").find(c) != std::string_view::npos;
}

// Options after which ssh prints something and connects nowhere.
bool printsOnly(char c) {
    return c == 'G' || c == 'Q' || c == 'V';
}

std::string normalized(std::string_view host) {
    std::string s(host);
    while (!s.empty() && s.back() == '.')
        s.pop_back();
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// True if `host` is `<name>.<tailnet>.ts.net` with a tailnet of one label.
bool isTailnetName(const std::string& host, const std::string& name) {
    const std::string suffix = ".ts.net";
    if (host.size() <= name.size() + 1 + suffix.size() || host.compare(0, name.size(), name) != 0 ||
            host[name.size()] != '.' || host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    const std::string_view tailnet =
        std::string_view(host).substr(name.size() + 1, host.size() - name.size() - 1 - suffix.size());
    return !tailnet.empty() && tailnet.find('.') == std::string_view::npos;
}

} // namespace

MachineList parseMachineList(std::string_view json) {
    MachineList list;
    PrJson* doc = prJsonCreate();
    size_t at = 0;
    if (!prJsonParse(doc, pr(json), &at)) {
        list.error = std::string(kCommand) + " printed something that is not JSON (stopped at byte " +
            std::to_string(at) + "); run it in a terminal to see what it prints";
        prJsonDestroy(doc);
        return list;
    }
    PrJsonValueIndex status = prJsonMember(doc, prJsonMember(doc, 0, pr("result")), pr("status"));
    if (prJsonKind(doc, status) == PrJsonKind_String && view(prJsonString(doc, status)) != "ok") {
        list.error = std::string(kCommand) + " answered status " + std::string(view(prJsonString(doc, status))) +
            "; run it in a terminal to see why";
        prJsonDestroy(doc);
        return list;
    }
    PrJsonValueIndex items = prJsonMember(doc, 0, pr("items"));
    if (prJsonKind(doc, items) != PrJsonKind_Array) {
        list.error = std::string(kCommand) + " printed no list of machines (no \"items\" array); run it in a terminal "
            "to see what it prints";
        prJsonDestroy(doc);
        return list;
    }
    for (PrJsonValueIndex i = prJsonFirst(doc, items); i != kPrJsonMissing; i = prJsonAfter(doc, items, i)) {
        PrJsonValueIndex fields = prJsonMember(doc, i, pr("fields"));
        PrJsonValueIndex name = prJsonMember(doc, fields, pr("name"));
        PrJsonValueIndex address = prJsonMember(doc, fields, pr("address"));
        if (prJsonKind(doc, name) != PrJsonKind_String || view(prJsonString(doc, name)).empty())
            continue;
        Machine m{std::string(view(prJsonString(doc, name))), {}};
        if (prJsonKind(doc, address) == PrJsonKind_String)
            m.address = std::string(view(prJsonString(doc, address)));
        list.machines.push_back(std::move(m));
    }
    prJsonDestroy(doc);
    return list;
}

Command machineListCommand(const char* override, const char* shell) {
    return novaConfigCommand(override, shell, {"machine", "list", "--json"});
}

MachineList loadMachines(const Command& command, const CommandRunner& run) {
    CommandResult r = run(command, kFriendListTimeoutSeconds);
    MachineList list;
    list.error = commandProblem(command, r, kFriendListTimeoutSeconds);
    if (!list.error.empty()) {
        list.error += "; until then a terminal tab in ssh shows the host as typed, not the fleet machine's name";
        return list;
    }
    return parseMachineList(r.out);
}

MachineList loadMachines() {
    return loadMachines(machineListCommand(std::getenv("NEXUS_NOVA_CONFIG"), std::getenv("SHELL")), runCommand);
}

std::string sshDestination(const std::vector<std::string>& argv) {
    if (argv.empty() || fileName(argv[0]) != "ssh")
        return {};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a == "--") {
            if (i + 1 < argv.size())
                return argv[i + 1];
            return {};
        }
        if (a.size() < 2 || a[0] != '-')
            return a;
        // A group of options: flags, until one that takes a value, which is the rest of the argument or the next.
        for (std::size_t j = 1; j < a.size(); ++j) {
            if (printsOnly(a[j]))
                return {};
            if (takesValue(a[j])) {
                if (j + 1 == a.size())
                    ++i;
                break;
            }
        }
    }
    return {};
}

std::string sshHost(std::string_view destination) {
    std::string_view d = destination;
    const bool uri = d.substr(0, 6) == "ssh://";
    if (uri)
        d.remove_prefix(6);
    const std::size_t at = d.rfind('@');
    if (at != std::string_view::npos)
        d.remove_prefix(at + 1);
    if (uri) {
        if (!d.empty() && d.front() == '[') {
            const std::size_t close = d.find(']');
            return std::string(close == std::string_view::npos ? d.substr(1) : d.substr(1, close - 1));
        }
        const std::size_t colon = d.find(':');
        if (colon != std::string_view::npos)
            d = d.substr(0, colon);
    }
    return std::string(d);
}

std::string remoteLabel(std::string_view destination, const std::vector<Machine>& machines) {
    const std::string given = sshHost(destination);
    if (given.empty())
        return {};
    const std::string host = normalized(given);
    for (const Machine& m : machines) {
        const std::string name = normalized(m.name);
        if (host == name || isTailnetName(host, name) || (!m.address.empty() && host == normalized(m.address)))
            return m.name;
    }
    return given;
}

void TabLabel::observe(const std::string& remote, const std::string& title, double time) {
    if (!remote.empty()) {
        remote_ = remote;
        return;
    }
    if (!remote_.empty()) {
        // The session ended: the title now, and any that arrives in its tail, is the session's, not the tab's.
        remote_.clear();
        masking_ = true;
        tail_ = true;
        tailEnds_ = time + kSessionTailSeconds;
        after_ = title;
        return;
    }
    if (masking_) {
        if (tail_ && time < tailEnds_) {
            after_ = title;
        } else {
            tail_ = false;
            if (title != after_) {
                masking_ = false;
                after_.clear();
            }
        }
    }
    // Outside a session, the title is the tab's own, unless a session left it. The title at the session's first
    // observation is not used: ssh may have set its own by then.
    if (!masking_)
        before_ = title;
}

std::string TabLabel::label(const std::string& title) const {
    if (!remote_.empty())
        return remote_;
    if (masking_ && (tail_ || title == after_))
        return before_;
    return title;
}

} // namespace nexus
