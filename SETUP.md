# Setting up nexus

This page is for the AI a person asked to "set up nexus". You build nexus from source on the person's machine, from
their own fork of it, and set it up. There is no installer, no signed package and no store.

**The goal**: the person runs no command, pastes nothing into a terminal and installs nothing; every step is yours.
The one thing only the person does is sign in, once, in nexus's own dialog, which you never see.

**What is true today**, and what is not yet, so that you can tell the person plainly:
- True: every step on this page is yours, on macOS and on Linux or WSL2 (where only the server builds), and none needs
  an administrator's password. The server runs as a per-user service; a rebuild hands over to the new build without
  losing a session, and puts the previous build back when the new one does not start.
- Not yet, each a row of `docs/ROADMAP.md` being worked on (details in "Not yet", at the end):
  - the window's friend tabs come from nova-tools, so on a fresh install the window shows no friend tab, and the
    sign-in dialog does not show either (`window-friends-from-server`);
  - after a takeover the window does not reconnect by itself; reopening it does (`window-reattach-takeover`);
  - the sign-in dialog asks for a token the person makes with `claude setup-token`, the one command they still run
    (`t-accounts-sign-in`).

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
fork (`cd ~/nexus`, or `git -C <fork>`). First look:

```sh
git remote -v
git branch --show-current
```

Then, for the remotes:
- `origin` is robotgamedevelopers/nexus and there is no `upstream`: `git remote rename origin upstream`.
- There is no `upstream` and `origin` is anything else (the person's own fork on GitHub, a path on this machine, or
  none): leave `origin` as it is and add upstream:
  `git remote add upstream https://github.com/robotgamedevelopers/nexus.git`.
- There is an `upstream` already: leave the remotes as they are.

And for the branch: if there is no branch `mine` yet, make it from the branch the clone has checked out, whichever it
is (a fresh clone of upstream is on `main`; a clone the person gave you is on the branch they want to start from):
`git switch -c mine`. If there is one, `git switch mine`.

Check: `git remote get-url upstream` prints the upstream URL, and `git branch --show-current` prints `mine`.

If the person wants their fork on GitHub as well (a backup, or to share it), that makes a repository under their
account: ask them first, then make it with their `gh` login and add it as `origin`. A fork that lives only on this
machine is a whole fork; nothing below needs GitHub.

**Updates from upstream**, whenever the person asks for them (and never on your own): fetch and merge on `mine`,
then rebuild (`~/.nexus/bin/nexus-server rebuild`, section "Changing nexus"):

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

**Node** (for the Claude driver): `command -v node` and `node --version`. Use the node on PATH when it is 22.13 or
newer (or 20.19 or newer in the 20 line) and lives where it stays: Homebrew's (`/opt/homebrew/bin`, `/usr/local/bin`),
the system's (`/usr/bin`), or a version manager's in the home directory. Setup writes that node's full path into the
drivers file, so a node inside a project, a build tree or a temporary directory is a poor choice. Otherwise (missing,
older, or in such a place) fetch one into `~/.nexus/node`, where `nexus-server` looks for it (no administrator
needed):
```sh
mkdir -p ~/.nexus/node && curl -fsSL https://nodejs.org/dist/v22.20.0/node-v22.20.0-darwin-arm64.tar.gz | tar -xz --strip-components=1 -C ~/.nexus/node
mkdir -p ~/.nexus/node && curl -fsSL https://nodejs.org/dist/v22.20.0/node-v22.20.0-linux-x64.tar.xz | tar -xJ --strip-components=1 -C ~/.nexus/node
```
(the first on macOS on Apple silicon; `darwin-x64` on an Intel Mac; the second on Linux, `linux-arm64` on arm). Then
put it first on PATH in each shell command that runs `npm` yourself, so that it is the one used:
`PATH="$HOME/.nexus/node/bin:$PATH"`. `nexus-server rebuild` does that by itself.

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

## 5. Test and set up

From the fork:

```sh
build/nexus-server rebuild
```

