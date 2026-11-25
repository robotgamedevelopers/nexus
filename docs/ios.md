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
state file, as `nexus --screenshot --state FILE` draws it on the desktop.

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
- **What it runs there.** `nexus-server new`, unless the client config's `server-command <words>` (or
  `--server-command <words>` at launch) names another command, as on the desktop: words split at spaces and tabs, with no
  quoting (`src/link/bootstrap.h` serverCommandWords), such as the mock's `/usr/bin/env HOME=<home> <nexus-server> new
  --dir <dir>`. A key behind `nexus-ios-gate` runs only `nexus-server new` and `history-page`, so the gate refuses any
  other command: a server command reaches only a server whose key is not behind the gate.

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
- **Terminal tabs.** A terminal tab is a local login shell. There is no Command-T.
- **Attachments.** A file goes to the server through ssh running `nexus-receive`, a child process. The in-app ssh
  runs only the link's commands, so attaching a file, or pasting text long enough to become one, says that it is not
  available on iOS yet; nothing is lost.

## Where it keeps things

The app's home is `Library/Application Support/.nexus/client` inside its own container (`src/client_home.h`): HOME on
iOS is the container, whose top an app may not write. It holds what the desktop window's `~/.nexus/client` holds.

## The overlay

An iOS app's window is the system's to show, so there is no overlay and no global hotkey: the app's window fills its
scene, on iPhone and on iPad. Command is the key left of the space bar on an iPad's keyboard, as on a Mac, so the
window's keys are the desktop's.

The window is laid out inside the safe area, clear of the status bar and the home indicator, and while the on-screen
keyboard shows, in the room above it, so the prompt sits at the safe area's foot or just above the keyboard. A tap
outside the prompt's box ends editing and lowers the keyboard, as in other apps; a drag or a scroll, a tap in the
prompt's box and a tap on a tab do not, and the draft stays. A tap in the prompt's box, or a key typed, edits again.
