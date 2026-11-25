// How a window shows one of the server's terminals (sync/terminal_state.h): it keeps a terminal emulator of its own
// (Prax's, with no process: remote_terminals.h), and writes into it the escape sequences that make its screen the
// server's. paintTerminal works those out from the view the window painted last and the one it now holds: the rows
// that changed, the input modes that changed, then the cursor. Pure: no OS, no Prax.
//
// What it writes, and why each part is safe in an emulator the window owns:
// - First, and on every change of size: autowrap off (DECAWM, ?7l), so that a row wider than the window's screen (the
//   window resized and the server has not yet) is cut at the right edge rather than wrapped and scrolled; the pen
//   reset; the screen and the scrollback erased.
// - Each row that changed: the cursor to its start, the pen reset, the row erased, then its text, which is the server's
//   cells with SGR sequences only (server/terminal_host.h paintRow). A row past the window's screen is left out.
// - Each input mode that changed, set or reset (DECSET/DECRST), so that the emulator's keys (an arrow, a paste) send
//   what the program asked for. No other mode is ever set.
// - The cursor's place and whether it shows.
// The server's text never reaches the emulator unpainted: paintRow writes SGR and printable characters only, and the
// window drops a row holding any other control (cleanRow) rather than write it.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sync/terminal_state.h"

namespace nexus::sync {

// The bytes that turn a screen showing `held` (null: never painted, or painted at another size) into one showing `now`,
// on an emulator of `columns` by `rows`.
std::string paintTerminal(const TerminalView* held, const TerminalView& now, std::uint32_t columns, std::uint32_t rows);

// True if `text` is a row as paintRow writes it: printable UTF-8 and SGR sequences (ESC [ digits ; : m) only.
bool cleanRow(std::string_view text);

} // namespace nexus::sync
