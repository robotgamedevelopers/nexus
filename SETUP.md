# Setting up nexus

This page is for the AI a person asked to "set up nexus". You build nexus from source on the person's machine, from
their own fork of it, and set it up. The person runs no command, pastes nothing into a terminal and installs nothing:
every step below is yours. The one thing only the person does is sign in, once, in nexus's own dialog, which you
never see. There is no installer, no signed package and no store.

Read it top to bottom once before starting. Each step says how to check it worked and what to do when it did not.

## The rules while you do this

- Never ask the person to run a command, paste a value or click through an installer. If a step needs something only
  an administrator can do and passwordless `sudo -n` does not work, stop and tell the person in one plain line what is
  missing; do not ask for their password.
- Never handle a secret: no token, key or password goes through you, into a file you write, or into your output. The
  person signs in in nexus's own dialog (`docs/secrets.md`).
- Never open the nexus window, take screenshots of it, or kill a running nexus process (`AGENTS.md`). You place the
  window; the person opens it.
- If there is already a `~/.nexus`, it is the person's: setup keeps their sessions, accounts and drivers file, and
  never deletes anything there.

## 1. Where you are

```sh
uname -sm
```

`Darwin arm64` or `Darwin x86_64`: macOS, which builds the server and the window. `Linux x86_64` (or `aarch64`): Linux,
which builds the server; this includes WSL2 on Windows (`uname -r` contains `microsoft`), where the server runs for a
Windows window. On Linux, the window itself is not built yet: the person's window runs on a Mac and opens on this
machine's server (step 8).

## 2. The fork

Each person's nexus is their own fork. Upstream nexus is a remote the fork pulls from; the person's changes (made by
you, from what they ask for) live on a branch of their own, so that pulling upstream never loses them.

If the person pointed you at a clone that is already on this machine, use it where it is. Otherwise clone upstream
to `~/nexus` (anywhere works; `nexus-server` remembers where its source is):

```sh
gh repo clone robotgamedevelopers/nexus ~/nexus
```

(`git clone https://github.com/robotgamedevelopers/nexus.git ~/nexus` does the same without `gh`.)

Then make it a fork: upstream is the remote `upstream`, and the person's work is on the branch `mine`. Run these in the
fork (`cd ~/nexus`, or `git -C <fork>`):

```sh
git remote -v
git remote rename origin upstream          # when origin is robotgamedevelopers/nexus and there is no upstream yet
git remote add upstream https://github.com/robotgamedevelopers/nexus.git   # when there is no upstream at all
git switch -c mine                          # when on main; stay on the branch you are on otherwise
```

Check: `git remote get-url upstream` prints the upstream URL, and `git branch --show-current` is not `main`.

If the person wants their fork on GitHub as well (a backup, or to share it), that makes a repository under their
account: ask them first, then make it with their `gh` login and add it as `origin`. A fork that lives only on this
machine is a whole fork; nothing below needs GitHub.

**Updates from upstream**, whenever the person asks for them (and never on your own): fetch and merge, then rebuild
(step 7, `nexus-server rebuild`):

```sh
git fetch upstream
git merge --no-edit upstream/main
```

A merge keeps every local commit. When it conflicts, resolve each file keeping what the person's change was for, run
the build and the tests, then commit the merge. Never `git reset --hard`, never rebase or force-push `mine`.

## 3. Prerequisites

Check each; fix what is missing as it says. Nothing here needs the person.

**git**: `git --version`. On macOS it comes with Xcode (below).

**cmake 3.28 or newer**: `cmake --version`.
- macOS with Homebrew: `HOMEBREW_NO_AUTO_UPDATE=1 brew install cmake`.
- macOS without Homebrew, a copy in your own folder (no administrator needed), then use
  `~/.nexus/cmake/CMake.app/Contents/bin/cmake` wherever this page says `cmake`:
  ```sh
  mkdir -p ~/.nexus/cmake && curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-macos-universal.tar.gz | tar -xz --strip-components=1 -C ~/.nexus/cmake
  ```
