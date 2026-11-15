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
//   - tsnet's log goes to <dir>/nexus-tailscale.log (0600), never to stderr;
//   - the state directory is owner-only: 0700 on macOS and Linux, a protected ACL that grants only the current user on
//     Windows. It holds the node's keys (tailscaled.state).
// The sign-in URL a node with no state prints is a secret: anyone who opens it can claim the node. It is written only
// to the log file above.
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

// The versions the shim was built with, as "tailscale.com v1.104.1 go1.27.1": static, never freed.
NTS_API const char* nts_version(void);

// Makes dir (an absolute path) a node's state directory, as nts_start does first: creates it and any missing parents,
// makes it owner-only (or refuses one this user does not own), and opens its log file, 0600. Touches no network.
// Returns 0, or -1 with the reason in err.
NTS_API int nts_prepare(const char* dir, char* err, size_t err_size);

// Starts a node named hostname on the state directory dir (prepared as nts_prepare does). Sign-in is interactive only:
// a start refuses while any of tsnet's sign-in variables (TS_AUTHKEY, TS_AUTH_KEY, TS_CLIENT_SECRET, TS_CLIENT_ID,
// TS_ID_TOKEN, TS_AUDIENCE) or TS_CONTROL_URL is set in the environment, since tsnet would act on them. A node with
// saved state comes back signed in; one without asks to sign in (its URL goes to the log file only). Contacts
// Tailscale's control server, so it is never called from a unit test or CI. Returns the node's handle (above 0, never
// reused), or 0 with the reason in err.
NTS_API int nts_start(const char* dir, const char* hostname, char* err, size_t err_size);

// The node's backend state as tsnet names it ("NoState", "NeedsLogin", "Starting", "Running", ...) into out. Returns
// 0, or -1 with the reason in out.
NTS_API int nts_backend_state(int node, char* out, size_t out_size);

// Closes the node and its log file; the handle is not valid afterwards. Returns 0, or -1 with the reason in err.
NTS_API int nts_close(int node, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
