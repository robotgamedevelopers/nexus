#include "state_window.h"

#include <fstream>
#include <iterator>

#include "sync/ai_strip.h"

namespace nexus {

bool readStateAt(const std::string& path, sync::Sessions* held, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *why = "could not read " + path;
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string reason;
    if (!sync::readStateFile(bytes, held, &reason)) {
        *why = path + ": " + reason;
        return false;
    }
    return true;
}

bool showHeldState(const sync::Sessions& held, bool lost, std::size_t tab, Model& model, View& view, FrameHost& host,
        std::string* why) {
    sync::AIStrip{}.take(model, held);
    if (tab == 0) tab = model.active() + 1;
    if (model.tab_count() != 0 && !model.select_tab(tab - 1)) {
        *why = "no tab " + std::to_string(tab) + " (there are " + std::to_string(model.tab_count()) + ")";
        return false;
    }
    if (model.tab_count() != 0) view.selectedId = model.tab(model.active()).id;
    host.showState(held, lost);
    return true;
}

} // namespace nexus
