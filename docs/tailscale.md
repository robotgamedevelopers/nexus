# Tailscale built in

nexus reaches its servers over the person's own tailnet with a Tailscale node inside the nexus process: tsnet
(`tailscale.com/tsnet`), the Go library that runs a node with userspace networking. No Tailscale app, no TUN device,
no admin password. nexus owns a small Go shim over tsnet with a C API (`src/tailscale/nts.h`), the same Go code on every
OS. It is built in layers, bottom up, each secured before the next starts (the layer list is at the end of this page
and in `docs/roadmap.sexp`, area `tailscale`).

Why a shim of nexus's own rather than libtailscale: libtailscale does not build for Windows (its C side includes
`<sys/socket.h>`, and its Go side hands file descriptors over `socketpair` and `SCM_RIGHTS`), and it gives no hook for
the log line that carries the sign-in URL, which it writes to stderr.

## Layer 0: Build (this layer)

Layer 0 is the build and nothing above it: the shim pinned and built on macOS, Linux and Windows, linked into a test
program on each, built and tested in CI, with its size and the licences of everything it links stated. Its C API is the
minimum that proves the build links and runs: the version, preparing a state directory, and starting and closing a
node. The real API is layer 1. Nothing in nexus links the shim yet: the top-level build has it off
(`-DNEXUS_TAILSCALE=OFF` by default), and `src/tailscale/CMakeLists.txt` builds it on its own, without Prax.

### Pins

| What | Pin | Where it is held |
|---|---|---|
| tailscale.com | v1.104.1 | `src/tailscale/go.mod`, with every module's hash in `src/tailscale/go.sum`; builds use `-mod=readonly`, so nothing else is fetched |
| Go toolchain | go1.27.1 | `go 1.27.1` in `go.mod`, and `GOTOOLCHAIN=go1.27.1` on every Go command the build runs (`cmake/tailscale.cmake`), so any Go from 1.21 on fetches exactly go1.27.1 into the module cache and verifies it against the checksum database; a newer local Go is not used |
| llvm-mingw (Windows only) | 20261020 (LLVM 23.1.4), UCRT x86_64, sha256 `fd5462b7...ebd74cd` | `cmake/tailscale.cmake`; cgo needs a gcc-style C compiler, which MSVC is not, so the DLL's C parts are compiled with it. Fetched and checked by the configure step, never installed system-wide |

Every Go command also runs with `GOENV=off` (no Go environment file is read) and `GOWORK=off`, and builds with
`-trimpath -buildvcs=false`. `nts_version()` reports the pins from the build's own record (`runtime/debug`), and the
unit test holds it to `tailscale.com v1.104.1 go1.27.1`.

### What holds from the first commit

- **No log upload.** The shim calls `envknob.SetNoLogsNoSupport()` when it loads, before any tsnet code runs. It sets
  `TS_NO_LOGS_NO_SUPPORT=true` in the process's environment, and tsnet's log transport then sends nothing (read:
  `logpolicy`, `TransportOptions.New`). tsnet still buffers its own log in `<dir>/tailscaled.log*.txt`, inside the
  owner-only state directory.
- **No log on stderr.** tsnet's user log (`UserLogf`, which carries the sign-in URL) goes to `<dir>/nexus-tailscale.log`,
  opened for appending and made 0600. Go's `log` package writes to the newest running node's file, and to nowhere
  while no node runs.
- **An owner-only state directory.** `nts_prepare` and `nts_start` create the state directory and its missing parents
  0700 (an existing one this user owns is made exactly 0700; one another user owns is refused). On Windows the
  directory gets a protected ACL with one entry, full control for the current user, inherited by what is inside it.
  It holds the node's keys (`tailscaled.state`).
- **Interactive sign-in only.** tsnet reads an auth key, an OAuth client, workload identity and another control server
  from the environment when its own fields are empty (`TS_AUTHKEY`, `TS_AUTH_KEY`, `TS_CLIENT_SECRET`, `TS_CLIENT_ID`,
  `TS_ID_TOKEN`, `TS_AUDIENCE`, `TS_CONTROL_URL`). `nts_start` refuses while any of them is set, as Go or the C
  library sees the environment, naming the variable and never its value.
- **The sign-in URL is a secret**: anyone who opens it can claim the node. It is written only to the log file above:
  never printed, never in CI's logs, never committed, never shown to an AI.

### The C API (layer 0)

`src/tailscale/nts.h`: `nts_version`, `nts_prepare(dir)`, `nts_start(dir, hostname)`, `nts_backend_state(node)` and
`nts_close(node)`. Every call that can fail writes its reason and the fix into the caller's buffer.
`nts_backend_state` names the node's state as tsnet does (`NeedsLogin`, `Running`, ...); it is there so that a start
can be seen to reach "needs login" by hand, and is replaced by layer 1's state watch.

### How it builds

`cmake/tailscale.cmake` runs `go build` on `src/tailscale/shim` with the pins above:

