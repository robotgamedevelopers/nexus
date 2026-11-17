# Tailscale built in

nexus reaches its servers over the person's own tailnet with a Tailscale node inside the nexus process: tsnet
(`tailscale.com/tsnet`), the Go library that runs a node with userspace networking. No Tailscale app, no TUN device,
no admin password. nexus owns a small Go shim over tsnet with a C API (`src/tailscale/nts.h`), the same Go code on every
OS. It is built in layers, bottom up, each secured before the next starts (the layer list is at the end of this page
and in `docs/roadmap.sexp`, area `tailscale`).

Why a shim of nexus's own rather than libtailscale: libtailscale does not build for Windows (its C side includes
`<sys/socket.h>`, and its Go side hands file descriptors over `socketpair` and `SCM_RIGHTS`), and it gives no hook for
the log line that carries the sign-in URL, which it writes to stderr.

## Layer 0: Build

Layer 0 is the build and nothing above it: the shim pinned and built on macOS, Linux and Windows, linked into a test
program on each, built and tested in CI, with its size and the licences of everything it links stated. Its C API was
the minimum that proved the build links and runs; layer 1 (below) replaced it with the real one. Nothing in nexus links
the shim yet: the top-level build has it off (`-DNEXUS_TAILSCALE=OFF` by default), and `src/tailscale/CMakeLists.txt`
builds it on its own, without Prax.

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
- **Sign-in only from start's parameters.** tsnet reads an auth key, an OAuth client, workload identity and another
  control server from the environment when its own fields are empty (`TS_AUTHKEY`, `TS_AUTH_KEY`, `TS_CLIENT_SECRET`,
  `TS_CLIENT_ID`, `TS_ID_TOKEN`, `TS_AUDIENCE`, `TS_CONTROL_URL`). `nts_start` refuses while any of them is set, as Go
  or the C library sees the environment, naming the variable and never its value. On Tailscale's hosted service the
  sign-in is interactive only; an auth key is taken only with the person's own control server (layer 1, below).
- **No environment variable steers the node** (layer 1). `src/tailscale/environment.txt` lists every variable the
  code the shim links reads, at the pins and with the omit tags, each with what the shim does with it: refused at every
  start (the sign-in ones above; `TSNET_FORCE_LOGIN`, which would throw the node's identity away; `SSLKEYLOGFILE`,
  which would write the TLS keys of every connection to control to a file; `SSL_CERT_FILE` and `SSL_CERT_DIR`, which
  would change which control certificates are trusted; `GODEBUG`; the `TS_DEBUG_*` knobs and the other variables that
  change what tsnet does), set by the shim itself (`TS_NO_LOGS_NO_SUPPORT`, set again at every start), or harmless
  (named with why). The list is re-derived from the pinned modules' source by `src/tailscale/environment`
  (`go run ./environment -check`, the `nexus_tailscale_environment_check` target, a dependency of `tailscale_checks`),
  which fails when the code reads a variable the list does not name or the list names one the code no longer reads, so
  a pin move that adds a lookup fails the build until it is decided. tsnet's map cache (`ts_omit_cachenetmap`) is left
  out, so a node's map is always one control sent since this start.
- **Every file in the state directory is secret**: never shared, attached to a bug report, committed, printed in CI's
  logs or shown to an AI. Besides the keys, the sign-in URL a node with no state asks for (anyone who opens it can
  claim the node) is written by tsnet to its own `tailscaled.log*.txt` buffers; it goes nowhere outside the
  directory. Since layer 1 the shim's own log, `nexus-tailscale.log`, withholds every URL and every key (below).

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
cmake --build build-tailscale --target nexus_tailscale_notices_check           # NOTICES.md and shim/licences.txt are current
cmake --build build-tailscale --target nexus_tailscale_notices                 # rewrite both
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
statuses turn to the prime's result. Every hand-off ends in success or failure on the commit that asked: a dispatch
GitHub does not take sets the status to error at once, and a prime that does not succeed and never reached its own
status step (its runner lost) gets failure from the prime's `verdict` job. A status belongs to its commit only: when a
pull request's head moves on, the new head gets its own statuses from its own jobs, and the old head's, pending or
not, neither blocks nor passes it. A machine the prime did not reach hands off again at its next per-commit job. The
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

