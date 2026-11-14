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
| `big_paste` | 1 MB pasted with Command-V (through the prompt's own paste), another tab the next frame, edits of the full prompt, typing at the limit, the prompt selected and typed over, 40 KB pasted and sent the next frame |
| `switching_tabs` | Command-digits between friends with 3,000 entries each and the sprint tab |
| `sprint_tab` | the board's page made, loaded and painted for the first time, polled, left and shown again |
| `terminals` | a command typed into a new tab before its shell has started, three Command-Ts, 10,000 lines of output, Command-W three times, a shell that cannot start |
| `text_scale` | Command-plus and minus over a long history and a terminal with 10,000 lines |
| `resizing` | a live resize, step by step, over a long history |
| `scrolling_up_a_long_history` | 5,000 entries scrolled up from the end a screen a frame and back, the estimated heights replaced by measured ones as items come into sight |
| `fallback_fonts` | emoji, CJK and Arabic text for the first time |
| `slow_ssh_link` | the real link over an ssh that hangs for 3 s and fails |

The preference files sit on a stand-in disk that takes 200 ms a write. Usage:
`nexus_frame_tests SLOW_SSH [--inject CASE FRAME] [CASE...]` (no case: all of them).

**A frame fails only if it is over the budget in every run.** A case with a frame over the budget runs again in a
fresh window, up to three runs in all. A real hitch is work on the frame thread, and it comes back on the same frame
each run; a frame the scheduler held up on a loaded host does not. The same frame is the same frame number when the
runs have as many frames, and otherwise (a case that waits on real time) the same place among the case's input events
with the same phase taking the most time. A failure names the frame, that phase, and each run's wall-clock time beside
the frame thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`): CPU time near the wall time is work, far below it is waiting.
A frame that is not over again is reported as held up by the host. The budget stays 8.3 ms in every run.

**A window's first frame keeps half the budget, 4 ms.** Each case reports its first frame's work, CPU time and phases.
A first frame over 4 ms in every run fails the case, by the same three-run rule, so that a host doing the same work at
half the speed (every core busy) still keeps 8.3 ms. With the faces made before it (`warmShaper`) a first frame needs
0.2 to 1.1 ms on an unloaded studio; without, 4.3 to 5.5 ms, and the check failed in every run. On macOS the
test's frames run at the QoS of an app's main thread (user-interactive), as the window's do; at the default QoS a loaded
host moves the thread to the efficiency cores, where the same work takes about twice the CPU time.

The proof that a real hitch still fails: `--inject` gives one frame 10 ms of busy work in the Keep phase.
`window_frame_budget_injected` (a case with a fixed frame count) and `window_frame_budget_injected_paced` (one that waits
on real time) pass only when the test reports that frame as a hitch in every run.

In CI the frame budget runs after the other functional tests, alone, under `nova-reserve run --exclusive` when every
slot on the host is free within 20 s, and shared otherwise.

## What it found, and the fixes

Every hitch below failed the test before its fix.

| Hitch | Cause | Fix |
| --- | --- | --- |
| 400 ms on every case's first frames; 200 ms on Command-T, Command-W and a tab click | the terminal tabs and the selected tab written to disk on the frame | `StoreWriter` (`src/tab_order.h`): a writer thread takes the newest text of each file |
| 360 ms typing over 5,000 entries; 286 ms a long transcript arriving; 432 ms a tab switch; 25 ms each step of a resize | the whole history declared, shaped and laid out every frame | the transcript is virtualised (`src/view.h` `TranscriptItems`): only what is in sight and a screen either side is declared, each item keyed by its entry, the rest stood in for by spacers of measured or estimated height |
| frames at 8.3 to 8.6 ms while a reply streams | the open entry reshaped whole at each update | the open entry is laid out by paragraph, so only the last one changes |
| 95 to 106 ms each edit after a 1 MB paste | the prompt's text area lays out its whole text at each change | the prompt holds 32 KB, the server's text limit: Command-V reads the clipboard through the host, which cuts text past the room left (32 KB less the draft, plus the selection it replaces) and hands it on in one event, in its place among the events; the end of that tab's transcript says how much was left out ("1,015,808 of 1,048,576 bytes were left out: the prompt holds 32 KB."), brought into sight, until the draft changes |
| 5 to 17 ms on Command-T; the kill, join and wait on Command-W | the shell's fork and exec waited for on the frame, and its end | shells start and end on the terminals' thread; a tab shows at once and its shell appears in it; what is typed meanwhile is held and reaches the shell in order once its screen has the focus |
| a sysctl and a copy of the process arguments for every terminal every frame | the foreground process read for the tab's label each frame | read on the terminals' thread four times a second, and on the frame only when a title changes (one small call, the arguments only for a new process group) |
| 6 ms opening a terminal during a text scale change | (as Command-T) | (as Command-T); a new text scale reaches a terminal when it shows |
| 197 ms on the frame the board first painted; 8 to 12 ms of page tasks a frame | the board's page made, run and painted on the frame | the page runs on its own thread, as Prax's browser runs a tab (`src/board.h`) |
| 184 ms on the first emoji or CJK text | the font set finding and loading the fallback faces on the frame | `FontPrefetch` (`src/font_prefetch.h`) loads them on a thread at launch (98 to 122 ms remain: below) |
| 3.4 to 5.2 ms on every window's first frame (9 to 9.7 ms with every core busy) | the shaper making each text font's face (CoreText reading the font's tables) the first time it shapes with it: over half the frame, and nearly all of it with no history | `warmShaper` (`src/font_prefetch.h`): when the window opens, before its first frame, a line of Latin text is set in every text role of the window's theme over the Studio app's shaper, which keeps the faces; first frames are now 0.2 to 1.1 ms |
| a swap chain made twice at the first show | the overlay made at one size and resized to the screen as it showed | the window is given the screen's frame at launch, while hidden (not covered by the test: there is no window in it) |

## Known hitches: fixes outside nexus

Each is a case marked known in the test. Only the frames the case marks known (`Window::known`) may pass the budget,
each under the case's ceiling, the worst measured on the studio and about 15%; every other frame of the case keeps
8.3 ms. Both are Prax requests. On a heavily loaded machine (load average over 50) the measured frames run up to
several times longer; a known frame past its ceiling fails only if it is past it in every run, as above.

- **`fallback_fonts`, 60 to 124 ms on the first frame that sets such text, ceiling 145 ms.** Each text font of the window's kit matches the font set's
  fallback faces itself the first time a cluster needs one (Prax `text_font.cpp` `matchFallback`), on the thread that
  lays the text out, and a kit makes its own text fonts (`kit.cpp` `makeFonts`). Warming the window's kit before its
  first frame takes the same time on the main thread, and warming another kit or the same shaper on a thread does not
  help. The request: a way to match a text font's fallback faces ahead of need, or off the frame thread, or text fonts
  that share their matched order.
- **`big_paste`, 10.5 to 13 ms, ceiling 15 ms**, on each frame that sets 32 KB in the prompt: the paste, each edit of
  a full prompt, and a full prompt's tab shown again. The kit's text area shapes and lays out its whole text at each
  change, about 0.35 ms a KB on the studio. The request: a text area that reshapes only the paragraphs an edit changed.

## Preferences on a kill

The tab order, the terminal tabs, the selected tab and the text scale are written by the writer thread (`StoreWriter`), never on the
frame. A kill of the app (not a quit, which writes what is waiting) loses at most the one write waiting after the last
change: the files then hold the state from before it. Accepted for the frame budget.
