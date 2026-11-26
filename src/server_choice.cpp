#include "server_choice.h"

#include "client_home.h"

namespace nexus {

namespace {

constexpr std::string_view kServerHeader = "nexus server 1";

bool isLetterOrDigit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool userByte(char c) {
    return isLetterOrDigit(c) || c == '.' || c == '_' || c == '-';
}

bool hostByte(char c) {
    return isLetterOrDigit(c) || c == '.' || c == '-' || c == ':';
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

} // namespace

bool parseServerAddress(std::string_view text, bool needUser, std::string* address, std::string* why) {
    text = trimmed(text);
    const char* shape = needUser ? "user@host" : "host or user@host";
    if (text.empty()) {
        *why = std::string("type the server as ") + shape;
        return false;
    }
    if (text.size() > kServerAddressMax) {
        *why = "the server is longer than " + std::to_string(kServerAddressMax) + " bytes";
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ' ') {
            *why = std::string("the server is one word, ") + shape + ", with no spaces or control characters";
            return false;
        }
    }
    const auto at = text.find('@');
    std::string_view user;
    std::string_view host = text;
    if (at != std::string_view::npos) {
        user = text.substr(0, at);
        host = text.substr(at + 1);
        if (user.empty()) {
            *why = "there is no user before the @";
            return false;
        }
        for (const char c : user) {
            if (!userByte(c)) {
                *why = "the user takes letters, digits, ., _ and - only";
                return false;
            }
        }
    } else if (needUser) {
        *why = "name the user too, as user@host: this device has no ssh config to supply one";
        return false;
    }
    if (host.empty()) {
        *why = "there is no host after the @";
        return false;
    }
    if (host.front() == '[') {
        if (host.size() < 3 || host.back() != ']') {
            *why = "a bracketed host is an IPv6 address, as [::1]";
            return false;
        }
        for (const char c : host.substr(1, host.size() - 2)) {
            if (!(isLetterOrDigit(c) || c == ':' || c == '.')) {
                *why = "a bracketed host is an IPv6 address, as [::1]";
                return false;
            }
        }
    } else {
        if (host.front() == '-' || host.front() == '.') {
            *why = "the host cannot start with - or .";
            return false;
        }
        for (const char c : host) {
            if (!hostByte(c)) {
                *why = "the host takes letters, digits, ., - and : only";
                return false;
            }
        }
    }
    *address = std::string(text);
    return true;
}

std::string formatServerFile(const std::string& address) {
    return std::string(kServerHeader) + '\n' + address + '\n';
}

bool parseServerFile(std::string_view text, bool needUser, std::string* address, std::string* why) {
    const auto first = text.find('\n');
    if (first == std::string_view::npos || text.substr(0, first) != kServerHeader) {
        *why = "its first line is not \"" + std::string(kServerHeader) + "\"";
        return false;
    }
    std::string_view rest = text.substr(first + 1);
    if (rest.empty() || rest.back() != '\n' || rest.find('\n') != rest.size() - 1) {
        *why = "it is not one address on one line after its first";
        return false;
    }
    rest.remove_suffix(1);
    return parseServerAddress(rest, needUser, address, why);
}

TabOrderStore fileServerStore(const std::string& dir) {
    if (dir.empty()) {
        TabOrderStore none;
        none.read = [](std::string*, std::string* error) {
            *error = "the window has no home (HOME is unset)";
            return false;
        };
        none.write = [](const std::string&, std::string* error) {
            *error = "the server cannot be kept: the window has no home (HOME is unset)";
            return false;
        };
        return none;
    }
    TabOrderStore store = fileTabOrderStore(dir + "/server.txt");
    store.write = [dir, write = store.write](const std::string& text, std::string* error) {
        return makeClientHome(dir, error) && write(text, error);
    };
    return store;
}

std::string loadServerAddress(const TabOrderStore& store, bool needUser, std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    if (!store.read(&text, &error)) {
        if (!error.empty())
            *warning = "the kept server " + store.where + " is not used: " + error + "; the window asks again";
        return {};
    }
    std::string address;
    if (!parseServerFile(text, needUser, &address, &error)) {
        *warning = "the kept server " + store.where + " is not used: " + error + "; the window asks again";
        return {};
    }
    return address;
}

} // namespace nexus
