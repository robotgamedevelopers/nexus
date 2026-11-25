#include "sync/sprint_board.h"

namespace nexus::sync {

std::string sprintListenUrl(std::string_view listen) {
    const std::size_t colon = listen.rfind(':');
    std::string_view host = colon == std::string_view::npos ? std::string_view() : listen.substr(0, colon);
    const std::string_view port = colon == std::string_view::npos ? listen : listen.substr(colon + 1);
    if (host.empty() || host == "0.0.0.0" || host == "[::]" || host == "::")
        host = "127.0.0.1";
    return "http://" + std::string(host) + ":" + std::string(port) + "/";
}

std::string sprintBoardFromArgv(const std::vector<std::string>& argv) {
    for (std::size_t k = 0; k < argv.size(); ++k) {
        const std::string_view w = argv[k];
        if (w == "--listen" && k + 1 < argv.size() && !argv[k + 1].empty())
            return sprintListenUrl(argv[k + 1]);
        if (w.size() > 9 && w.substr(0, 9) == "--listen=")
            return sprintListenUrl(w.substr(9));
    }
    return {};
}

} // namespace nexus::sync