`tests/tailscale` (`ctest -L tailscale`; each test is labelled unit or functional as well). Unit tests touch no file,
socket, clock or network:

- `tailscale_unit_lifecycle`: `go test` of `src/tailscale/lifecycle`, the pure state code: every state and reason named
  as the header names them; an interactive sign-in to ready; running is not ready without the peer map; a close is
  final; each refusal's class, a rate limit as unreachable, logged errors; expiry and a new login; machine approval;
  the generation counts changes only; the model's invariants on random runs; the directory table; the control URL
  and auth key rules (every spelling of the hosted service's name, IDNA, ports, zones); the control record (and state
  with no record refused); the poll's close as control-down while ready; a map for an expired key not ready; the dial
  decision (`DialTarget`: peers only); the scrub of URLs, keys and long texts.
- `tailscale_unit_notices`: `go test` of `src/tailscale/notices`: classifying licence files, scanning linked
  subpackages and Apache-2.0 `NOTICE` files, refusing licences nexus cannot link, and catching missing, empty or
  unlisted sections in `shim/licences.txt`.
- `tailscale_unit_version`: the versions are the pins. `tailscale_unit_licences_unit`: `nts_licences` returns the
  embedded UTF-8 licence bundle, static and NUL-terminated, with the Go standard library's licence and every module
  section carrying non-empty text. `tailscale_unit_names`: every state's and reason's name, and
  "unknown" past them. `tailscale_unit_handles`: every call that takes a handle refuses one never issued, a null
  out-pointer or a bad port, with the reason cut to the caller's buffer. `tailscale_unit_start_checks`: a start is
  refused for an http, user, path, query or hosted-service control URL (also spelled with a trailing dot, in upper
  case, or with `:443`), a host that is not ASCII or that IDNA would change, a port outside 1 to 65535, an IPv6 zone,
  an auth key without a control URL, a missing hostname or directory, and each refused environment variable, with the
  directory never created.
  `tailscale_unit_panics_unit`: a panic injected into every export is stopped there.

Functional tests, on the real filesystem in a directory of the test's own, and where said on sockets of this machine:

- `tailscale_functional_licences`: every row and licence file in `src/tailscale/NOTICES.md` is present in
  `nts_licences` 1-to-1 in order, with non-empty text.
- `tailscale_functional_state_dir`: on the real filesystem, in a directory of the test's own: the directory and its
  missing parents created 0700 and the log file 0600 (on Windows: the directory and the parent created on the way with
  a protected ACL granting only this user, and the log file inheriting it); an existing directory and log file open to
  others made owner-only; a second prepare keeps the log.
- `tailscale_functional_refusals`: a null, empty or relative path; a file in the way; a parent this user cannot write
  to (only once the test has seen that this user really cannot create anything in it; root or an administrator can,
  and then those checks are skipped with a line saying so, since a start there would reach tsnet); a start while each
  of tsnet's sign-in variables is set (named, its value never shown; on a path under a file, which no user can
  create); a start with no hostname; a handle that is not a node; the reason cut to the caller's buffer; a directory
  that recorded another control server, and a directory with a node's state and no record, refused with nothing
  written; a start's record agreed with by the next.
- `tailscale_functional_refuses_inherited_key`: with `TS_AUTHKEY` in the environment from the process's start, a start
  refuses.
- `tailscale_functional_lock`: a second start, and a prepare, on a directory a node of this process holds are refused
  however the path is spelled (a trailing separator, `sub/..`), with nothing created; another directory is not held;
  closing the node frees it.
- `tailscale_functional_lock_two_process`: a child (`tailscale_test hold_lock`) holds a directory with a node; this
  process's start and prepare on it are refused as another process's; once the child closes its node and exits, a
  start succeeds.
