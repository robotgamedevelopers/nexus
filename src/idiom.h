// The device's idiom, which the client policy reads (ui.idiom) to choose the window's form (docs/voice.md): "phone" on
// an iPhone, "pad" on an iPad, and "desktop" elsewhere. platform_ios.mm says which on iOS; the desktop builds never ask.
#pragma once

#include <string>

namespace nexus {

std::string deviceIdiom();

} // namespace nexus
