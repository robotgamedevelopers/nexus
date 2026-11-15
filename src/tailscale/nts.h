// nexus's Tailscale shim: a small C API over tsnet (tailscale.com/tsnet), the Go library that runs a Tailscale node
// inside this process with userspace networking (no TUN, no root, no Tailscale app). The Go side is src/tailscale/shim;
// docs/tailscale.md gives the pins, the build and the layers.
//
// Layer 0 (Build) is this much and no more: the version, the state directory, and starting and closing a node. The
// real API (the state watch, peers, WhoIs, TCP and UDP) is layer 1.
//
// What holds in every build, from the first call:
//   - log uploads to Tailscale are off (envknob.SetNoLogsNoSupport, which sets TS_NO_LOGS_NO_SUPPORT=true in this
//     process's environment when the shim loads);
//   - nothing the shim or tsnet logs goes to stdout or stderr. tsnet's user log goes to <dir>/nexus-tailscale.log
//     (0600; past 1 MiB it moves to nexus-tailscale.log.1 and a new one begins), and so do Go's log package and
//     gVisor's logger while that node is the newest running (nowhere while none runs). tsnet's backend log goes to its
//     own buffer files, <dir>/tailscaled.log1.txt and tailscaled.log2.txt, which tsnet caps at 50 MB together
//     (filch.DefaultMaxFileSize);
//   - the state directory is owner-only: 0700 on macOS and Linux, a protected ACL that grants only the current user on
//     Windows. It holds the node's keys (tailscaled.state);
//   - a state directory is one node's at a time: a start or a prepare on a directory that a running node holds, in
//     this process or another, is refused before anything in it is opened (an OS lock on <dir>/nexus-tailscale.lock,
//     held for the node's life and dropped by the OS if the process ends);
//   - a Go panic in a call is stopped there: the call fails with a reason, gives back what it held, and the panic's
//     text goes to the newest running node's log. What the shim cannot stop: a panic in one of tsnet's own goroutines,
//     or a Go fatal error (out of memory, a deadlock, a concurrent map write), which Go prints to stderr and which ends
//     the whole process, as it does any Go program.
// Every file in the state directory is secret, and none is ever shared, attached to a bug report or shown to anyone:
// besides the keys, the sign-in URL a node with no state asks for (anyone who opens it can claim the node) is written
// to nexus-tailscale.log and to the tailscaled.log*.txt buffers.
//
// Every call that can fail writes its reason and the fix into the caller's buffer (err, err_size), NUL-terminated and
// cut to fit; on success it writes an empty string there. A null buffer or a size of 0 is allowed, and then the reason
// is not written.
#ifndef NEXUS_TAILSCALE_NTS_H
#define NEXUS_TAILSCALE_NTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// On Windows the shim is a DLL (nts.dll) with an MSVC import library; elsewhere it is a static c-archive.
#if defined(_WIN32)
#define NTS_API __declspec(dllimport)
#else
#define NTS_API
#endif

// The versions the shim was built with, as "tailscale.com v1.104.7 go1.27.4": static, never freed.
NTS_API const char* nts_version(void);

// Makes dir (an absolute path) a node's state directory, as nts_start does first: refuses it while a running node holds
// it, creates it and any missing parents (each parent it creates owner-only too), makes it owner-only, and opens its
// log file, 0600. On macOS and Linux an existing directory another user owns is refused; on Windows the directory's
// ACL is rewritten to the current user alone whoever owns it, and refused only when this user may not change it.
// Touches no network. Returns 0, or -1 with the reason in err.
NTS_API int nts_prepare(const char* dir, char* err, size_t err_size);

// Starts a node named hostname on the state directory dir (prepared as nts_prepare does). Sign-in is interactive only:
// a start refuses while any of tsnet's sign-in variables (TS_AUTHKEY, TS_AUTH_KEY, TS_CLIENT_SECRET, TS_CLIENT_ID,
// TS_ID_TOKEN, TS_AUDIENCE) or TS_CONTROL_URL is set in the environment, since tsnet would act on them. A node with
// saved state comes back signed in; one without asks to sign in (its URL goes to the state directory only). Contacts
// Tailscale's control server, so it is never called from a unit test or CI. Returns the node's handle (above 0, never
// reused), or 0 with the reason in err.
NTS_API int nts_start(const char* dir, const char* hostname, char* err, size_t err_size);

// The node's backend state as tsnet names it ("NoState", "NeedsLogin", "Starting", "Running", ...) into out. Returns
// 0, or -1 with the reason in out.
NTS_API int nts_backend_state(int node, char* out, size_t out_size);

// Closes the node and its log file and frees its state directory; the handle is not valid afterwards. Returns 0, or -1
// with the reason in err.
NTS_API int nts_close(int node, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
