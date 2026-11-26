#include "relay/relay_words.h"

#include <algorithm>
#include <cstddef>

namespace nexus::relay {

const char* const kDefaultRelayScript =
    "PATH=\"$HOME/.local/bin:$PATH\"; export PATH; exec nexus-link-relay \"$@\"";

std::vector<std::string> relayCommand(const std::string& host, const std::vector<std::string>& relay) {
    std::vector<std::string> where = host == "localhost" ? std::vector<std::string>{"--local"} :
        std::vector<std::string>{"--server", host};
    std::vector<std::string> words;
    if (relay.empty() || relay.front().empty()) {
        // $0 is the name sh's own messages give.
        words = {"/bin/sh", "-c", kDefaultRelayScript, "nexus-link-relay"};
    } else {
        words = {relay.front()};
    }
    words.insert(words.end(), where.begin(), where.end());
    if (!relay.empty()) words.insert(words.end(), relay.begin() + 1, relay.end());
    return words;
}

std::string windowsCommandLine(const std::vector<std::string>& words) {
    std::string line;
    for (const std::string& w : words) {
        if (!line.empty()) line += ' ';
        if (!w.empty() && w.find_first_of(" \t\n\v\"") == std::string::npos) {
            line += w;
            continue;
        }
        line += '"';
        std::size_t backslashes = 0;
        for (const char c : w) {
            if (c == '\\') {
                ++backslashes;
                continue;
            }
            if (c == '"') {
                line.append(backslashes * 2 + 1, '\\');
            } else {
                line.append(backslashes, '\\');
            }
            backslashes = 0;
            line += c;
        }
        line.append(backslashes * 2, '\\');
        line += '"';
    }
    return line;
}

namespace {

constexpr std::size_t kPlainMax = 300;

// UTF-16LE to UTF-8; a surrogate (outside the basic plane, not in wsl.exe's messages) as '?'.
std::string fromUtf16(const std::string& bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const unsigned c = static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) |
            (static_cast<unsigned>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c >= 0xD800 && c < 0xE000) {
            out += '?';
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

} // namespace

std::string plainText(const std::string& bytes) {
    std::string text = bytes;
    // UTF-16LE: zeros in at least a third of the bytes (its ASCII has every other byte zero; UTF-8 text has none). A
    // piece of such a stream cut after a '\n' starts on that '\n''s zero.
    const auto zeros = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\0'));
    if (text.size() >= 2 && zeros * 3 >= text.size()) {
        if (text.front() == '\0') text.erase(0, 1);
        if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF &&
                static_cast<unsigned char>(text[1]) == 0xFE)
            text.erase(0, 2);
        text = fromUtf16(text);
    }
    std::string out;
    bool space = true;      // none at the start
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F) {
            if (!space) out += ' ';
            space = true;
        } else {
            out += c;
            space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > kPlainMax) {
        std::size_t at = kPlainMax;
        while (at > 0 && (static_cast<unsigned char>(out[at]) & 0xC0) == 0x80) --at;   // not inside a character
        out = out.substr(0, at) + "...";
    }
    return out;
}

} // namespace nexus::relay
