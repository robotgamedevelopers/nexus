# nexus on iPhone and iPad

One universal app for iPhone and iPad: a second client of `nexus-server`, a thin portal like the desktop window. It
sends what the person types and shows the server's state; every decision stays on the server (AGENTS.md). It is built
from the same sources as the desktop window, over Prax's UIKit backend and its Metal renderer.

## Building

On a Mac with Xcode 27 (its iOS SDK, its Metal toolchain: `xcodebuild -downloadComponent MetalToolchain`) and rustup
with the iOS targets (`rustup target add aarch64-apple-ios aarch64-apple-ios-sim`), which the Starlark policy runtime
(policy/native, Rust) needs:

```sh
# The simulator, where the tests run (an iOS simulator runtime: xcodebuild -downloadPlatform iOS)
cmake -S . -B build-ios-sim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-ios-sim -j "$(sysctl -n hw.ncpu)"
ctest --test-dir build-ios-sim -L 'unit|ios' -j "$(sysctl -n hw.ncpu)"

# Devices (arm64)
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-ios -j "$(sysctl -n hw.ncpu)"
```

The build also fetches and compiles what the desktop takes from the machine, each a pinned release checked by its
SHA-256 (`cmake/ios_link.cmake`): protobuf's lite runtime with protoc for the Mac that builds, mbedTLS, and libssh2.

The app is `build-ios*/nexus.app`, signed ad hoc as the simulator requires. The simulator build's tests run on a
simulator device through Prax's runner (ctest's emulator), which boots an available iOS device unless
`PR_SIMULATOR_DEVICE` names one. CI builds both and runs the tests (`.github/workflows/ios.yml`).

To see the app in the simulator: `xcrun simctl install booted build-ios-sim/nexus.app`, then
`xcrun simctl launch booted com.robotgamedevelopers.nexus`. With `--state FILE` after the identifier it shows a server's
state file, as `nexus --screenshot --state FILE` draws it on the desktop. `--server USER@HOST --ssh-port N` links to a
server whose sshd answers on port N in place of 22: an sshd of a test's own, beside the simulator.

## On an iPhone: Talk to your AIs

On an iPhone the app opens on a menu of your tabs with a microphone at the bottom: say an AI's name and what to
ask, and the reply comes back on the menu, spoken. Speech is recognized on the phone and sent as an ordinary prompt.
An AI's own page has the way back, the transcript and the prompt, whose microphone is plain dictation. The form comes
from the client policy by device idiom; an iPad keeps the desktop's strip. See [voice.md](voice.md) for the form, the
voice turn, its tests (`ctest -L speech` on a simulator) and how a watch reuses it.

## Reaching the server

The app reaches nexus-server as the desktop window does: over ssh it runs `nexus-server new` on the server's machine, then
keeps mosh's session to the port and key that prints (`src/link/client.h`). An iOS app cannot run `/usr/bin/ssh`, so the
ssh is inside the app (`src/link/inapp_ssh.h`, libssh2 over mbedTLS); everything after it is the desktop's code.

- **Which server.** The client config's `server user@host` (`Library/Application Support/.nexus/client/config` in the
  app's container), or `--server user@host` at launch. The user is part of it: the device has no ssh config to take one
  from. On a tailnet, the host is the server's tailnet name, reached through the Tailscale app.
- **The device's key.** The app makes an ECDSA P-256 key on first use and keeps it in its client home (`ssh-key`, 0600);
  it never leaves the device. Its public line is in the app's Documents (`nexus-ssh-key.pub`, shown by the Files app and
  a Mac's Finder), and in the window's message when the server does not take it.
- **On the server's machine** the line goes into `~/.ssh/authorized_keys` behind `tools/nexus-ios-gate`, installed in
  `~/.local/bin`, so that the key runs `nexus-server new` and `nexus-server history-page` and nothing else:

  ```
  restrict,command="$HOME/.local/bin/nexus-ios-gate" ecdsa-sha2-nistp256 AAAA... nexus-ios
  ```