- `tailscale_functional_panics`: a panic injected with a node running, and into a start after its directory is held
  and its log opened, is stopped: the call fails with the reason, what it held is given back (the next start on the
  directory succeeds), the node keeps running, and the panic's text is in that node's log.
- `tailscale_functional_log_cap`: 3 MB through the library loggers leaves `nexus-tailscale.log` and
  `nexus-tailscale.log.1` each at most 1 MiB, both 0600, and no third file.
- `tailscale_functional_quiet`: a child (`tailscale_test quiet_child`) with its stdout and stderr redirected to files
  starts and closes a node, writes through Go's `log` and gVisor's logger before, during and after it, and runs the
  panics group; both files stay empty, and the node's log holds the lines written while it ran and none of the others.
- `tailscale_functional_unreachable`: an offline node stays starting or needs-login with no URL, with the reason
  `control-unreachable` that only tsnet's log said.
- `tailscale_functional_watch`: the callback runs once at once and then for each change up to the latest
  generation; none runs after `nts_close` has returned; removing it stops it; `nts_close` from inside it closes the
  node.
- `tailscale_functional_threads`: eight threads call status, wait, peers and listen on one node while another closes
  it; every call answers, an error always with its reason, before and after the close.

The groups that follow sign nodes in to the test control server (layer 1, above):

- `tailscale_functional_sign_in`: needs-login with a URL, which is refused into a buffer one byte short; approved, the
  node is ready; the URL is gone once past needs-login; the shim's log never holds it; the directory records its
  control server.
- `tailscale_functional_tailnet`: two nodes see each other by DNS name (letter case and a trailing dot ignored), with
  the tags an admin gives; TCP both ways by DNS name; who-is of the remote address, and "unknown" for an address that
  is no node; a read with nothing to read times out at once; a second read while one waits is busy; a close from
  another thread ends a waiting read with an error; double close and use after close are errors; the far end sees the
  stream end; handles of the wrong kind refused; UDP, connected by name and replied to by address and by name; a
  datagram longer than the buffer returns its whole length with what fit; a size above 1 GiB refused; dials and sends
  to loopback (a trap listening there), a private address, a tailnet address no peer has, `localhost` and a public
  name refused with the reason, with no call into tsnet that sends and nothing at the trap; timeout 0 (a dial refused,
  peers answered, an accept taking a waiting connection, a read returning data already there); closing a node closes
  its handles.
- `tailscale_functional_restart`: a signed-in node closed and started again on its directory is ready with no sign-in
  and never shows a URL.
- `tailscale_functional_expiry`: control expires a node's key; it is expired with its key expiry, with no URL; a new
  login gives a new URL, and approved, the node is ready with the same DNS name and addresses.
- `tailscale_functional_control_down`: with control stopped, a ready node keeps ready with the reason
  `control-unreachable` and `control_down` set, and a TCP connection through the tailnet keeps carrying bytes; with
  control back, the reason and the flag clear.
- `tailscale_functional_control_blackhole`: control's connections cut and its port then held silently: a ready node
  sets `control_down` within 10 s (in practice at the next look, 250 ms), stays ready, and keeps carrying bytes; with
  control back, the flag clears.
- `tailscale_functional_auth_key`: a wrong key is failed with reason `auth-key` and never in the text; the right key is
  ready with no sign-in; the shim's log never holds the key.
- `tailscale_functional_long_sign_in`: a child with stdout and stderr in files leaves a sign-in waiting until tsnet has
  written its URL line twice (it repeats every 5 s), then approves it: the log holds the placeholder twice and never the
  URL or any `://`, no reason text ever names a URL, and both files stay empty.
- `tailscale_functional_control_error` (offline): a control error naming the node's own auth key, another key and a
  URL, 700 bytes long, gives `failed`/`auth-key` with a text that names none of them, at most 256 bytes; the log holds
  neither key.
