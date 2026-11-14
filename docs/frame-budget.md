# The frame budget

The client runs at 120 Hz: every frame's work on the main thread fits in 8.3 ms, and anything that waits runs off
that thread (AGENTS.md, Display). Two things keep it so: the hitch detector, always on in the window, and the
frame-budget test, which fails on any frame over budget.

## The hitch detector

`src/hitch.h` (library `nexus_hitch`, no Prax and no system calls of its own). The window owns one
`HitchDetector`, made with a monotonic clock and a log line writer that the caller passes in, so its unit tests run on
a fake clock (`tests/window/test_hitch.cpp`, label `unit`).

- **Budget.** `1000 / refresh rate` ms: the main display's refresh rate as Prax reports it (`PrAppDisplayInfo::refreshRate`),
  read when the window opens and again when the displays change; 120 Hz when Prax reports none.
- **A frame.** `begin()` as the frame starts, before its work (after Prax's swap chain wait, which is not work), and
  `end()` when it is rendered. The work is the time between them; the interval is the time from the last frame's
  start, vsync to vsync. Work over budget is a hitch.
- **Phases.** `HitchScope scope(&detector, Phase::X)` around each part of the frame, or `add(phase, ms)` for a time
  measured elsewhere (Prax's own trace: rounds are `layout`, scene and play are `draw`, render is `present`). The
  phases are `input`, `terminals` (foreground and labels), `keep` (preferences), `sync apply` (the link's state into
  the model), `board`, `layout`, `draw`, `present`, `terminal tab N` (each terminal tab's own service and show),
  `other`. Work outside a frame, such as an event between frames, counts toward the next frame. Up to 16 phases a
  frame are kept; the rest are summed into `other`.
- **Memory.** A ring of the last 64 hitches, fixed in size: no allocation per frame. `hitch(i)`, `held()`,
  `hitches()`, `frames()` and `worst()` read it, for tests.
- **Log.** One line per hitch on stderr, at most one a second; a line after a quiet second counts the hitches it did
  not print. For example:
  `nexus: hitch: frame 7 took 37.71 ms of main-thread work (budget 8.33 ms, 8.40 ms since the frame before): input 0.00 ms; board 8.35 ms; layout 29.35 ms; draw 0.01 ms`
- **Nothing shows.** The detector draws nothing in the window (nexus shows no labels or hints).
- **Its own cost.** The frame-budget test measures a frame's markers (begin, 12 scopes, 3 phases added, end) on the
  real clock over 200,000 frames: about 0.6 to 0.7 µs a frame on the studio, under 0.01% of the budget. The test
  fails if it is over 1% (83 µs).

## The frame-budget test

`tests/window/test_frames.cpp`, CTest `window_frame_budget`, label `functional`: it uses real time, real shells
(`/bin/sh`) and the real link's thread over a stand-in ssh, and stays inside its working directory. It drives the
window's own code (`src/frame_host.h` `FrameHost`, the same frame start, rounds and events the app runs) with no
window: frames are laid out and played into a draw list, not presented. Each case runs in a window of its own and
reports every hitch with its frame and phases.

| Case | What it does |
| --- | --- |
| `first_frames` | the window comes up connected and draws its first frames |
| `typing_over_a_long_history` | typing into the prompt over 5,000 entries, the busy cursor breathing |
| `stalled_link_then_a_long_transcript` | the server goes quiet for seconds, comes back, and 4,000 entries arrive at once |
| `streaming_reply` | a reply streams a few hundred bytes a frame to 32 KB |
| `big_paste` | 1 MB pasted into the prompt, then edits at its end |
| `switching_tabs` | Command-digits between friends with 3,000 entries each and the sprint tab |
| `sprint_tab` | the board's page made, loaded and painted for the first time, polled, left and shown again |
| `terminals` | three Command-Ts, 10,000 lines of output, Command-W three times, a shell that cannot start |
| `text_scale` | Command-plus and minus over a long history and a terminal with 10,000 lines |
| `resizing` | a live resize, step by step, over a long history |
| `fallback_fonts` | emoji, CJK and Arabic text for the first time |
| `slow_ssh_link` | the real link over an ssh that hangs for 3 s and fails |

The preference files sit on a stand-in disk that takes 200 ms a write. Usage:
`nexus_frame_tests SLOW_SSH [CASE...]` (no case: all of them).

## What it found, and the fixes

Every hitch below failed the test before its fix.

| Hitch | Cause | Fix |
| --- | --- | --- |
| 400 ms on every case's first frames; 200 ms on Command-T, Command-W and a tab click | the terminal tabs and the selected tab written to disk on the frame | `StoreWriter` (`src/tab_order.h`): a writer thread takes the newest text of each file |
| 360 ms typing over 5,000 entries; 286 ms a long transcript arriving; 432 ms a tab switch; 25 ms each step of a resize | the whole history declared, shaped and laid out every frame | the transcript is virtualised (`src/view.h` `TranscriptItems`): only what is in sight and a screen either side is declared, each item keyed by its entry, the rest stood in for by spacers of measured or estimated height |
| frames at 8.3 to 8.6 ms while a reply streams | the open entry reshaped whole at each update | the open entry is laid out by paragraph, so only the last one changes |
| 95 to 106 ms each edit after a 1 MB paste | the prompt's text area lays out its whole text at each change | the prompt holds 32 KB, the server's text limit, and the transcript says when text was left out; Command-V reads the clipboard through the host, which sets a long paste 8 KB a frame |
| 5 to 17 ms on Command-T; the kill, join and wait on Command-W | the shell's fork and exec waited for on the frame, and its end | shells start and end on the terminals' thread; a tab shows at once and its shell appears in it |
| a sysctl and a copy of the process arguments for every terminal every frame | the foreground process read for the tab's label each frame | read on the terminals' thread four times a second, and on the frame only when a title changes (one small call, the arguments only for a new process group) |
| 6 ms opening a terminal during a text scale change | (as Command-T) | (as Command-T); a new text scale reaches a terminal when it shows |
| 197 ms on the frame the board first painted; 8 to 12 ms of page tasks a frame | the board's page made, run and painted on the frame | the page runs on its own thread, as Prax's browser runs a tab (`src/board.h`) |
| 184 ms on the first emoji or CJK text | the font set finding and loading the fallback faces on the frame | `FontPrefetch` (`src/font_prefetch.h`) loads them on a thread at launch (98 to 122 ms remain: below) |
| a swap chain made twice at the first show | the overlay made at one size and resized to the screen as it showed | the window is given the screen's frame at launch, while hidden (not covered by the test: there is no window in it) |

## Known hitches: fixes outside nexus

Each is a case marked known in the test, with a ceiling that still fails a regression on nexus's side. Both are Prax
requests.

- **`fallback_fonts`, 98 to 122 ms once, ceiling 150 ms.** Each text font of the window's kit matches the font set's
  fallback faces itself the first time a cluster needs one (Prax `text_font.cpp` `matchFallback`), on the thread that
  lays the text out, and a kit makes its own text fonts (`kit.cpp` `makeFonts`). Warming the window's kit before its
  first frame takes the same time on the main thread, and warming another kit or the same shaper on a thread does not
  help. The request: a way to match a text font's fallback faces ahead of need, or off the frame thread, or text fonts
  that share their matched order.
- **`big_paste`, 11 to 12 ms an edit at 32 KB, ceiling 20 ms.** The kit's text area shapes and lays out its whole
  text at each change, about 0.35 ms a KB on the studio. The request: a text area that reshapes only the paragraphs an
  edit changed.