It builds again (nothing to do after step 4), runs the fast tests (`ctest -L unit`, and the Claude driver's), and only
once they pass marks the build tested (`build/nexus-server.tested`) and runs its setup. setup refuses a build with no
such mark, so a build whose tests failed is never placed; `build/nexus-server setup` on its own is for setting up again
what is already tested. Each prints one line per step: `ok`, `done`, `fixed`, `moved`, `start`, `drain`, `stop`,
`note`, or `FAIL` with what failed and what to do. Running it again changes nothing that is right.
`build/nexus-server help setup` and `help rebuild` have every step.

Before it changes anything, setup tries the build: it starts it as a server on an empty home of its own under `/tmp`
(`/tmp/nexus-probe.*`, removed after), and refuses a build that does not answer there. Then it places, under
`~/.nexus`:
- the server at `~/.nexus/bin/nexus-server`, keeping the one there before as `nexus-server.previous`;
- the Claude driver at `~/.nexus/drivers/claude@<its build>`, beside the one running; `~/.nexus/drivers/claude` links
  to the driver of the build that runs, and each placed server runs the driver it was placed with;
- a first drivers file, `~/.nexus/server/drivers`, with one friend, `claude`, on the account `main` (an existing one is
  kept; only paths into the fork move to the placed driver, the old file kept as `drivers.before-setup`).

And outside `~/.nexus`, each one undone by "Removing nexus" below (lingering, a setting of the user's, excepted):
- the link `~/.local/bin/nexus-server` to the placed server (an older file there is moved to
  `~/.local/bin/nexus-server.before-setup`), where a login shell over ssh finds it;
- on macOS, the window at `~/Applications/nexus.app`;
- the per-user service that keeps the server running across logout and reboot, which needs no administrator: on macOS
  the launchd agent `~/Library/LaunchAgents/com.robotgamedevelopers.nexus-server.plist`, loaded into the person's
  session; on Linux the systemd user unit `~/.config/systemd/user/nexus-server.service`, enabled, and lingering turned
  on for the user (`loginctl enable-linger`) when it is off, so the server runs from boot and after the last logout.
  With HOME set to a directory other than the user's own, the name ends in a hash of it (`-1a2b3c4d`), so that a
  second install never replaces the first, and on Linux the unit is linked into the user's own
  `~/.config/systemd/user`.

Then the takeover (section "Takeover"): setup starts a handover process of its own, which brings the running server to
the new build, and watches it for up to 30 seconds (`--wait`). The ends, and setup's exit:
- `ok    the server runs the placed build` and `setup: done`, exit 0.
- `FAIL  the placed build ... did not come up on the store ..., so the handover put the previous build ... back`: the
  new build crashed or never answered once started on the person's store, and the previous one runs again. Exit 1; the
  server log, `~/.nexus/server/server.log`, says why. Fix it in the fork and rebuild.
- `note  the handover still runs, in the background` and `setup: placed; the handover finishes in the background`,
  exit 3: the old server is draining (a friend's turn is open on it). The handover finishes by itself, with a service
  or without one; `~/.nexus/bin/nexus-server doctor` says later how it ended.

Two things setup writes in full, as they are when it runs:
- The drivers file names node by its full path: the first node 20.19 or newer on PATH, else `~/.nexus/node/bin/node`
  (step 3 says which to put first). If that node later goes away, the friend's driver cannot start, and doctor says
  which line of the drivers file to change.
- The service runs the server with a PATH setup computes, never the shell's: node's directory, cmake's when the build
  was configured with one by full path, `~/.local/bin`, then Homebrew's and the system's directories that exist. The
  drivers inherit it.

On Linux without a systemd user manager (some WSL2 distributions, some containers), setup says so in a `note` and
registers no service; the handover starts the server itself, and `nexus-server new`, which is what the window runs,
starts it again after a reboot.

## 6. Check

```sh
~/.nexus/bin/nexus-server doctor
```

One line per check: `ok`, a `note` (something only the person does: the sign-in), or `fix <what>: <command>`. Run each
fix command it gives, then doctor again, until it ends `doctor: nothing to fix`. It also says how the last handover
ended: a rollback is a `fix` line naming the build that did not come up. `nexus-server help doctor` lists the checks.

## 7. Tell the person

Say, in your words: nexus is built from their fork and running. On macOS: open nexus (Spotlight: "nexus", or
`~/Applications`); Command-backtick (Command with the key left of 1, the one labelled ` and ~) shows and hides it.
nexus asks them to sign in to their Claude subscription in its own small dialog; that is the one thing they do, and no
AI sees it. From then on they change nexus by asking you (section "Changing nexus"). Then tell them, plainly, which
items of "Not yet" below still apply to them.

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
   `git config user.name "<the person's name or yours>"` and `git config user.email "<an address they give>"`; without
   one, a noreply address such as `nexus@localhost` will do for a fork that lives only on this machine.
3. Rebuild:
   ```sh
   ~/.nexus/bin/nexus-server rebuild
   ```
   (`--jobs <n>` keeps it to n cores on a machine others share.) It builds the fork (and the Claude driver), runs the
   fast tests, and only when they pass places the new build and hands over to it. A build or test that fails stops
   there with `FAIL`, and what runs is not changed: fix it and run rebuild again. Its exit is setup's (step 5): 0 the
   new build runs; 3 the handover finishes in the background, which is what a rebuild asked from inside a friend's
   turn always sees, since the server drains only once that turn ends; 1 a failure, or a rollback. After a 3, run
   `~/.nexus/bin/nexus-server doctor` in a later turn to see how it ended. `nexus-server help rebuild` has the steps.
4. When the change is in the window, the person quits nexus and opens it again to get the new one (Command-Q, then
   Spotlight).

To undo a change: `git revert <commit>` on `mine`, then rebuild.

## Takeover

A new build takes over from the running server without losing a session. setup (and so rebuild) starts
`nexus-server handover`, detached, so that the handover finishes whether or not setup is still there to watch it. The
handover asks the running server for its build (the hash of its program's bytes and its driver's). The placed build
already running: done. Another build: it is asked to drain, and stops as soon as no friend's turn is open, so no turn
is cut (after 10 minutes it stops anyway, and a turn still open is cut with a notice); it exits with status 75. No
server: the placed one is started, by the service when there is one and by the handover itself when there is none. One
server holds the store at a time, so a turn never runs on two.

When the placed build does not answer within 30 seconds of being started (it crashes as it opens the person's store,
say), the handover puts `nexus-server.previous` and its driver back and starts that, so the person's AI is never left
without a server, and says so in `~/.nexus/server/handover` (`rolled-back <new> <previous>`) and its log,
`~/.nexus/server/handover.log`; setup and doctor then say so too.

Two kinds of server are stopped at once instead, since they cannot drain: a server from before takeover (it does not
answer `build`), and one of the older layout, whose home was `~/.nexus` itself rather than `~/.nexus/server` (setup
adopts its sessions first). A turn open on one of them is cut, on that first upgrade only; every takeover after it
drains.

`nexus-server help takeover` says the same; the state machine is `models/Takeover.tla`.

## Removing nexus

Only when the person asks. Stop the service and remove its file, with the name setup and doctor print. On macOS:

```sh
launchctl bootout gui/$(id -u)/com.robotgamedevelopers.nexus-server
rm ~/Library/LaunchAgents/com.robotgamedevelopers.nexus-server.plist
```

On Linux (`disable` also removes the link a HOME other than the user's own made):

```sh
systemctl --user disable --now nexus-server.service
rm ~/.config/systemd/user/nexus-server.service
```

That stops the server and keeps it from starting again. Lingering, if setup turned it on (its line said "lingering is
on"), is a setting of the user's, which their other user services may need, so removing nexus leaves it as it is; it
is the person's to change. Then `~/Applications/nexus.app`, `~/.nexus/bin`, `~/.nexus/drivers` and the link
`~/.local/bin/nexus-server` can go (and `~/.local/bin/nexus-server.before-setup` back in its place, if setup moved one
there). `~/.nexus/server` holds the person's sessions, accounts and logins, and the fork holds their changes: never
delete either unless the person asks for exactly that.

## Not yet

What a fresh install does not do yet, so that you can say so plainly rather than work around it:

- The window takes its friends from `nova-config friend list`, not from the server's `friends.json`, so on a machine
  without nova-tools the window shows no friend tab yet, and since no session then starts a driver, the sign-in dialog
  does not show either; the server side of the one friend is in place (`docs/ROADMAP.md`,
  `window-friends-from-server`). Terminal tabs (Command-T) work without it.
- The sign-in dialog asks for the account's token, and today that token comes from `claude setup-token`, which the
  person runs themselves (a nexus terminal tab, Command-T, will do) and pastes into the dialog. That is the one place
  the person still runs a command; never run it for them, since it prints the token. The in-app sign-in that needs no
  token is roadmap 5c (`t-accounts-sign-in`).
- After a takeover the window does not reconnect by itself; reopening it does (`window-reattach-takeover`).
- On Linux only the server builds: Prax has no Linux window at its pin (`linux-window`). The Windows window builds
  only at a later Prax pin; on Windows the server runs in WSL2 (`AGENTS.md`, platform targets).
