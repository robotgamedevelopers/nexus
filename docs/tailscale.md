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
| tailscale.com | v1.104.7 | `src/tailscale/go.mod`, with every module's hash in `src/tailscale/go.sum`; builds use `-mod=readonly`, so nothing else is fetched |
| Go toolchain | go1.27.4 | `go 1.27.4` in `go.mod`, and `GOTOOLCHAIN=go1.27.4` on every Go command the build runs (`src/tailscale/pins.cmake`, used by `cmake/tailscale.cmake`), so any Go from 1.21 on fetches exactly go1.27.4 into the module cache and verifies it against the checksum database; a newer local Go is not used |
| llvm-mingw (Windows only) | 20261020 (LLVM 23.1.4), UCRT x86_64, sha256 `fd5462b7...ebd74cd` | `src/tailscale/pins.cmake`; cgo needs a gcc-style C compiler, which MSVC is not, so the DLL's C parts are compiled with it. Fetched and checked by the configure step, which keeps the archive's sha256 beside it and fetches again on a mismatch; never installed system-wide |

The pins moved on 2026-11-15 from tailscale.com v1.104.1 and go1.27.1 to the newest release of the 1.104 line, as
Tailscale's changelog and security bulletins list it that day: v1.104.7 (2026-11-12). v1.104.7 requires go1.27.4
(its `go.mod`), so the toolchain pin moved with it. What the line fixed after v1.104.1: v1.104.2, Go 1.27.3 for
distrusted TLS intermediate certificates (TS-2026-013), and Windows and macOS app fixes; v1.104.3, internal only;
v1.104.4, Tailscale SSH (TS-2026-014, TS-2026-015) and Serve (TS-2026-016, TS-2026-017) vulnerabilities, neither of
which the shim uses; v1.104.5, connection stalls on lossy networks that affected tsnet among others, and a Windows
installer privilege escalation; v1.104.6, a rare stall of dormant connections, and Windows and macOS fixes; v1.104.7,
Go 1.27.4 for a TLS ClientHello problem with some middleboxes on control connections, and a Kubernetes operator fix
(TS-2026-018). Modules that moved with it: wireguard-go and golang.org/x/crypto, net, sync, sys, term and text.

Every Go command also runs with `GOENV=off` (no Go environment file is read) and `GOWORK=off`, and with where modules
come from set rather than inherited: `GOPROXY=https://proxy.golang.org` (no `direct` fallback, so nothing is fetched
from a VCS), `GOSUMDB=sum.golang.org`, and `GOPRIVATE`, `GONOPROXY`, `GONOSUMDB` and `GOINSECURE` empty (nothing
exempt from the proxy or the checksum database), with `GOFLAGS=-mod=readonly`. Builds use `-trimpath -buildvcs=false`.
`nts_version()` reports the pins from the build's own record (`runtime/debug`), and the unit test holds it to
`tailscale.com v1.104.7 go1.27.4`.

The features of tailscale.com that a tsnet node signing in interactively does not use are left out by its own build
tags (`ts_omit_<feature>`, `tailscale.com/feature/featuretags`), listed in `src/tailscale/pins.cmake`
(`NEXUS_TAILSCALE_OMIT`) and passed to every Go command: the web client (and with it the embedded web UI and its
font), Taildrop, Taildrive, SSH, the CLI, client updates, AWS, Kubernetes, the system tray, capture, the relay server,
app connectors, the Linux DNS and firewall managers (resolved, NetworkManager, iptables, D-Bus), and the rest of that
list. Kept: netstack, the port mapper, DNS, logtail (uploads off), health and what tsnet itself needs.

### What holds from the first commit

- **No log upload.** The shim calls `envknob.SetNoLogsNoSupport()` when it loads, before any tsnet code runs. It sets
  `TS_NO_LOGS_NO_SUPPORT=true` in the process's environment, and tsnet's log transport then sends nothing (read:
  `logpolicy`, `TransportOptions.New`). tsnet still buffers its own log in `<dir>/tailscaled.log*.txt`, inside the
  owner-only state directory.
