----------------------- MODULE WOverlayHeldAfterOpen -----------------------
\* Reversed witness: nexus as #10 had it, holding the window only from the window's open. The window starts hidden, a
\* hidden window has no frames, so open never comes: the launch's show and every hotkey reach no window. HotkeyShows
\* must reject it.
EXTENDS Overlay

BrokenSpec == InitWith(FALSE) /\ [][Next]_vars
=============================================================================
