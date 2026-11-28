// Native desktop presentation entry points. The window and all live state belong to the host.
#pragma once

#include "frame_host.h"

namespace nexus {
struct ClientModule {
    const char* compatibility;
    const char* revision;
    void (*begin)(FrameHost&, PrKit*, PrFontSet*, double, const PrAppDisplay&);
    void (*build)(View&, PrKit*, PrLayoutNode);
    bool (*event)(FrameHost&, const PrAppEvent&);
    bool (*wantsFrame)(const View&);
};
using GetClientModule = const ClientModule* (*)();
}
