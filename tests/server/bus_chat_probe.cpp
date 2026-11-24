// Connect after the server has delivered the test messages. This window sends no actions.
#include "link/bootstrap.h"
#include "link/mosh_link.h"
#include "sync/mirror.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string contents(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("could not read expected bus entry");
    return std::string(std::istreambuf_iterator<char>{file}, {});
}

void checkChat(const std::string& recipient, const std::vector<std::string>& expected,
               const std::string& forbidden) {
    using namespace nexus;
    std::string line;
    link::Connect connect;
    if (!std::getline(std::cin, line) || !link::parseConnect(line, &connect)) {
        throw std::runtime_error("missing server CONNECT line");
    }

    link::freezeClock();
    link::ClientSession client(connect.key, "127.0.0.1", connect.port, 1, connect.format);
    Model model({{recipient, {}}});
    sync::Mirror mirror;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        link::freezeClock();
        std::string why;
        if (!client.tick(&why)) throw std::runtime_error(why);
        std::vector<pollfd> fds;
        for (int fd : client.fds()) fds.push_back({fd, POLLIN, 0});
        const int ready = ::poll(fds.data(), fds.size(), std::clamp(client.waitMs(), 0, 100));
        if (ready < 0 && errno != EINTR) throw std::runtime_error("poll on the client link failed");
        link::freezeClock();
        for (const auto& fd : fds) {
            if ((fd.revents & POLLIN) && !client.receive(&why)) throw std::runtime_error(why);
        }
        if (!client.stateNum() || !client.state().first(recipient)) continue;
        if (client.state().sessions().size() != 1) throw std::runtime_error("unexpected extra recipient session");
        mirror.apply(model, client.state());
        std::vector<std::string> actual;
        for (const auto& entry : model.tab(0).transcript) {
            if (entry.kind == EntryKind::Person) throw std::runtime_error("bus delivery appeared as a human prompt");
            if (!forbidden.empty() && entry.text.find(forbidden) != std::string::npos)
                throw std::runtime_error("an unregistered sender's message reached the transcript");
            if (entry.kind == EntryKind::Bus) actual.push_back(entry.text);
        }
        if (actual == expected) return;
        if (actual.size() >= expected.size()) throw std::runtime_error("wrong senders, bodies, order or entry count");
    }
    throw std::runtime_error("timed out waiting for the receiving chat's two bus entries");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) return 2;
    try {
        checkChat(argv[1], {contents(argv[2]), contents(argv[3])}, argc == 5 ? contents(argv[4]) : "");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bus_chat_probe: " << e.what() << '\n';
        return 1;
    }
}