- `tailscale_functional_close_each_other` (offline): two nodes' callbacks, running at once, each close the other node;
  both closes return 0 and neither waits on the other.
- `tailscale_functional_certificate`: a control server whose certificate is not trusted is failed with reason
  `certificate`.

The lock, panics, log_cap, quiet, unreachable, watch and threads groups start offline nodes
(`nts_test_start_offline`): the control server is `http://127.0.0.1:9`, which refuses at once, and the port mapper is
off, so a node opens sockets on this machine only. The groups on the test control server reach only it, its relay and
its STUN server, all on 127.0.0.1. The functional groups wait only on conditions (a state's generation, a peer seen,
a file written), never on a fixed sleep; lock_two_process waits on the clock for its child. Each takes 0.02 s to 2 s on
an Apple silicon Mac. Nothing in CI starts a node that reaches Tailscale's control server. That is
`tailscale_test start_needs_login <scratch dir>`, by hand on a bench only: it starts a node, waits until it asks to
sign in, prints the state (never the URL) and closes it. It ran once, at layer 0, on captain's Windows (the Windows 10
manifest in place), on 2026-11-15: `backend state NeedsLogin`; the scratch directory was then removed, and nobody
approved the URL. It has not been run with layer 1's API: against the hosted service, layer 1 is tested by the test
control server only.

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

The table is layer 0's API. Layer 1's API, on macOS arm64 the same day: a program that links nexus's build
(`libnts.a`) and calls `nts_start`, so the linker keeps what nexus would, is 22,879,496 bytes (stripped 19,433,584),
+0.1 MB over layer 0's test program. On Windows, nexus's `nts.dll` is 21,713,920, +0.16 MB over layer 0's. Layer 1's
test programs are no longer the measure (macOS arm64 24,104,752; Linux x86_64 46,177,736 unstripped; Windows
tailscale_test.exe 250,880): their test-hooks build carries tailscale's test control server and a DERP relay, which
nexus's build does not.

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
platform (`src/tailscale/notices`, which scans each module's root and the directories of the packages the shim links
inside it for licence and Apache-2.0 `NOTICE` files, and fails on a licence it cannot name or one nexus may not link).
At these pins and omissions: 37 modules, BSD-3-Clause 17, MIT 11, Apache-2.0 5, BSD-2-Clause 2, and 2 that carry
Apache-2.0, MIT and BSD-3-Clause together (`klauspost/compress`, including its linked `internal/snapref/LICENSE` and
`zstd/internal/xxhash/LICENSE.txt`, and `gvisor`); plus the Go standard library and runtime, BSD-3-Clause. Every one
may be combined with nexus's AGPL-3.0. The omissions took out five modules (coder/websocket, godbus/dbus,
mitchellh/go-ps, golang.org/x/oauth2 and web-client-prebuilt, whose embedded web UI carried a font).

The verbatim licence and notice texts — the pinned Go toolchain's `LICENSE` (`go env GOROOT`) followed by the 39
licence files across those 37 modules — are generated by `src/tailscale/notices` into `src/tailscale/shim/licences.txt`
(139,410 bytes, UTF-8 with LF line ends), embedded into `src/tailscale/shim` by `//go:embed`, and exposed through
`nts_licences(size_t* len)` (`src/tailscale/nts.h`), so every binary that links the shim carries them with no external
file dependency. On Linux x86_64 (`clang` and `lld`, Release), embedding `shim/licences.txt` adds +142,816 bytes
unstripped (44,049,344 to 44,192,160) and +140,928 bytes stripped (23,748,680 to 23,889,608, +0.14 MB) to a program
linking `libnts.a` (+147,296 bytes in `libnts.a` itself, 61,737,980 to 61,885,276). CI's `tailscale_checks` runs
`nexus_tailscale_notices_check` (`go run ./notices -check`), failing when either `NOTICES.md` or `shim/licences.txt`
differs from what `notices` writes at the pins or when any module or file in `NOTICES.md` is missing or empty in
`shim/licences.txt`, and `tailscale_functional_licences` checks `nts_licences` 1-to-1 against `NOTICES.md`.

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

