#include "link/bootstrap.h"

#include <netdb.h>
#include <sys/socket.h>

#include <cctype>
#include <cstring>
#include <sstream>

namespace nexus::link {

namespace {

const char kConnect[] = "NEXUS CONNECT ";

std::string quoted(const std::string& word) {
    std::string out = "'";
    for (char c : word) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

bool base64Char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/';
}

// The last non-empty line of `text`, trimmed: ssh's own error, or the remote command's.
std::string lastLine(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (!line.empty())
            last = line;
    }
    return last;
}

std::string ended(const CommandResult& r) {
    switch (r.end) {
        case CommandResult::End::NotFound: return "was not found";
        case CommandResult::End::NotStarted: return "could not start";
        case CommandResult::End::TimedOut: return "did not finish in time and was stopped";
        case CommandResult::End::Signaled: return "was ended by signal " + std::to_string(r.code);
        case CommandResult::End::Exited: return "exited with status " + std::to_string(r.code);
    }
    return "ended";
}

} // namespace

std::string connectLine(const std::string& port, const std::string& key) {
    return kConnect + port + " " + key;
}

bool parseConnect(const std::string& line, Connect* out) {
    if (line.compare(0, sizeof(kConnect) - 1, kConnect) != 0)
        return false;
    const std::string rest = line.substr(sizeof(kConnect) - 1);
    const std::size_t space = rest.find(' ');
    if (space == std::string::npos)
        return false;
    const std::string port = rest.substr(0, space);
    const std::string key = rest.substr(space + 1);
    if (port.empty() || port.size() > 5 || port[0] == '0')
        return false;
    for (char c : port) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    if (std::stoi(port) > 65535)
        return false;
    if (key.size() != 22)
        return false;
    for (char c : key) {
        if (!base64Char(c))
            return false;
    }
    if (out != nullptr)
        *out = Connect{port, key};
    return true;
}

std::string remoteLine(const std::vector<std::string>& command) {
    std::string inner;
    for (const std::string& w : command)
        inner += (inner.empty() ? "" : " ") + quoted(w);
    return "exec \"$SHELL\" -l -c " + quoted(inner);
}

Command bootstrapCommand(const Bootstrap& b) {
    const std::string line = remoteLine(b.command);
    Command c;
    c.program = b.ssh;
    c.arguments = {"-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", b.host, "--", line};
    c.shown = b.ssh + " " + b.host + " " + line;
    return c;
}

bool bootstrap(const Bootstrap& b, const CommandRunner& run, Connect* out, std::string* why) {
    const Command c = bootstrapCommand(b);
    const CommandResult r = run(c, b.timeoutSeconds);
    std::istringstream lines(r.out);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (parseConnect(line, out))
            return true;
    }
    // Standard output is quoted only when nothing in it looks like a CONNECT line, which could hold a key.
    const bool malformed = r.out.find("NEXUS CONNECT") != std::string::npos;
    std::string err = lastLine(r.err);
    if (err.empty() && !malformed)
        err = lastLine(r.out);
    *why = "`" + c.shown + "` " + ended(r) + (malformed ? " with a CONNECT line that does not parse" :
        " without a CONNECT line") + (err.empty() ? "" : ": " + err);
    if (r.end == CommandResult::End::Exited && r.code == 127)
        *why += "; install nexus-server on " + b.host + " where its login shell finds it (~/.local/bin)";
    else if (r.end == CommandResult::End::Exited && r.code == 255)
        *why += "; check that `ssh " + b.host + "` works without a password prompt";
    return false;
}

bool resolveHost(const Bootstrap& b, const CommandRunner& run, std::string* ip, std::string* why) {
    Command c;
    c.program = b.ssh;
    c.arguments = {"-G", b.host};
    c.shown = b.ssh + " -G " + b.host;
    const CommandResult r = run(c, 10);
    std::string name;
    if (r.end == CommandResult::End::Exited && r.code == 0) {
        std::istringstream lines(r.out);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.compare(0, 9, "hostname ") == 0) {
                name = line.substr(9);
                break;
            }
        }
    }
    if (name.empty()) {
        *why = "`" + c.shown + "` " + ended(r) + " without the host's name: " + lastLine(r.err);
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(name.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        *why = "cannot resolve " + name + " (" + b.host + "): " + gai_strerror(rc);
        return false;
    }
    const addrinfo* pick = res;
    for (const addrinfo* a = res; a != nullptr; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            pick = a;
            break;
        }
    }
    char host[NI_MAXHOST];
    const int nrc = getnameinfo(pick->ai_addr, pick->ai_addrlen, host, sizeof host, nullptr, 0, NI_NUMERICHOST);
    freeaddrinfo(res);
    if (nrc != 0) {
        *why = "cannot print the address of " + name + ": " + gai_strerror(nrc);
        return false;
    }
    *ip = host;
    return true;
}

} // namespace nexus::link