| OS | Output | C compiler for cgo | Linked as |
|---|---|---|---|
| macOS | `libnts.a`, a c-archive per architecture in `CMAKE_OSX_ARCHITECTURES` (arm64, x86_64, or both joined by `lipo`), built for macOS 26.0 like the rest of nexus | the configured C compiler | static, with CoreFoundation, Security and libresolv |
| Linux (and WSL2) | `libnts.a`, a c-archive, x86_64 | the configured C compiler (clang in CI) | static, with the thread library |
| Windows x64 | `nts.dll` (stripped, `-s -w`) and `nts.lib`, its import library, made from `src/tailscale/nts.def` by MSVC's `lib` | llvm-mingw's clang | through `nts.lib`; `nts.dll` is copied beside the program |

A program links the shim with `nexus_link_tailscale(<target>)`, which on Windows also embeds
`cmake/windows10.manifest`: it declares Windows 10, without which tsnet panics at start ("incoherent Windows
version").

```sh
cmake -S src/tailscale -B build-tailscale -DCMAKE_BUILD_TYPE=Release        # add "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" on macOS, -A x64 on Windows
cmake --build build-tailscale --target tailscale_test
ctest --test-dir build-tailscale -L tailscale                                  # -C Release on Windows
cmake --build build-tailscale --target nexus_tailscale_vet                     # go vet
cmake --build build-tailscale --target nexus_tailscale_notices_check           # src/tailscale/NOTICES.md is current
```

Or from the top level: `cmake -S . -B build -DNEXUS_TAILSCALE=ON`. The caches are cache variables, so that CI keeps them
outside its workspace: `NEXUS_GO_MODCACHE` (modules and the toolchain), `NEXUS_GO_BUILDCACHE` (Go's build cache) and,
on Windows, `NEXUS_TAILSCALE_DOWNLOADS` (llvm-mingw) or `NEXUS_LLVM_MINGW` (an unpacked one). Each defaults to a
directory under the build tree.

CI (`.github/workflows/ci.yml`) has one job per OS, beside the others and in parallel with them: `tailscale-macos`
(arm64 and x86_64, the x86_64 slice run under Rosetta), `tailscale-linux` (with the licence check) and
`tailscale-windows` (captain). Each configures `src/tailscale` alone, builds, vets and runs `ctest -L tailscale`. Go's
caches are the machine's, shared by its runners (Go locks both).

#### Build times

The shim's Go build on its own (`cmake --build --target tailscale_test`, the step a job spends its time in), measured
on 2026-11-15. Cold is empty Go caches (the toolchain and every module downloaded, tsnet compiled); warm is a fresh build
directory over kept caches, as a CI job after the first on a machine.

| Where | Cold | Warm |
|---|---|---|
| macOS arm64 (studio, both architectures, 4 jobs) | 34.5 s | 5.1 s (an edit to the shim rebuilt: both architectures relinked) |
| Linux x86_64 (amd-1, 4 CPUs) | 63.0 s | 3.3 s |
| Windows x64 (captain, 4 CPUs) | see below | 13.0 s (the test program only, shim cached) |
| CI | CI_TIMES | |

On Windows the configure step fetches and unpacks llvm-mingw (190 MB) once per runner: 27 s on captain, inside the
configure that measured.

### Tests

`tests/tailscale` (`ctest -L tailscale`; each test is labelled unit or functional as well):

- `tailscale_unit_version` (unit): the versions are the pins.
- `tailscale_functional_state_dir`: on the real filesystem, in a directory of the test's own: the directory and its
  missing parents created 0700 and the log file 0600 (on Windows: a protected ACL granting only this user, and the log
  file inheriting it); an existing directory and log file open to others made owner-only; a second prepare keeps the
  log.
- `tailscale_functional_refusals`: a null, empty or relative path; a file in the way; a parent this user cannot write
  to; a start while each of tsnet's sign-in variables is set (named, its value never shown); a start with no hostname;
  a handle that is not a node; the reason cut to the caller's buffer.
- `tailscale_functional_refuses_inherited_key`: with `TS_AUTHKEY` in the environment from the process's start, a start
  refuses.

No test uses a socket, the network or the clock, and nothing in CI starts a node, since a start contacts Tailscale's
control server. That is `tailscale_test start_needs_login <scratch dir>`, by hand on a bench only: it starts a node,
waits until it asks to sign in, prints the backend state (never the URL) and closes it. It ran once, on captain's
Windows (the Windows 10 manifest in place), on 2026-11-15: `backend state NeedsLogin`; the scratch directory was then
removed, and nobody approved the URL.

### Binary size

Measured on 2026-11-15, Release builds. "With the shim" links it whole, as layer 2 will (the probe forced
`nts_start` in; Go has already dropped what the exports do not reach). Sizes in bytes; MB is 10^6 bytes.

| OS | Program | Without the shim | With the shim | Cost |
|---|---|---|---|---|
| macOS arm64 | nexus-server | 1,313,488 (stripped 1,136,000) | 24,935,680 (stripped 21,210,760) | +23.6 MB (stripped +20.1 MB) |
| macOS arm64 | nexus (the client) | 10,818,008 (stripped 9,622,120) | 34,437,624 (stripped 29,694,304) | +23.6 MB (stripped +20.1 MB) |
| macOS x86_64 | tailscale_test (the shim and a test of under 100 KB) | | 25,209,392 (stripped 21,649,616) | about +25.2 MB (stripped +21.6 MB) |
| Linux x86_64 | tailscale_test | | 46,168,088 (stripped 24,991,176) | about +46.1 MB with Go's debug information, +25.0 MB stripped |
| Windows x64 | tailscale_test.exe + nts.dll | | 99,840 + 22,434,304 | +22.4 MB (the DLL, built stripped) |

nexus-server and the client build only on macOS today (Linux has no window and, at Prax's pin, no window logic or
link, so no nexus-server; Windows builds neither natively: CMakeLists.txt says why), so elsewhere the cost is the test
program's. The macOS arm64 test program is 23,699,776 bytes, the same as nexus-server's cost, which bears that out. A
universal macOS build carries both architectures, about twice the cost.

### Licences

`src/tailscale/NOTICES.md` lists every Go module the shim links on darwin/arm64, darwin/amd64, linux/amd64 and
windows/amd64, with the licence its licence files state, measured by `go list -deps ./shim` per platform
(`src/tailscale/notices`, which fails on a licence it cannot name or one nexus may not link). At these pins: 42 modules,
BSD-3-Clause 19, MIT 12, Apache-2.0 5, BSD-2-Clause 3, ISC 1, and 2 whose licence file carries Apache-2.0, MIT and
BSD-3-Clause together (klauspost/compress, gvisor); plus the Go standard library and runtime, BSD-3-Clause. Every one
may be combined with nexus's AGPL-3.0. CI fails when the file differs from what the program writes.

### Not seen headless: the first-run OS prompts (a layer 2 check)

Two prompts may show the first time a node runs, and only on a desktop session; from ssh neither can be seen, and
nothing here clicks through either. Declining either costs direct paths, not function: DERP relays still connect.

- **macOS, Local Network** ("nexus would like to find and connect to devices on your local network"): magicsock's LAN
  probing may raise it. To observe: on a Mac with a logged-in desktop (the studio), start a node of a freshly built,
  never-run program from the desktop session (`tailscale_test start_needs_login <scratch dir>` from Terminal, which
  macOS attributes to Terminal, and later the nexus app itself, attributed to nexus), and record whether the prompt
  shows, for which program, and what System Settings > Privacy & Security > Local Network then lists. Then check that a
  node with the permission declined still reaches `Running` and its peers through DERP.
- **Windows, Firewall** ("Windows Defender Firewall has blocked some features of this app"): a listening UDP socket
  (magicsock's) may raise it. To observe: on captain, logged in at its desktop, run the test program from a desktop
  terminal on a scratch state directory, and record whether the prompt shows, which networks it offers (private,
  public), and what rule `Get-NetFirewallApplicationFilter` shows for the program afterwards. Then check a node with the
  prompt declined still reaches `Running` through DERP.

Both are layer 2's (client sign-in), where the client first starts a node on a person's machine, and are owed there.

## The layers

Bottom up, each secured before the next starts. **Sign-in is path A, interactive only: the owner's choice.**

0. **Build.** The shim pinned to tailscale.com v1.104.1 and Go 1.27.1; CMake builds a c-archive (macOS arm64 and
   x86_64, Linux x86_64 and WSL2) and a DLL with an import library (Windows MSVC); the Windows program gets the
   manifest; CI builds it on all three; the binary-size cost is stated. *This page, above.*
1. **Shim API.** Start and close, a state watch (`WatchIPNBus`: needs login with its URL, running, expired), peers,
   WhoIs, TCP listen and dial, UDP listen and dial; logs to the file, uploads off, the state directory owner-only; a
   TLA+ model of the node's lifecycle with a trace check.
2. **Client sign-in.** The URL to the system browser, wait for running, keep the state (Prax later); the first-run OS
   prompts above observed.
3. **Server enrolment.** **Path A, interactive only (chosen):** the server's node (`~/.nexus/server/tailscale/`) hands
   its sign-in URL to the client, over the pipe the client spawned it on for a local server (macOS, WSL2), and the
   client opens it in the browser that is already signed in: one Connect click per server, and approval again when
   an untagged node expires (180 days). A remote server's first start shows its URL there. (Path B, an OAuth client
   secret captured once so that servers join with no click, was not chosen.)
4. **Bootstrap over the tailnet.** nexus-server as a per-user service; a tailnet port answers `NEXUS CONNECT <port>
   <key>` only to callers whose WhoIs is the server's own login; the link model extended.
5. **mosh over tsnet.** mosh unmodified, through an in-process loopback relay between its 127.0.0.1 socket and the
   shim's tailnet UDP (a layout model if the relay keeps rings); the added latency measured in ms against the 8.3 ms
   frame.
6. **Discovery** of nexus servers (`nexus-server-<host>`) among the tailnet's peers.
7. **Expiry and recovery** shown in the app. ssh retires once layers 4 to 6 are secured.