- **Nothing on stdout or stderr.** tsnet's user log (`UserLogf`, which carries the sign-in URL) goes to
  `<dir>/nexus-tailscale.log`, opened for appending and made 0600; past 1 MiB it moves to `nexus-tailscale.log.1`
  (replacing an older one) and a new one begins, so it takes at most 2 MiB (1 MiB is a guess, not a measurement).
  The two process-wide library loggers, Go's `log` package and gVisor's logger (which writes to stderr by default and
  which tailscale never redirects), are pointed in `init` at the newest running node's log file, and at nowhere while
  no node runs. tsnet's backend log goes to its own buffer, `<dir>/tailscaled.log1.txt` and `tailscaled.log2.txt`,
  which tsnet caps at 50 MB together. Not stoppable from the shim: a Go fatal error (out of memory, a deadlock, a
  concurrent map write) or a panic in one of tsnet's own goroutines prints to stderr and ends the whole process, as in
  any Go program.
- **A panic in a call is stopped there.** Every exported function recovers a Go panic in its own call: it returns its
  failure value with the reason in the caller's buffer, gives back what it held (the state directory, the log), and
  writes the panic's text and stack to the newest running node's log only.
- **An owner-only state directory.** `nts_prepare` and `nts_start` create the state directory and its missing parents
  0700 (an existing one this user owns is made exactly 0700; one another user owns is refused). On Windows the
  directory, and each parent created on the way, gets a protected ACL with one entry, full control for the current
  user, inherited by what is inside it; an existing directory's ACL is rewritten so whoever owns it, and refused only
  when this user may not change it. It holds the node's keys (`tailscaled.state`).
- **One node per state directory.** Two nodes on one directory would share its keys and state and corrupt both. A
  start, or a prepare, is refused while a node holds the directory, before anything in it is opened: in this process
  by a table of the directories held (by their cleaned path), and in any process by an OS lock on
  `<dir>/nexus-tailscale.lock` (`flock` on macOS and Linux, `LockFileEx` on Windows), held for the node's life and
  dropped by the OS if the process ends.
- **Interactive sign-in only.** tsnet reads an auth key, an OAuth client, workload identity and another control server
  from the environment when its own fields are empty (`TS_AUTHKEY`, `TS_AUTH_KEY`, `TS_CLIENT_SECRET`, `TS_CLIENT_ID`,
  `TS_ID_TOKEN`, `TS_AUDIENCE`, `TS_CONTROL_URL`). `nts_start` refuses while any of them is set, as Go or the C
  library sees the environment, naming the variable and never its value.
- **Every file in the state directory is secret**: never shared, attached to a bug report, committed, printed in CI's
  logs or shown to an AI. Besides the keys, the sign-in URL a node with no state asks for (anyone who opens it can
  claim the node) is written to `nexus-tailscale.log` and to tsnet's `tailscaled.log*.txt` buffers; it goes nowhere
  outside the directory.

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
version"). The shim is built twice: nexus's build (`nexus_tailscale`: `libnts.a`, or `nts.dll` and `nts.lib`) and the
test program's (`nexus_tailscale_testhooks`: `libnts_test.a`, or `nts_test.dll` and `nts_test.lib`, linked with
`nexus_link_tailscale(<target> TESTHOOKS)`), which adds the hooks of `src/tailscale/shim/hooks_on.go` behind the
`nts_testhooks` build tag (an injected panic, an offline start, a line through the library loggers;
`tests/tailscale/nts_test_hooks.h`). Nothing else differs, and nexus never links the test build.

```sh
cmake -S src/tailscale -B build-tailscale -DCMAKE_BUILD_TYPE=Release        # add "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" on macOS, -A x64 on Windows
cmake --build build-tailscale --target tailscale_test
ctest --test-dir build-tailscale -L tailscale                                  # -C Release on Windows
cmake --build build-tailscale --target nexus_tailscale_vet                     # go vet
cmake --build build-tailscale --target nexus_tailscale_notices_check           # src/tailscale/NOTICES.md is current
cmake --build build-tailscale --target nexus_tailscale_notices                 # rewrite it
```

