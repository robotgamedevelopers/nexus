// The two things the window needs from macOS that Prax's lib/app does not offer: entering fullscreen, and the size of
// the main display without a window.

#pragma once

namespace nexus {

// Puts the window that holds `nsView` (PrAppSurface::handle) into native macOS fullscreen.
void enterNativeFullScreen(void* nsView);

// Writes the main display's size in logical points and its backing scale. Returns false if there is no display.
bool mainDisplay(float* width, float* height, float* scale);

} // namespace nexus
