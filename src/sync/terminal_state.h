// The server's terminals as the sessions state carries them down (docs/terminals.md): a terminal tab whose shell runs
// on the server's machine, started and owned by nexus-server (server/terminal_host.h), so that a window that cannot
// start a process (iOS) shows it and types into it, and so that it keeps running while no window is linked.
//
// What crosses the link is the terminal's screen as the server's own terminal emulator holds it (Prax's lib/vterm),
// never the shell's byte stream: each row as text with the SGR sequences of its colours (TerminalView::lines), the
// cursor, the title, and the input modes the program set, which the window's keys must follow (the arrow keys of an
// application cursor mode, a bracketed paste). A window that links again, or a second window, is sent the screen as it
// is now, whole, and from then on only the rows that changed (Sessions::diff_from, as a session's entries are).
//
// Pure logic and data: no OS, no Prax, no time, so that it builds and is unit-tested everywhere.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::sync {

// A terminal's id: minted by the server (TerminalBook), nonzero, never reused. Its high 32 bits are the epoch of the run
// of the server that made it (Sessions::terminalEpoch), so an id of an earlier run never names a terminal of this one.
using TerminalId = std::uint64_t;

using LineRef = std::shared_ptr<const std::string>;

// The most terminals one server runs at once, and the bounds of a terminal's screen.
inline constexpr std::size_t kMaxTerminals = 16;
inline constexpr std::uint32_t kMaxTerminalColumns = 500;
inline constexpr std::uint32_t kMaxTerminalRows = 300;
// The most bytes of input one action carries (a paste of more is several actions, in order).
inline constexpr std::size_t kMaxTerminalInput = 64 * 1024;
// The most bytes of one row's painted text: 500 cells with a colour change at each.
inline constexpr std::size_t kMaxTerminalLine = 64 * 1024;

struct TerminalView {
    TerminalId id = 0;
    std::string opener;             // the token of the window whose TerminalCreate made it (empty for none)
    std::string title;              // the title the program set (OSC 0 or 2); empty: none
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::uint32_t cursorColumn = 0;
    std::uint32_t cursorRow = 0;
    bool cursorHidden = false;
    std::string modes;              // the input modes set, as DEC private mode numbers in ascending order: "1,2004"
    std::string ended;              // empty while its shell runs; else one line saying how it ended, never empty then
    std::vector<LineRef> lines;     // `rows` of them, each painted text (terminal_paint.h), shared between states
    bool operator==(const TerminalView& x) const;
};

using TerminalRef = std::shared_ptr<const TerminalView>;

// The input modes a window's keys follow, of all those a program can set: cursor keys (1), the numeric keypad (66, which
// the server also reads from ESC = and ESC >), the mouse reports (1000, 1002, 1003, 1006), focus reports (1004) and
// bracketed paste (2004). Others change only the screen, which the server keeps.
inline constexpr std::uint32_t kInputModes[] = {1, 66, 1000, 1002, 1003, 1004, 1006, 2004};
bool isInputMode(std::uint32_t mode);

// The modes of `text` ("1,2004") as numbers; false if it is not ascending input mode numbers joined by commas.
bool parseModes(std::string_view text, std::vector<std::uint32_t>* out);

// What became of a terminal a window's tab showed, by the state the window now holds (TerminalFate), so that a tab
// never changes silently: it runs; its shell ended (the tab shows the line and stays until closed); the server's run
// that held it ended (a restart: the tab says so and stays until closed); or it was closed (from this window or
// another), and the tab goes.
enum class TerminalFate { Running, Ended, Restarted, Closed };

// The fate of terminal `id`, held under `heldEpoch`, in a state whose terminals are `terminals` under `epoch`.
TerminalFate terminalFate(TerminalId id, std::uint64_t heldEpoch, std::uint64_t epoch,
                          const std::vector<TerminalRef>& terminals);

// The line a tab shows for a terminal lost to a server's restart.
inline constexpr char kTerminalRestarted[] = "This terminal ended when nexus-server restarted.";

} // namespace nexus::sync
