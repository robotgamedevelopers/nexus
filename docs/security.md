# Security: the client lock and the link

nexus talks directly to AIs that can act: run tools, edit code, reach the fleet. Whoever holds an unlocked nexus window
holds all of that. So no one uses nexus without authenticating first (the owner, 2026-11-25: "you cannot use nexus
without auth"; while locked, "no dashboard, no talk to ais, no terminals. nothing.").

This page says what the **client lock** does, how it and the **link's own authentication** fit together, and what each
does not cover.

## The client lock

Every window (the macOS overlay and `--windowed` window, the Windows window, and the iOS app with the same rules) is
locked until the device's owner passes the system's own authentication:

| Client | Authentication | Where |
|---|---|---|
| macOS | LocalAuthentication's device-owner policy: Touch ID, a paired Apple Watch, or the account's login password | `src/lock_mac.mm` |
| Windows | Windows Hello (face, fingerprint or PIN) through `UserConsentVerifier`, asked over the nexus window | `src/lock_win.cpp` |
| iOS | Face ID or the passcode (the same model, `models/ClientLock.tla`) | the iOS app |

nexus never sees a fingerprint, a face, a PIN or a password: the system checks the owner and answers yes or no.

**While locked** the window draws the lock screen alone: a card saying that nexus is locked, why, and how to unlock.
There is no Dashboard, no tab strip, no AI's tab or name, no transcript, prompt or draft, no terminal, no dialog, and
no sound of what an AI did. No input reaches anything but the lock screen: typed keys, text, pastes, scrolls and drops
are dropped before the board, a terminal's shell or the prompt can see them. Return or a click on the card asks the
system again; Escape hides the overlay. nexus posts no system notifications, so none can carry content, and plays no
sound while locked (`View::chirp`). A locked window claims no sign-in and opens nothing in the browser: locking takes
back the claim a show allowed, no sign-in step runs until unlocked, and a page's link that arrives while locked is
dropped, with a line in the log, never kept to open later. The tests: `window_input` `lockedWindowDrawsOnlyTheLockScreen`
(the tree holds the lock screen's two lines and nothing else), `frame_host_unit` `lockedHostDropsInput`,
`window_unit_signin` `lockedClaimsNothing`, and `window_lock_unit`.

**It locks** at launch, and again when:

- the machine goes to sleep (Prax's `PrAppEventKind_Suspend`; on Windows also the display turning off, which modern
  standby sends where it sends no suspend) or the displays sleep (macOS);
- the screen locks (Prax's `PrAppEventKind_ScreenLocked`);
- the user session switches away: fast user switching on macOS; a console or remote disconnect, a logoff, or remote
  control starting or stopping on Windows;
- the phone's app goes to the background (iOS);
- the window has had no input for the idle timeout.

Locking from unlocked hides the overlay at once, so nothing of nexus stays on the screen, and takes away a system
prompt that is still up: its answer, if one still comes, is stale and unlocks nothing. A `--windowed` window, which
stays on the screen, is veiled at once (its opacity to 0, which the window server applies before a sleep) and shows
again only with the lock screen's frame, so that no frame of content stays in a capture, the Dock's thumbnail or
Mission Control.

**It asks** on the macOS overlay hotkey while the overlay stays hidden. Only a current successful authentication
reveals Nexus; cancellation or failure keeps it hidden, and another hotkey retries. Repeated hotkeys during a prompt
do not start another. Launch itself still shows nothing and takes no focus (AGENTS.md). Other windows ask when
shown locked; the `--windowed` window asks as it opens. After a refusal (the check failed, the prompt was cancelled, or
the system has no way to authenticate) it does not ask again by itself. On a visible locked window the lock screen says which, with the system's
own words, and waits for Return or a click. A system with no way to authenticate the owner (no Windows Hello set up)
leaves nexus locked, and the lock screen says to set it up.

**The idle timeout** is the client policy's `lock` event, `{"idle_minutes": N}`; the shipped rule
(`policy/rules/client/window.star`) answers config `lock_idle_minutes`, **10** unless set. N is a whole number of
minutes from 1 to 240. Nothing turns the lock off: any other answer is the default, with a line in the log saying why
(`window_app.h` `lockIdleSeconds`). Only the person's deliberate input starts the idle time again: a key, text, a press,
a scroll, a paste or a drop. A pointer or a drag passing over the window, a gesture or a touch on the trackpad does
not, so nothing but the person's act keeps nexus unlocked. The window checks every 5 seconds, also while hidden, at
every frame, before the overlay shows, and at every input, so it locks within 5 seconds of the timeout, and input that
arrives after the timeout locks before it is delivered. The `--windowed` window reads the timeout from the policy too.

**The model**: `models/ClientLock.tla` (`models/README.md`): `locked`, `prompting`, `unlocked`; only the system's grant
for the attempt that is current unlocks; every relock locks from any state; a frame draws content and input is
delivered if and only if unlocked. Nine reversed witnesses, and the code (`src/client_lock.h`) bound to it by
`tests/model/lock_walk.cpp` and `ClientLockTrace.tla` in CI.

## The link, and how the two fit

The window reaches `nexus-server` by `ssh <host> nexus-server new` (the person's own ssh keys and `known_hosts`), which
hands back a UDP port and a session key; from then on mosh's transport carries everything, encrypted and authenticated
with that key (AES-128-OCB). The server authenticates the *machine and account* that connect: whoever can ssh there as
the person. It knows nothing of the client lock, and trusts nothing a window says about it.

The client lock authenticates the *person at the device*, to the device. The two are separate walls and neither stands
in for the other:

- The lock weakens nothing on the link. A locked window keeps its link: the AIs' work goes on, the transcripts keep
  arriving into the window's memory, terminals' shells keep running, unseen. The lock hides and blocks; it does not
  disconnect, and it does not encrypt what the window holds.
- The link's authentication does not stop someone at an unlocked, unattended window; the lock does.
- Anyone who can run code as the same user on the device can reach the server without the window at all (`ssh <host>
  nexus-server ...`), read the window's memory, and read `~/.nexus/client`. The lock does not stand against that:
  the system's own login and account separation do.

## Threats

| Threat | What stands against it | What is left |
|---|---|---|
| An unlocked Mac left unattended | the idle timeout (10 minutes unless set) and the system's own screen lock, each of which locks nexus | up to the timeout (and 5 s) of use by whoever sits down first |
| A stolen laptop, asleep | sleep and the displays sleeping lock nexus; on wake it shows only the lock screen, and the system's login stands before it | a thief who gets past the system's login is the owner as far as nexus can tell; disk encryption (FileVault, BitLocker) guards the data at rest, not nexus |
| Another local user on the same machine | each user's `~/.nexus/client` is owner-only (0700), and a session switch locks nexus | none from the lock's side; an administrator can read any user's memory and files |
| Screen sharing or a screenshot | while locked nothing of nexus is drawn, so nothing can be captured | while unlocked, nexus shows on a shared screen like any other window |
| Someone with the person's ssh key or user account | the link's ssh authentication only | the client lock does not cover this: it guards the window, not the account |
| A test build | the stand-in authenticator exists only with the CMake option `NEXUS_TEST_AUTHENTICATOR`, off by default; it is a configure error in a Release, multi-config or signed build; every other build of the app is checked after linking for none of its words (`cmake/no_test_authenticator.cmake`, and the unit test `nexus_no_test_authenticator` on CI's Release build); a test build warns at configure, logs at start, and names itself on the lock screen | anyone who builds nexus as Debug with it on gets a nexus that checks no one |
| A screenshot of the live window | `--screenshot` draws a state file or the demo only, and refuses otherwise | none |

## What is verified, and how

- The rules: `client_lock_unit` (unit tests, time as an argument), and the model with its witnesses and the trace
  check (`ctest -L model -R client_lock`).
- The window's drawing and input while locked: `window_input` and `frame_host_unit` (headless, no window).
- What the app does around the lock (asking, relocking, hiding, veiling, the sign-in taken back, the idle reset, links
  and sounds while locked): `window_lock_unit`, over recording hooks (`src/window_lock.h`); main.cpp gives it the window.
- The system's codes: the LocalAuthentication and Windows Hello answers and the Windows session events are mapped by
  plain functions (`client_lock.h`), unit-tested, and each platform file checks the numbers against its SDK at
  compile time.
- macOS: the app built with the stand-in authenticator, run as the overlay against a test server, and screenshotted
  locked and unlocked. Touch ID and the password prompt themselves cannot be driven by a test: no test touches the
  sensor or types the owner's password.
- Windows: the Windows code compiles in CI (`nexus_platform_win_check`, MSVC at `/W4 /WX`). On captain (2026-11-25)
  the Windows window, `lock_win.cpp` with it, builds and links at Prax's current pin, and the lock's unit tests pass
  there (`client_lock_unit`, `window_lock_unit`, `window_unit_signin`, `nexus_no_test_authenticator`), but only with
  MSVC's warnings not made errors: the window's older code (`view.cpp`, `window_app`, `chat_selection.cpp`,
  `client_home.cpp`, `terminals.cpp`) has `/W4` warnings, so `NEXUS_WINDOW` stays off on Windows. Captain has no
  interactive desktop, so Windows Hello and the session events are verified only through the seam (the mapping
  functions and the rules), not on a desktop.

## Requests for Prax's author

Prax reports sleep and the screen locking. nexus watches the rest itself (`src/client_lock_platform.h`): on macOS the
displays sleeping and the session switched away; on Windows a console or remote disconnect, a logoff, and the display
turning off. Prax events for these (and a lock-relevant lifecycle event on desktop) would let those files go.