Or from the top level: `cmake -S . -B build -DNEXUS_TAILSCALE=ON`. The caches are cache variables, so that CI keeps them
outside its workspace: `NEXUS_GO_MODCACHE` (modules and the toolchain), `NEXUS_GO_BUILDCACHE` (Go's build cache) and,
on Windows, `NEXUS_TAILSCALE_DOWNLOADS` (llvm-mingw) or `NEXUS_LLVM_MINGW` (an unpacked one). Each defaults to a
directory under the build tree.

CI has one job per OS (`.github/workflows/tailscale.yml`, called by `ci.yml` for every pull request and push to main),
beside the others and in parallel with them: `tailscale-macos` (arm64 and x86_64, the x86_64 slice run under Rosetta),
`tailscale-linux` (with the licence check) and `tailscale-windows` (captain). Each configures `src/tailscale` alone,
builds both builds of the shim and the test program, vets and runs `ctest -L tailscale`. On a pull request they check
out the head commit, not the merge commit the other jobs build.

**The verdict is a commit status.** Each OS's result is the status `tailscale-macos`, `tailscale-linux` or
`tailscale-windows` on the exact commit built: a pull request's head, or the pushed commit. Only a job that built and
tested that commit on that OS sets it, to success or failure (a cancelled or timed-out job sets failure). **A lander
waits for all three statuses to be success on the head it lands**; a missing or pending one is not a pass, whatever
the jobs show.

The Go toolchain, the modules, Go's build cache and llvm-mingw live on the runner's machine in a directory keyed by the
pins (`src/tailscale/pins.cmake`, `go.mod`, `go.sum`), which no other job cleans, so only a pin change is cold; each is
checked against its pin when used. A cold build does not fit a per-commit job's 2 minutes (the times below), so that one
case has its own workflow, `tailscale-prime.yml`. A per-commit job on a machine whose key directory is not yet primed
sets its status to pending (linked to tailscale-prime's runs), dispatches tailscale-prime for the same commit and OS,
and fails, since it built and tested nothing. The dispatched run is one slot, which GitHub places on any runner of that
OS (runners carry no label per machine, so it cannot be sent to the one that asked): it sets the status to pending with
a link to itself, builds and tests the commit with no time limit, primes the machine it ran on, and sets the status to
its result. So after a pin change a pull request shows its tailscale jobs red and its statuses pending, then the
statuses turn to the prime's result. A machine the prime did not reach hands off again at its next per-commit job. The
nightly (main, every OS) runs a slot per runner so that GitHub may place one on every machine: a slot on a machine
already primed marks its key directory used and builds nothing, and one on a new machine primes it; it sets no status.

#### Build times

The shim's Go build on its own (`cmake --build --target tailscale_test`, the step a job spends its time in), measured
on 2026-11-15 at v1.104.1 and go1.27.1, before the omit tags and the test-hooks build (which builds the shim twice). Cold is empty Go caches (the toolchain and every module downloaded, tsnet compiled); warm is a fresh build
directory over kept caches, as a CI job after the first on a machine.

| Where | Cold | Warm |
|---|---|---|
| macOS arm64 (studio, both architectures, 4 jobs) | 34.5 s | 5.1 s (an edit to the shim rebuilt: both architectures relinked) |
| Linux x86_64 (amd-1, 4 CPUs) | 63.0 s | 3.3 s |
| Windows x64 (captain, 4 CPUs) | 233 s (plus a 64.7 s configure that fetched llvm-mingw) | 13.0 s (the test program only, shim cached) |

On Windows the configure step fetches and unpacks llvm-mingw (190 MB) once per pin and machine (CI keeps it in the key
directory): 27 s on captain, inside the configure that measured, 64.7 s in the cold one above.

In CI, measured on 2026-11-15, as whole jobs (the build step in brackets). Cold is a new key directory (a pin change, or
a machine's first build); warm is a kept key directory after a Go source changed; no-change is a kept key directory and
the same sources.

| OS | Cold | Warm | No-change |
|---|---|---|---|
| macOS (both architectures, both run) | 52 s [40 s] on studio, 40 s [29 s] on mini-m5 | 20 s [8 s] on studio | 20 s on studio |
| Linux x86_64 | 84 s [77 s] on amd-3 alone; 111 s build on hulk; 183 s build on captain's WSL2 beside another | 14 s [7 s] on hulk | 16 s on hulk |
| Windows x64 (captain) | 8 min 20 s in tailscale-prime: configure 58 s (llvm-mingw fetched), build 331 s (go vet, which compiles the whole dependency tree first, 210 s; the DLL's Go build and link 104 s; the test program 13 s), tests 2 s | see the note below | 93 s [29 s] per commit (checkout 24 s, configure 16 s, tests 2 s); a rebuild in the same job: 11.5 to 12.8 s |

A cold build fits a per-commit job's 2 minutes on macOS only, hence `tailscale-prime.yml`. On Windows, once primed, a
per-commit job fits with 27 s to spare, of which checkout is the largest single step. In tailscale-prime the first build
in a job over a full key directory (7,931 cache files, 1.2 GB) took 94 s in tailscale-prime (go vet 37 s, then the
DLL's Go build and link 40 s, then the test program 11 s; two slots building at once on captain), against 12.8 s for a
second build in the same job. What measured there: the cache was in use (GOCACHE and GOMODCACHE in the generated build
are the key directory's), held every entry the cut-off per-commit attempts had written (so a cancelled job kept what
Go had finished), and reading every cache file took 12.2 s the first time and 3.1 s the second, with Defender's
real-time and on-access protection on and its exclusions not readable by the runner's account (it was not changed).
MSBuild ran go vet before the DLL's build rather than beside it, as Make does. The per-commit attempts before priming
did not fail for want of a cache: each was cut off at 2 minutes while Go was still compiling, and a cold build needs
over 5 minutes there.

### Tests

`tests/tailscale` (`ctest -L tailscale`; each test is labelled unit or functional as well):

- `tailscale_unit_version` (unit): the versions are the pins.
- `tailscale_functional_state_dir`: on the real filesystem, in a directory of the test's own: the directory and its
  missing parents created 0700 and the log file 0600 (on Windows: the directory and the parent created on the way with
  a protected ACL granting only this user, and the log file inheriting it); an existing directory and log file open to
  others made owner-only; a second prepare keeps the log.
- `tailscale_functional_refusals`: a null, empty or relative path; a file in the way; a parent this user cannot write
  to (only once the test has seen that this user really cannot create anything in it; root or an administrator can,
  and then those checks are skipped with a line saying so, since a start there would reach tsnet); a start while each
  of tsnet's sign-in variables is set (named, its value never shown; on a path under a file, which no user can
  create); a start with no hostname; a handle that is not a node; the reason cut to the caller's buffer.
- `tailscale_functional_refuses_inherited_key`: with `TS_AUTHKEY` in the environment from the process's start, a start
  refuses.
- `tailscale_functional_lock`: a second start, and a prepare, on a directory a node of this process holds are refused
  however the path is spelled (a trailing separator, `sub/..`), with nothing created; another directory is not held;
  closing the node frees it.
- `tailscale_functional_lock_two_process`: a child (`tailscale_test hold_lock`) holds a directory with a node; this
  process's start and prepare on it are refused as another process's; once the child closes its node and exits, a
  start succeeds.
- `tailscale_functional_panics`: a panic injected into each export (`nts_prepare`, `nts_start`, `nts_backend_state`,
  `nts_close`, `nts_version`, and a start after its directory is held and its log opened) is stopped: the call fails
  with the reason, what it held is given back (the next start on the directory succeeds), and with a node running the
  panic's text is in that node's log.
- `tailscale_functional_log_cap`: 3 MB through the library loggers leaves `nexus-tailscale.log` and
  `nexus-tailscale.log.1` each at most 1 MiB, both 0600, and no third file.
- `tailscale_functional_quiet`: a child (`tailscale_test quiet_child`) with its stdout and stderr redirected to files
  starts and closes a node, writes through Go's `log` and gVisor's logger before, during and after it, and runs the
  panics group; both files stay empty, and the node's log holds the lines written while it ran and none of the others.

The version is a unit test. The lock, panics, log_cap and quiet groups start offline nodes
(`nts_test_start_offline`): the control server is `http://127.0.0.1:9`, which refuses at once, and the port mapper is
off, so a node opens sockets on this machine only (that refused connection, and magicsock's UDP socket) and reaches no
other machine; lock_two_process waits on the clock for its child. Nothing in CI starts a node that reaches Tailscale's
control server. That is `tailscale_test start_needs_login <scratch dir>`, by hand on a bench only: it starts a node,
waits until it asks to sign in, prints the backend state (never the URL) and closes it. It ran once, on captain's
Windows (the Windows 10 manifest in place), on 2026-11-15: `backend state NeedsLogin`; the scratch directory was then
removed, and nobody approved the URL.

### Binary size

Measured on 2026-11-15, Release builds, at tailscale.com v1.104.7 and go1.27.4 with the omit tags. Sizes in bytes; MB
is 10^6 bytes. The test program is the shim and a test of under 150 KB, and links the test-hooks build, which is the
same size as nexus's to within a few KB (on Windows: nts.dll 21,554,176, nts_test.dll 21,557,760).

| OS | Program | With the shim | Cost of the shim |
|---|---|---|---|
| macOS arm64 | tailscale_test | 22,781,776 (stripped 19,357,400) | about +22.7 MB (stripped +19.3 MB) |
| macOS x86_64 | tailscale_test | 24,237,080 (stripped 20,802,112) | about +24.2 MB (stripped +20.7 MB) |
| Linux x86_64 | tailscale_test | 43,895,952 (stripped 23,691,896) | about +43.8 MB with Go's debug information, +23.6 MB stripped |
| Windows x64 | tailscale_test.exe + nts_test.dll | 136,704 + 21,557,760 | +21.6 MB (the DLL, built stripped) |

Against the first measurement (v1.104.1, go1.27.1, no omit tags), the omit tags and the pin move together take 0.9 MB
off the macOS arm64 slice and 1.0 MB off x86_64, 2.3 MB off Linux (1.3 MB stripped) and 0.9 MB off the Windows DLL. On
macOS, measured apart, the omit tags took 1.0 MB off each slice and the pin move put back 0.06 MB.

nexus-server and the client build only on macOS today (Linux has no window and, at Prax's pin, no window logic or
link, so no nexus-server; Windows builds neither natively: CMakeLists.txt says why). Linked into them at v1.104.1 without
the omit tags, the shim cost each +23.6 MB on macOS arm64 (stripped +20.1 MB: nexus-server 1,313,488 to 24,935,680,
the client 10,818,008 to 34,437,624), close to that build's arm64 test program of 23,699,776, so the test program's
size stands for the cost. A universal macOS build carries both architectures, about twice the cost.

### Licences

`src/tailscale/NOTICES.md` lists every Go module the shim links on darwin/arm64, darwin/amd64, linux/amd64 and
windows/amd64, with the licence its licence files state, measured by `go list -deps -tags <the omissions> ./shim` per
platform (`src/tailscale/notices`, which fails on a licence it cannot name or one nexus may not link). At these pins
and omissions: 37 modules, BSD-3-Clause 17, MIT 11, Apache-2.0 5, BSD-2-Clause 2, and 2 whose licence file carries
Apache-2.0, MIT and BSD-3-Clause together (klauspost/compress, gvisor); plus the Go standard library and runtime,
BSD-3-Clause. Every one may be combined with nexus's AGPL-3.0. The omissions took out five modules (coder/websocket,
godbus/dbus, mitchellh/go-ps, golang.org/x/oauth2 and web-client-prebuilt, whose embedded web UI carried a font). CI
fails when the file differs from what the program writes.

These licences require their texts to go with any binary that links the shim, and nothing carries them yet: no nexus
binary that links the shim ships until one does (`docs/roadmap.sexp`, `t-ts-licence-texts`).

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

0. **Build.** The shim pinned to tailscale.com v1.104.7 and Go 1.27.4; CMake builds a c-archive (macOS arm64 and
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

The layer 1 API carries the control server's address as a parameter of start (empty for Tailscale's hosted service),
so the later roadmap row on using your own control server needs nothing redone.
