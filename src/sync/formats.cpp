#include "sync/formats.h"

#include <vector>

#include "wire/tables.h"

namespace nexus::sync {

std::string windowRefusal(std::uint32_t window, std::uint32_t oldest) {
    if (window >= oldest) {
        return std::string();
    }
    return "This window is too old for nexus-server (it reads state format " + std::to_string(window) +
        ", and the server sends " + std::to_string(oldest) + " or later): update nexus.";
}

std::string serverRefusal(std::uint32_t server, std::uint32_t oldest) {
    if (server >= oldest) {
        return std::string();
    }
    return "nexus-server is too old for this window (it sends state format " + std::to_string(server) +
        ", and the window reads " + std::to_string(oldest) + " or later): update nexus-server, then run "
        "`nexus-server stop` so the next window starts the new one.";
}

std::string refusalOfWindow(const Hello& window, const Hello& server) {
    const std::string refused = windowRefusal(window.proto, server.minPeer);
    if (!refused.empty()) {
        return refused;
    }
    return serverRefusal(server.proto, window.minPeer);
}

std::string refusalOfServer(const Hello& server, const Hello& window) {
    const std::string refused = serverRefusal(server.proto, window.minPeer);
    if (!refused.empty()) {
        return refused;
    }
    return windowRefusal(window.proto, server.minPeer);
}

namespace {

const char kHex[] = "0123456789abcdef";

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// The longest Hello text read: a Hello is two numbers and its id table, well under a hundred bytes; a later version's
// may add a few fields.
constexpr std::size_t kHelloTextMax = 1024;

} // namespace

std::string helloText(const Hello& hello) {
    nexuswire::HelloBuilder b;
    b.GetRoot()->proto = hello.proto;
    b.GetRoot()->min_peer = hello.minPeer;
    const int64_t need = nexuswire::HelloMeasure(b);
    if (need <= 0) {
        return std::string();
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(need));
    const int64_t wrote = nexuswire::HelloSave(b, bytes.data(), need);
    if (wrote != need) {
        return std::string();
    }
    std::string text;
    text.reserve(bytes.size() * 2);
    for (std::uint8_t v : bytes) {
        text += kHex[v >> 4];
        text += kHex[v & 15];
    }
    return text;
}

bool readHello(const std::string& text, Hello* out, std::string* why, int* unknown) {
    if (text.empty() || text.size() % 2 != 0 || text.size() > kHelloTextMax) {
        *why = "a Hello of " + std::to_string(text.size()) + " characters, not an even number from 2 to " +
            std::to_string(kHelloTextMax);
        return false;
    }
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int hi = hexValue(text[2 * i]);
        const int lo = hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            *why = "a Hello that is not lowercase hex";
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    const int64_t size = static_cast<int64_t>(bytes.size());
    const int64_t need = nexuswire::HelloLoadMeasure(bytes.data(), size);
    if (need < 0) {
        *why = "a Hello that does not load as schema's table";
        return false;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    nexuswire::TableReport report;
    const nexuswire::Hello* h = nexuswire::HelloLoad(region.data(), need, bytes.data(), size, &report);
    if (h == nullptr || report.damaged || report.refused) {
        *why = "a Hello that does not load as schema's table";
        return false;
    }
    if (h->proto == 0) {
        *why = "a Hello with proto 0";
        return false;
    }
    if (unknown != nullptr) {
        *unknown = report.unknown;
    }
    *out = Hello{h->proto, h->min_peer};
    return true;
}

} // namespace nexus::sync
