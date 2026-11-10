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
// Command-T, Command-W, the text scale's keys) and Escape (which hides the overlay) stay with the window, but for
// Command-Up and Command-Down, which scroll the page to its top and its bottom as on macOS; other keys,
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

// The Cost breakdown panel as laid out, in CSS px, which board.h measures from the page after each paint (the page gives
// no other way to know how tall the header and tiles above it are): its top in the document; the gap above it, from the
// block before it, which is the page's gap between blocks; and the gap below it, to the block after it. A negative top
// is not measured yet.
struct SpendFit {
    double top = -1;
    double gap = 0;
    double below = 0;
};

// The style sheet nexus adds to the board's page. The page keeps its own opaque background, as nova-sprint serves it
// (the owner 2026-11-10: a see-through page is hard to read). The sheet stretches the Cost breakdown panel
// (kSpendSelector), while it is open, down to one gap above the bottom of the first screen, so that its border and
// rounded corners show with the page's own gap below them, and the next block starts at or below the screen's bottom
// (the owner 2026-11-10: not flush to the bottom, but no panel peeking in below it): a min-height of the viewport's
// height (100vh) less the top and the gap, and a bottom margin of at least the gap. The pie keeps its own size at the
// top of its column. Not measured yet, the sheet has no rule for the panel, which leaves it its own height.
// board.h adds it to each document's head, as a style element with the id kSprintStyleId, before the document paints,
// and sets it again when the measures move. The sheet starts with kSprintRepoLinkStyle, measured or not.
std::string sprintPageStyle(const SpendFit& fit);

// The footer's link to nova-sprint's repository is not shown (the owner 2026-11-10: "remove the link to nova sprint
// github"), matched by its address, not its place; a footer that holds nothing else is not shown either, so no empty
// space is left where it was. The board's own header, its logo and its "nova-sprint" title, shows as served.
constexpr std::string_view kSprintRepoLinkStyle =
    "a[href^=\"https://github.com/\"][href$=\"/nova-sprint\"], a[href^=\"https://github.com/\"][href$=\"/nova-sprint/\"] "
    "{ display: none !important; }\n"
    "footer:has(> a[href^=\"https://github.com/\"][href$=\"/nova-sprint\"]:only-child), "
    "footer:has(> a[href^=\"https://github.com/\"][href$=\"/nova-sprint/\"]:only-child) { display: none !important; }\n";

// Whether a click on a link would take the sprint tab off the board, so that board.h opens the link in the system's
// browser instead and the tab stays on the board. `href` is the link's URL as the page resolves it, `document` the
// board's document's URL, and `target` the link's target attribute. A link to another document leaves, wherever it
// is (another site, or another page of the board's server); so does a link with target _blank, which a browser opens
// in a new tab. A link to a place in the board's own document (the same URL but for its fragment) stays, as does a
// link with no URL or a javascript: one, which runs in the page.
bool linkLeavesBoard(std::string_view href, std::string_view document, std::string_view target);
constexpr std::string_view kSprintStyleId = "nexus-sprint-style";
// The Cost breakdown panel in nova-sprint's index.html: <section class="panel spend">.
constexpr std::string_view kSpendSelector = ".panel.spend";

} // namespace nexus
