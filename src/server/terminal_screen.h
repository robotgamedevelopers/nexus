// The parts of a server terminal that read the shell's output, apart from its process (server/terminal_host.h): which
// input modes the program set (TerminalModes), and each row of the screen as the text the window paints
// (paintRow, sync/terminal_paint.h). Pure over Prax's lib/vterm: no OS, no time.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "pr/vterm.h"

namespace nexus::server {

// The input modes (sync::kInputModes) a program set, read from its output as it goes to the screen: DECSET and DECRST
// (CSI ? Pm h, CSI ? Pm l) of those modes, the keypad's ESC = and ESC > (as mode 66), and the resets that clear them all
// (RIS, ESC c; DECSTR, CSI ! p). Lib/vterm keeps these modes too but does not tell them, and the window's keys must
// follow them (sync/terminal_paint.h). A sequence cut between two reads continues in the next. A string (OSC, DCS, APC,
// PM, SOS) is skipped to its end, so that its bytes never read as a mode.
class TerminalModes {
public:
    void feed(std::string_view bytes);
    // The modes set, as TerminalView::modes says them: ascending, comma-separated ("1,2004"); empty for none.
    std::string text() const;
    const std::set<std::uint32_t>& set() const { return set_; }

private:
    enum class State { Ground, Escape, Csi, String, StringEscape };
    void finishCsi(char final);
    State state_ = State::Ground;
    std::string params_;            // a CSI's parameter and intermediate bytes, bounded
    std::set<std::uint32_t> set_;
};

// Row `row` of the screen that shows (0 is the top), as text with SGR sequences from the default pen: each run of
// cells with one pen after the SGR that sets it, a cell that holds nothing as a space, the tail of a wide cluster and
// a spacer as nothing, trailing blank cells with the default pen left out, and nothing but printable UTF-8 and SGR
// (sync::cleanRow). A code point that is a control or not a scalar value is written as U+FFFD.
std::string paintRow(const PrVterm* vterm, std::uint32_t row);

} // namespace nexus::server