## Layer 1: Shim API

The real C API, `src/tailscale/nts.h`, whose header is the contract (threads, errors, handles and every call's result
are stated there); this section says what it is for and what it rests on. It is linked only into the test program:
nothing in nexus-server or the client calls it yet.

| Calls | What they do |
|---|---|
| `nts_start(params)`, `nts_close(node)`, `nts_prepare(dir)` | start a node on a state directory, close it and everything it owns |
| `nts_status_get`, `nts_status_wait(node, after, timeout)`, `nts_watch(node, fn, user)` | the node's state, polled, waited on by generation, or by callback |
| `nts_login_url`, `nts_login` | the sign-in URL while one is needed; a new login (after expiry or a refusal) |
| `nts_peers`, `nts_peer_find(dns_name)`, `nts_whois_addr(remote)` | the tailnet's peers, one by DNS name, who is behind an address |
| `nts_listen_tcp`, `nts_accept`, `nts_dial_tcp`, `nts_dial_udp`, `nts_listen_udp` | TCP and UDP through the node's embedded network stack |
| `nts_read`, `nts_write`, `nts_recv_from`, `nts_send_to`, `nts_local_addr`, `nts_remote_addr`, `nts_handle_close` | I/O with timeouts, addresses, closing a handle |
| `nts_version`, `nts_licences`, `nts_state_name`, `nts_reason_name` | the pins; the embedded licence texts; names of states and reasons |

Nothing goes through the host's network: every dial, listen and name lookup is tsnet's userspace stack, so the host's
routes, DNS and firewall are never touched. **A dial or a datagram goes only to a peer.** tsnet's own `Dial` would
reach an address off the tailnet through the host (and give a name to the host's resolver), so the shim decides the
address itself before calling it (`lifecycle.DialTarget`, a pure function with unit tests): an IP must be one of a
peer's tailnet addresses, and a name must be a peer's DNS name or its first label (letter case and a trailing dot
ignored), giving the peer's IPv4 address. Anything else (loopback, a private or public address no peer has, a name no
peer has, an IPv6 zone) is refused with the reason before any socket or resolver is touched. A peer's subnet routes
are not dialable through this API.

**Timeouts.** A negative timeout waits without limit. 0: `nts_status_wait` does not wait; peers, peer find and who-is
answer from the node's own state (no limit; they never wait on the network); `nts_accept` takes a connection already
waiting or times out at once; reads, writes and datagrams wait up to 1 ms (the embedded stack fails a deadline already
past before it looks for data); a dial is refused, since it must wait for the peer.

**UDP truncation.** A datagram read (`nts_recv_from`, `nts_read` on a `nts_dial_udp` socket) goes through a 64 KiB
buffer of the shim's and returns the datagram's whole length: more than the caller's size means it was cut, and the
buffer holds what fit. A size above 1 GiB in any read, write or datagram call is refused by validation, before the
buffer is touched.

### Start parameters and the control server

`nts_start` takes a state directory, a hostname, a control URL, an auth key and an ephemeral flag.

- **The control URL** is empty for Tailscale's hosted service, or the person's own control server (headscale, below).
  It must be `https://` with a host and nothing else (no user, query, fragment or path), and may not name
  `tailscale.com`, `tailscale.io` or any host under them (leave it empty for that). It is canonicalised before it is
  compared or recorded: the host lower-cased, one trailing dot dropped, `:443` dropped; a host that is not ASCII, that
  IDNA would change or calls invalid, a port outside 1 to 65535 (or empty) and an IPv6 zone are refused, so no spelling
  of the hosted service gets past the check. An http address is refused for the reason found in the headscale trial
  below. The URL is never put into an error.
- **The auth key** is taken only with a control URL: the hosted service signs in interactively, the owner's choice.
  It is never logged or put into an error; a key control refuses is the failed state with reason `auth-key`.