- **The server's host key** is trusted on first use and kept in `known_hosts` in the client home; a different key later is
  refused, with what to do.

Until the server has sent a tab, the window shows the link's line: connecting, or why it failed.

## On a device

`tools/ios-device-run TEAM_ID DEVICE_UDID build-ios OUT_DIR [-- --server user@host]` signs a device build for a paired
iPhone or iPad with Xcode's own signing (the development certificate in the keychain, and a team provisioning profile,
which Xcode makes the first time), installs it with `devicectl` and launches it. `xcrun devicectl device capture screenshot`
takes the device's screen.

## What iOS leaves out, by design

An iOS app starts no processes: the system gives it none. So the iOS build leaves out, rather than building to fail at
run time, everything that runs one (`src/build_parts.h`, `NEXUS_PROCESSES`; `cmake/ios.cmake`):

- **nexus-server**, its store and drivers. The server runs on a Mac or Linux machine; the app reaches it.
- **Looking up the sprint board.** A desktop window can find the board's address itself by running nova-config. The
  app takes the address nexus-server sends in its state (`SessionsDiff.sprint_url`, `docs/formats.md`) and shows the
  board in its sprint tab, the Dashboard, through the same page code as the desktop (`src/board.cpp`).
- **Attachments.** A file goes to the server through ssh running `nexus-receive`, a child process. The in-app ssh
  runs only the link's commands, so attaching a file, or pasting text long enough to become one, says that it is not
  available on iOS yet; nothing is lost.

## Terminal tabs

A terminal tab on iOS is a shell that runs on the server (`docs/terminals.md`): nexus-server starts it, keeps its
screen and keeps it running while the app is away; the tab shows the screen and sends what is typed, through the link
the app already has. On iPad, the + after the last tab, or Command-T on a keyboard, opens one; on iPhone, the New
terminal row at the end of the list. The close at the end of the key row, or Command-W, closes the tab and ends its
shell, as on the Mac. A terminal another window opened, on this device or another, shows in a tab of its own here
too.

Under the screen is a row of the keys the on-screen keyboard lacks: Escape, Tab, Control (held for the next key) and
the arrows, then close. The phone's keyboard types the rest; a hardware keyboard types everything. iOS hands a hardware key's
character to the app after the key itself, at times after the next keys: so a key that types a character sends it as
the key goes down, and the character that follows is dropped, so that `ls` then Return reaches the shell as typed
(`TerminalKeys`, `src/remote_terminals.h`; Prax's own terminal view would need the same on iOS).

What a terminal tab keeps on the device: the screen, in memory, while the tab is open. Nothing in a file or a log.



The app's home is `Library/Application Support/.nexus/client` inside its own container (`src/client_home.h`): HOME on
iOS is the container, whose top an app may not write. It holds what the desktop window's `~/.nexus/client` holds.

## The overlay

An iOS app's window is the system's to show, so there is no overlay and no global hotkey: the app's window fills its
scene, on iPhone and on iPad. Command is the key left of the space bar on an iPad's keyboard, as on a Mac, so the
window's keys are the desktop's.

## For Prax

What nexus works around on iOS, for Prax to take up (Prax stays pinned; nexus does not patch it):

- Studio's window panel pads its content to the safe area but stays the screen's height, so a window of full height
  runs past the home indicator; and it does not leave room for the on-screen keyboard (`PrAppFrame.keyboard`). nexus
  sizes its window from `PrAppFrame.safeArea` and `keyboard` itself (`usableHeightOf`, src/main_ios.cpp).
- A button has no accessibility label: the back chevron on an AI's page should read "Back" to VoiceOver.
- Text has no ellipsis: nexus shortens a long title from the last layout's measure (src/view.cpp fitPhoneTitle).
- The UIKit backend sends touches as pointer events with no scrolling, so the menu's scroll is nexus's own
  (src/finger_scroll.h).
