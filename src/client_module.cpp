#include "client_module.h"
#include "client_module_version.h"

extern "C" __attribute__((visibility("default"))) const nexus::ClientModule* nexusClientModule() {
    static const nexus::ClientModule api{
        NEXUS_CLIENT_COMPATIBILITY, NEXUS_CLIENT_REVISION,
        [](nexus::FrameHost& host, PrKit* kit, PrFontSet* fonts, double time, const PrAppDisplay& display) {
            host.beginFrame(kit, fonts, time, display);
        },
        nexus::buildWindow,
        [](nexus::FrameHost& host, const PrAppEvent& event) { return host.event(event); },
        nexus::windowWantsFrame,
    };
    return &api;
}