- **One control server per state directory.** The first start records the control server in
  `<dir>/nexus-tailscale.control` (`hosted`, or the URL in its canonical form), 0600; a start naming another is
  refused before tsnet opens anything. A directory that holds a node's state and no record was changed outside nexus
  (or is layer 0's, which shipped nowhere) and is refused: the record is restored, or a new directory given.
- **Ephemeral** asks control to remove the node soon after it goes offline.

### States, and what ready means

| State | Meaning | Reasons it may carry |
|---|---|---|
| `not-started` | before the start | |
| `starting` | started; tsnet has not yet said whether it needs a sign-in | `control-unreachable` |
| `needs-login` | a sign-in is needed; `nts_login_url` gives the URL once tsnet has one | `machine-approval`, `control-unreachable` |
| `running` | tsnet runs, but no network map has been received and applied since this start or sign-in | `control-unreachable` |
| `ready` | running **and** a network map received and applied since this start or sign-in: peers can be listed and dialed | `control-unreachable` (the tailnet keeps working while control is down) |
| `expired` | the node's key expired; `nts_login` asks for a new sign-in and the node keeps its identity | |
| `stopped` | closed by the host, or stopped by control | `closed`, `control-stopped` |
| `failed` | control refused the node; `nts_login` tries again | `auth-key`, `tags`, `refused`, `certificate`, `logging` |

Each status carries the state, a reason code and its text, the key's expiry (seconds since 1970 UTC, 0 when it does
not expire or is not known), `control_down`, and a generation that counts every change, so a host that saw
generation g has seen every change up to it.

**Ready** is not tsnet's `Running`: a restarted node reports running before its peers arrive (seen against headscale,
below). The shim calls a node ready once tsnet's backend is `Running` and tsnet has received and applied a network map
since this start or sign-in: tsnet's own signal, never a time. tsnet sends the self node (`ipn.Notify.SelfChange`)
after it applies each map from control; a watcher that subscribes after one came sees it in its first status
(`InitialStatus`, `Self.InNetworkMap`), which counts only before any `nts_login`. The shim builds tsnet without its
map cache (`ts_omit_cachenetmap`), so a map tsnet holds is one control sent since this start. A map for an expired
key is not ready; a new login or an expiry clears ready.