- Linux: with the compilers, below.

**Node 20.19 or newer** (for the Claude driver): `node --version`. If it is missing or older, fetch it into
`~/.nexus/node`, where `nexus-server` looks for it (no administrator needed):
```sh
mkdir -p ~/.nexus/node && curl -fsSL https://nodejs.org/dist/v22.12.0/node-v22.12.0-darwin-arm64.tar.gz | tar -xz --strip-components=1 -C ~/.nexus/node
mkdir -p ~/.nexus/node && curl -fsSL https://nodejs.org/dist/v22.12.0/node-v22.12.0-linux-x64.tar.xz | tar -xJ --strip-components=1 -C ~/.nexus/node
```
(the first on macOS on Apple silicon; `darwin-x64` on an Intel Mac; the second on Linux, `linux-arm64` on arm). Then
put it first on PATH in each shell command that runs `npm`: `PATH="$HOME/.nexus/node/bin:$PATH"`. `nexus-server
rebuild` does that by itself.

**macOS: Xcode and its Metal toolchain** (the window's shaders): `xcode-select -p` names Xcode
(`/Applications/Xcode.app/...`) and `xcrun -f metal` prints a path. Without the Metal toolchain:
```sh
xcodebuild -downloadComponent MetalToolchain
```
Without Xcode at all, stop and tell the person: "nexus's window needs Xcode, from the App Store; once it is installed,
tell me and I will go on." That is the one prerequisite you cannot fetch for them.

**macOS: protobuf** (mosh's transport): Homebrew's if it is there (`brew install protobuf`); without it, the build
fetches and compiles protobuf itself, which adds about a minute to the first build. Nothing to do.

**Linux: compilers and libraries** (clang and lld, as nexus's CI builds; zlib and OpenSSL; protobuf, optional, saves
building it):
```sh
sudo -n apt-get install -y --no-install-recommends git cmake clang lld zlib1g-dev libssl-dev libprotobuf-dev protobuf-compiler
```
If `sudo -n` says a password is required, check whether they are there already (`clang --version`, `ld.lld --version`,
`cmake --version`, `dpkg -s zlib1g-dev libssl-dev`); if one is missing, stop and tell the person which, in one line.
On another distribution, its own package names.

## 4. Build

In the fork. On macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(sysctl -n hw.ncpu)"
```

On Linux (and WSL2):

```sh
CC=clang CXX=clang++ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEXUS_LINK=ON -DCMAKE_LINKER_TYPE=LLD
cmake --build build -j "$(nproc)"
```

The first build compiles Prax, the UI library, and takes several minutes; later ones are incremental. Warnings are
errors: a warning that stops the build is a bug to report upstream, not one to silence. Check: `build/nexus-server help`
prints the usage; on macOS `build/nexus.app` is there too.

If the configure fails with `patch does not apply` (a configure run again after `cmake` itself was replaced), remove
`build` and configure again.

The Claude driver, in `drivers/claude` of the fork:

```sh
npm ci --no-audit --no-fund
npm run build
```

Check: `drivers/claude/dist/src/main.js` is there.

## 5. Set up

From the fork:

```sh
build/nexus-server setup
```

It places the server at `~/.nexus/bin/nexus-server` (linked from `~/.local/bin/nexus-server`), the Claude driver at
`~/.nexus/drivers/claude`, and on macOS the window at `~/Applications/nexus.app`; writes a first drivers file with one
friend, `claude`, on the account `main`; registers a per-user service that keeps the server running across logout and
reboot (a launchd agent on macOS, a systemd user unit with lingering on Linux; neither needs an administrator); and
starts the server, or takes over from one already running (section "Takeover" below). It prints one line per step and
ends with `setup: done`; a `FAIL` line says what failed and what to do. Running it again changes nothing that is right.
`build/nexus-server help setup` has every step.

On Linux without a systemd user manager (some WSL2 distributions, some containers), setup says so in a `note` and
registers no service; the server is then started on demand by `nexus-server new`, which is what the window runs.

## 6. Check

```sh
~/.nexus/bin/nexus-server doctor
```

One line per check: `ok`, a `note` (something only the person does: the sign-in), or `fix <what>: <command>`. Run each
fix command it gives, then doctor again, until it ends `doctor: nothing to fix`. `nexus-server help doctor` lists the
checks.

## 7. Tell the person

Say, in your words: nexus is built from their fork and running. On macOS: open nexus (Spotlight: "nexus", or
`~/Applications`); Command-backtick (the key left of 1) shows and hides it. The first time, nexus asks them to sign in
to their Claude subscription in its own small dialog; that is the one thing they do, and no AI sees it. From then on
they change nexus by asking you (section "Changing nexus").

Do not open the window yourself.

## 8. A server on another machine

The window opens on the server its config names: setup writes `server local` (this machine) into
`~/.nexus/client/config`. To use a server on another machine (a Linux box, WSL2, an always-on Mac), set it up there by
this page, then on the Mac with the window change that line to `server <host>`, where `ssh <host>` works without a
password prompt; the window runs `nexus-server new` there over ssh and then talks to it over UDP (`README.md`, "The
server").

## Changing nexus

The person asks for a change; you make it in their fork and rebuild. This works from inside a nexus conversation too.

1. Edit the source in the fork, on `mine`, following `AGENTS.md` (the rules for any change to nexus).
2. Commit it. If git has no identity there, give the fork one of its own, never a global one:
   `git config user.name "<the person's name or yours>"` and `git config user.email "<an address they give, or a noreply one>"`.
3. Rebuild:
   ```sh
   ~/.nexus/bin/nexus-server rebuild
   ```
   It builds the fork (and the Claude driver), runs the fast tests, and only when they pass places the new build and
   hands over to it. A build or test that fails stops there with `FAIL`, and what runs is not changed: fix it and run
   rebuild again. `nexus-server help rebuild` has the steps.
4. When the change is in the window, the person quits nexus and opens it again to get the new one (Command-Q, then
   Spotlight).

To undo a change: `git revert <commit>` on `mine`, then rebuild.

## Takeover

A new build takes over from the running server without losing a session. `setup` (and so `rebuild`) asks the running
server for its build (the hash of its program's bytes). The placed build already running: done. Another build: it is
asked to drain, and stops as soon as no friend's turn is open, so no turn is cut (after 10 minutes it stops anyway, and
a turn still open is cut with a notice); it exits with status 75 and the service starts the placed build on the same
store. A server from before takeover is stopped. No server: the placed one is started. One server holds the store at a
time, so a turn never runs on two. A rebuild asked from inside a friend's turn cannot see the handover until that turn
ends, so rebuild waits only 5 seconds and says the server is draining; the handover then happens by itself.
`nexus-server help takeover` says the same; the state machine is `models/Takeover.tla`.

## Not yet

What a fresh install does not do yet, so that you can say so plainly rather than work around it:

- The window takes its friends from `nova-config friend list`, not from the server's `friends.json`, so on a machine
  without nova-tools the window shows no friend tab yet; the server side of the one friend is in place
  (`docs/roadmap.sexp`, `window-friends-from-server`).
- The sign-in dialog asks for the account's token, which comes from `claude setup-token`; the in-app sign-in that needs
  no token is roadmap 5c (`t-accounts-sign-in`).
- After a takeover the window does not reconnect by itself; reopening it does (`window-reattach-takeover`).
- On Linux only the server builds: Prax has no Linux window at its pin. The Windows window builds only at a later Prax
  pin; on Windows the server runs in WSL2 (`AGENTS.md`, platform targets).
