#include "client_module.h"
#include "client_module_version.h"

#include <cstdio>

namespace {
void build(nexus::View& view, PrKit* kit, PrLayoutNode root) {
    static bool first = true;
    if (first) {
        first = false;
        auto& model = *view.model;
#if FIXTURE_STEP == 1
        model.add_ai({"update fixture", "offline model"});
        for (size_t i = 0; i < model.tab_count(); ++i) {
            if (model.tab(i).name == "update fixture" && model.select_tab(i) && model.set_draft(i, "unsent draft"))
                std::fprintf(stderr, "fixture draft ready\n");
        }
#else
        if (model.tab_count() && model.tab(model.active()).name == "update fixture" &&
                model.tab(model.active()).draft == "unsent draft")
            std::fprintf(stderr, "fixture tab and draft preserved\n");
#endif
    }
    nexus::buildWindow(view, kit, root);
}
}

extern "C" __attribute__((visibility("default"))) const nexus::ClientModule* nexusClientModule() {
    static const std::string revision(64, '0' + FIXTURE_STEP);
    static const nexus::ClientModule api{
        NEXUS_CLIENT_COMPATIBILITY, revision.c_str(),
        [](nexus::FrameHost& host, PrKit* kit, PrFontSet* fonts, double time, const PrAppDisplay& display) {
            host.beginFrame(kit, fonts, time, display);
        },
        build, [](nexus::FrameHost& host, const PrAppEvent& e) { return host.event(e); }, nexus::windowWantsFrame,
    };
    return &api;
}