**Control down** (`control_down`, reason `control-unreachable`): set the moment the node's map poll to control closes
(tsnet's health tracker, `GetInPollNetMap`, read every 250 ms: the poll is the node's one connection to control), in
any state; a ready node stays ready, since the tailnet keeps working on the map it has. The next map clears it. A
poll that stays open but silent (control's host gone without closing the connection) is seen only when tsnet's own
keep-alive watchdog closes it, which may take up to about two minutes; the shim adds no timer of its own.

**Refusals seen only in tsnet's log become states.** Control's refusals reach the shim as `ipn.Notify.ErrMessage`
(auth key, tags, logging, others) and are classified into reasons; an untrusted certificate and an unreachable control
server reach no notification at all, only tsnet's backend log (`Received error: ...`), so the shim reads those lines
from its own backend logger and turns them into `failed`/`certificate` and the `control-unreachable` reason. A rate
limit counts as unreachable. A connection to control that ends in EOF, which tsnet logs only as a verbose line
(`[v1] Received error: EOF`), is caught as the poll closing (above).

**Control's words are scrubbed.** A refusal's text is control's own, and could echo the auth key or name a URL. Every
text from tsnet or control that reaches a state (`ErrMessage`, a logged error) has every URL replaced by
`<a URL, withheld>`, and every auth key of a node of this process and every `tskey-...` by `<a secret, withheld>`;
the reason code is classified from the whole scrubbed text, and the text a status carries is cut to 256 bytes. The
same scrub runs on everything written to `nexus-tailscale.log`: tsnet's user log, Go's `log` package, gVisor's logger
and a stopped panic's text.

**Expiry** is read from the node's own status (its key expiry passing, edge-triggered), since some control servers
leave an expired node needing a login with no URL. `nts_login` then starts an interactive login on the same keys and
state directory: a new URL, and after approval the same DNS name and addresses.

### The sign-in URL is a secret

Anyone who opens it can claim the node. It reaches the host only through `nts_login_url`, only in `needs-login`, and
is never cut (a buffer too small is an error), and is never in a status's text. tsnet's user log, which carries it
every 5 s while a sign-in waits, reaches `nexus-tailscale.log` through the scrub above, which replaces every URL with
`<a URL, withheld>`; no code of nexus's logs it. tsnet's own backend
buffers (`tailscaled.log*.txt`) still hold it, inside the owner-only directory. The header says so at its top.

### Handles, panics and threads

Every node, listener, connection and UDP socket is an int handle from one counter, never reused. A handle of the wrong
kind, a closed one (a second close included) and one never issued are errors with a reason. Closing a node closes
every handle it owns, and a call waiting on one of them ends with an error. Every export recovers a Go panic into its
failure value. Any call may come from any thread; on one connection one read and one write may run at once, a second
of the same kind is refused as busy. The state callback runs on a thread of the shim's own, one change at a time, and
no callback of a node runs once `nts_close` of it has returned on a thread that runs no callback. From inside any
callback (its own node's, or another's: two callbacks may close each other's nodes at once), `nts_close` does not
wait for the closed node's callback, which starts no more but may still be running when the close returns.

### The model

`models/Tailscale.tla` is the node's lifecycle as the host sees it: the facts tsnet and control give
(`src/tailscale/lifecycle`, a Go module with no dependencies, which the shim calls for every state it reports), the
host's start, close and login, and the state directory table, with the world around them: control up or down, a
start with an auth key or without, a control server whose certificate is trusted or not, directories control has
enrolled. Invariants: ready implies running and the peer map (`ReadyHasPeers`); a closed node never reports running
(`ClosedNeverRunning`); the URL is shown only in needs-login (`URLOnlyInNeedsLogin`); one live node per state
directory (`OneNodePerDir`); every state and reason is one of the header's (`StatesNamed`); a node whose control
certificate is not trusted is never ready (`CertNeverReady`). Liveness: `GetsReady`, a started node never closed whose
control certificate is trusted ends ready, and again after every expiry, outage or refusal; `CertFails`, one whose
certificate is not trusted ends failed with reason `certificate` and stays there.

The liveness holds only under assumptions about the world outside the shim, which the spec's `Fairness` states and
which nothing in the shim can make true: the person approves a sign-in URL that stays shown; the host calls
`nts_login` whenever the node stays expired, failed or waiting with no URL; control, once down, comes back up; and
tsnet keeps taking the steps it can. A person who never approves, a host that never asks again, or a control server
that never comes back leaves the node short of ready for good, and the model says nothing against that. Nothing forces
a start, a close, an outage, an expiry or a refusal.

Checked by TLC on three instances (two node slots on one directory, one outage, one expiry, one refusal):
`MCTailscale.cfg` (interactive starts, trusted certificates: 47,169 distinct states, about 10 s), `MCTailscaleKeys.cfg`
(starts with and without a key, no expiry: 68,635, about 16 s) and `MCTailscaleCerts.cfg` (certificates trusted or not:
74,329, about 17 s), every invariant and both liveness properties. Seven reversed witnesses under `models/witnesses/`
(`WTs*`) each break the code in one way and must fail: ready without peers, the URL kept after needs-login, a closed
node still running, the table ignoring a held directory, a certificate failure ignored (each its invariant), no new
login after expiry and the poll's state ignored (each liveness). Four orders recorded from the real shim against the
test control server (`TailscaleOrders.tla`: a restart running straight from its own state, an auth-key start with no
sign-in, a certificate failure first, an interactive restart by way of starting) must each be produced by the model's
`Next`, environment included. The trace check (`TailscaleTrace.tla`) binds the Go code to the model: a random walk
(`src/tailscale/lifecycle/walk`) plays the model's world (control's outages, key expiries, refusals, enrolled
directories, keys and certificates, each bounded as the model bounds them) and drives the real `lifecycle` code with
only the events that world can produce; TLC checks every record is a step of the spec's actions, the world's
included, on five seeds (up to 1,000 steps; a walk ends when its world is spent, 92 to 271 steps on those seeds); five
faulty walks (ready, url, close, table, down) must be rejected. All run as `tlc_tailscale_*` in CI's model shard.

### Tests against a test control server

The functional groups below sign nodes in to tailscale's own test control server (`tailscale.com/tstest/integration/
testcontrol`), run in the test program's process on 127.0.0.1 with a DERP relay and a STUN server of its own, also on
127.0.0.1 (`src/tailscale/shim/hooks_control.go`, in the test-hooks build only). The port mapper is off and the DERP
map holds only that relay, so a node opens sockets on this machine only and reaches no other. It is the same
control protocol as the hosted service's, from the same release of tailscale.com; what it does not show is the
hosted service's own behaviour: its sign-in pages, its key expiry times, its rate limits, its admin console's
approvals and tags, and the network between. Nothing in CI or the tests reaches Tailscale's hosted service or any
outside machine, and no node is signed in to a real tailnet.

## The layers

Bottom up, each secured before the next starts. **Sign-in is path A, interactive only: the owner's choice.**

0. **Build.** The shim pinned to tailscale.com v1.104.7 and Go 1.27.4; CMake builds a c-archive (macOS arm64 and
   x86_64, Linux x86_64 and WSL2) and a DLL with an import library (Windows MSVC); the Windows program gets the
   manifest; CI builds it on all three; the binary-size cost is stated. *Built; this page, above.*
1. **Shim API.** Start (with the control server as a parameter) and close, a state watch (needs login with its URL,
   running, ready, expired, failed with a reason), peers by DNS name, who-is, TCP listen and dial, UDP listen and
   dial; logs to the file with URLs withheld, uploads off, the state directory owner-only and one control server's; a
   TLA+ model of the node's lifecycle with a trace check; tests against tailscale's test control server. *Built; this
   page, above.*
2. **Client sign-in.** The URL to the system browser, wait for ready, keep the state (Prax later); the first-run OS
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
so the later roadmap row on using your own control server needs nothing redone: the decisions below (https only, a
pre-auth key, ready meaning peers, expiry read from status, peers by DNS name, a state directory per control server)
are layer 1's rules already. What that row still owes is the product around them (the key through the secret dialog,
the address in settings) and a run against a real headscale.

### Later, not built: use your own control server (headscale)

For people who run their own control server: they set headscale up themselves and give its address, and nexus's
embedded nodes use it instead of Tailscale's hosted service (roadmap `ts-own-control`). Decided for that row, from a
trial against a real headscale on 2026-11-15, so none of it is asked again:

- **The address must be https.** With a plain http address to a public server, tsnet falls back to port 443 after a
  recent dial; after a control outage a node in the trial stayed off control for over 5 minutes once the server was
  back, until it was restarted. Over https the fallback is the same port, and the node was back within seconds.
- **Enrolment, either of two ways.** A pre-auth key, minted on the server and pasted once through the product's secret
  dialog, is the normal way and the only way a node gets a tag (the tag comes from the key; a node that asks for a tag
  itself is refused). Or the server's admin approves the node's sign-in with their own command on the server
  (`headscale auth register`), which the product shows them.
- **Ready means peers are present**, not that the node reports running: a restarted node reports running before its
  peers arrive.
- **Expiry is read from status**, and on expiry the product starts the login again itself: headscale's expiry leaves a
  node needing a login with no sign-in URL until something asks for one.
- **Peers are matched by DNS name**, not host name: headscale renames a duplicate host name.
- **A separate state folder per control server**, so each server's enrolment stays its own and switching back costs
  nothing.

Tailscale's hosted service stays interactive sign-in only.
