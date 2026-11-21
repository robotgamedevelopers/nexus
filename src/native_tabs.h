// Native Prax tab implementations register a renderer by name. The Starlark window chooses where it appears.
#pragma once

#include "model.h"
#include "pr/kit.h"

#include <map>

namespace nexus {
using NativeTabDraw = void (*)(PrKit*, PrLayoutNode, Model&, bool canFocus);
using NativeTabs = std::map<std::string, NativeTabDraw>;
NativeTabs nativeTabTypes();
void drawTextEditor(PrKit* kit, PrLayoutNode parent, Model& model, bool canFocus);
}
