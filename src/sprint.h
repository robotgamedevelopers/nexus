// The sprint tab's logic that runs without a page: where nova-sprint serves its board, which events go to the page, and
// the script and style nexus adds to the page. The page itself is board.h's, over Prax's HTML view.
//
// The board's address is read from the sprint's own record: the service `sprint-run` in nova-config, which
// `nova-sprint up` installs to run `nova-sprint run --listen <host:port>`, and which `nova-sprint up` and `check` read
// back. (`nova-sprint show run` names the same service, but answers only a name hooked in to the sprint's bus, which an
// app launched at login is not.) NEXUS_SPRINT_URL overrides it with the board's address.

#pragma once

#include <string>
#include <string_view>

#include "pr/app.h"

#include "friends.h"

namespace nexus {

// The board's address, or why there is none: one line for the tab, saying what failed and what to do.
struct SprintBoardAddress {
    std::string url;            // http://<host:port>/, empty when `error` is set
    std::string error;
};

// How long findSprintBoard waits for nova-config. It runs off the window's thread.
constexpr int kSprintRunTimeoutSeconds = 5;

// The command that reads the sprint's run service: novaConfigCommand with `service show sprint-run --json`.
Command sprintRunCommand(const char* override, const char* shell);

// Reads the command's JSON: {"result": {"status": "ok"}, "items": [{"fields": {"argv": [...]}}]}, whose argv holds
// `--listen <host:port>`. A host the service listens on everywhere (empty, 0.0.0.0 or ::) is reached at 127.0.0.1.
// Pure: no OS.
SprintBoardAddress parseSprintRun(std::string_view json);

// The board's address: `override` (NEXUS_SPRINT_URL) if set, else what `command` prints, run with `run`. Every error
// names what failed and what to do.
SprintBoardAddress findSprintBoard(const char* override, const Command& command, const CommandRunner& run);

// findSprintBoard with NEXUS_SPRINT_URL, sprintRunCommand(NEXUS_NOVA_CONFIG, SHELL) and runCommand. Blocks for up to
// kSprintRunTimeoutSeconds: call it off the window's thread.
SprintBoardAddress findSprintBoard();

// Where an event goes while the sprint tab shows.
enum class BoardRoute {
    Window,     // the window's own tree: the tab strip, and the keys nexus binds
    Page,       // the page only
    Both,       // a pointer move: the page and the strip each track the pointer
};

// The route of `event` while the sprint tab shows. `inPage` is true if the pointer is over the page's rectangle;
// `pressInPage` is true if the button now held was pressed over the page. Every Command chord (Command-1 to Command-0,
// Command-T, Command-W, the text scale's keys) and Escape (which hides the overlay) stay with the window; other keys,
// text and input-method composition go to the page; the pointer's presses and the wheel go where the pointer is, and a
// release where its press went.
BoardRoute routeBoardEvent(const PrAppEvent& event, bool inPage, bool pressInPage);

// The script nexus runs in each of the page's windows before the page's own scripts. Prax's QuickJS has no Intl, and
// its toLocaleString ignores the locale: the script supplies the part of Intl.DateTimeFormat (format, formatToParts)
// and Intl.NumberFormat the board uses, en-US only, with the Date toLocale*String forms over it, and groups the digits
// of Number and BigInt toLocaleString. `standardZone` and `daylightZone` are the short names of the local time zone
// (the system's tzname), which a date's offset picks between. Each part is used only if the engine lacks what it
// replaces.
std::string sprintPrelude(std::string_view standardZone, std::string_view daylightZone);

// The style sheet that makes the page's own background see-through, so that the window's fill shows behind it: the
// root and body backgrounds and the board's --bg colour. The page's tiles, panels and tables keep their own fills.
// board.h adds it to each document's head, as a style element with the id kSprintStyleId, before the document paints.
std::string_view sprintPageStyle();
constexpr std::string_view kSprintStyleId = "nexus-see-through";

} // namespace nexus
