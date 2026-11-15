// nexus's Tailscale shim: a small C API over tsnet (tailscale.com/tsnet), the Go library that runs a Tailscale node
// inside this process with userspace networking (no TUN, no root, no Tailscale app, nothing that touches the host's
// network settings). The Go side is src/tailscale/shim; docs/tailscale.md gives the pins, the build, the states and
// the layers.
//
// Layer 1 (Shim API) is this header: start and close a node, its state, peers, who-is, TCP and UDP through the
// embedded stack, key expiry and a new login. Nothing in nexus links it yet; only its test programs do.
//
// What holds in every build, from the first call:
//   - log uploads to Tailscale are off (envknob.SetNoLogsNoSupport, which sets TS_NO_LOGS_NO_SUPPORT=true in this
//     process's environment when the shim loads);
//   - nothing the shim or tsnet logs goes to stdout or stderr. tsnet's user log goes to <dir>/nexus-tailscale.log
//     (0600; past 1 MiB it moves to nexus-tailscale.log.1 and a new one begins), with every URL in it withheld, and so
//     do Go's log package and gVisor's logger while that node is the newest running (nowhere while none runs).
//     tsnet's backend log goes to its own buffer files, <dir>/tailscaled.log1.txt and tailscaled.log2.txt, which tsnet
//     caps at 50 MB together (filch.DefaultMaxFileSize) and which hold the sign-in URL (tsnet writes it there itself);
//   - the state directory is owner-only: 0700 on macOS and Linux, a protected ACL that grants only the current user on
//     Windows. It holds the node's keys (tailscaled.state);
//   - a state directory is one node's at a time: a start or a prepare on a directory that a running node holds, in
//     this process or another, is refused before anything in it is opened (an OS lock on <dir>/nexus-tailscale.lock,
//     held for the node's life and dropped by the OS if the process ends);
//   - a state directory is one control server's: the first start records it (<dir>/nexus-tailscale.control), and a
//     start that names another is refused. Keep one directory per control server;
//   - a Go panic in a call is stopped there: the call fails with a reason, gives back what it held, and the panic's
//     text goes to the newest running node's log. What the shim cannot stop: a panic in one of tsnet's own goroutines,
//     or a Go fatal error (out of memory, a deadlock, a concurrent map write), which Go prints to stderr and which ends
//     the whole process, as it does any Go program.
// Every file in the state directory is secret, and none is ever shared, attached to a bug report or shown to anyone.
//
// THE SIGN-IN URL IS A SECRET: anyone who opens it can claim the node. This API hands it to the caller only through
// nts_login_url, only while the node's state is NTS_STATE_NEEDS_LOGIN, and the shim's own code never logs it
// (tsnet's backend buffer files above do hold it). Never print it, log it, put it in a file, or show it to an AI.
// The auth key passed to nts_start is a secret too, and is never logged or put in an error.
//
// Handles. Every node, listener, connection and UDP socket is an int handle above 0, one counter for all, never reused.
// A handle of the wrong kind, a closed one (closing it again included) and one never issued are errors with a reason,
// never a crash. Closing a node closes every handle it owns.
//
// Errors. Every call that can fail writes its reason and the fix into the caller's buffer (err, err_size),
// NUL-terminated and cut to fit; on success it writes an empty string there. A null buffer or a size of 0 is allowed,
// and then the reason is not written.
//
// Threads. Every call may be made from any thread, and calls on different handles run concurrently. On one
// connection, one nts_read and one nts_write may run at once (a second of the same kind at the same time is refused
// as busy); on one listener one nts_accept, on one UDP socket one nts_recv_from and one nts_send_to. A call that waits
// takes a timeout in milliseconds: 0 does not wait (on a read, write, accept or datagram call it waits up to 1 ms,
// since the embedded stack fails a deadline already past before it looks for data), a negative one waits without
// limit; closing the handle (or its node) from another thread ends the wait with an error. The state callback
// (nts_watch) runs on a thread of the shim's own, never inside another call; it may call anything here except
// nts_close on its own node, which it may call too but which then returns before the callback does.
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

// Results of the calls that wait or read: a count of bytes or a handle (>= 0), or one of these.
#define NTS_ERROR (-1)
#define NTS_TIMEOUT (-2)

// A node's state, as the host sees it (docs/tailscale.md, "States"; models/Tailscale.tla).
#define NTS_STATE_NOT_STARTED 0
#define NTS_STATE_STARTING 1    // started; tsnet has not yet said whether it needs a sign-in, or brings the tunnel up
#define NTS_STATE_NEEDS_LOGIN 2 // a sign-in is needed: nts_login_url gives the URL when tsnet has one
#define NTS_STATE_RUNNING 3     // running, but no peer map from control yet since this start or sign-in
#define NTS_STATE_READY 4       // running and the peer map received: peers can be listed and dialed
#define NTS_STATE_EXPIRED 5     // the node's key expired: nts_login asks for a new sign-in, keeping its identity
#define NTS_STATE_STOPPED 6     // closed, or stopped by control
#define NTS_STATE_FAILED 7      // control refused the node, with the reason; nts_login tries again

// What qualifies a state.
#define NTS_REASON_NONE 0
#define NTS_REASON_AUTH_KEY 1            // failed: control refused the auth key (used, expired or unknown)
#define NTS_REASON_TAGS 2                // failed: control refused a tag
#define NTS_REASON_REFUSED 3             // failed: control refused the login for another reason (in the text)
#define NTS_REASON_CERTIFICATE 4         // failed: the control server's certificate is not trusted
#define NTS_REASON_LOGGING 5             // failed: the tailnet requires log uploads, which nexus never turns on
#define NTS_REASON_CONTROL_UNREACHABLE 6 // any state: control cannot be reached now; tsnet retries by itself
#define NTS_REASON_MACHINE_APPROVAL 7    // needs-login: the control server's admin must approve this machine
#define NTS_REASON_CLOSED 8              // stopped: closed by the host
#define NTS_REASON_CONTROL_STOPPED 9     // stopped: control stopped the node

#ifndef NTS_TYPES_ONLY

// The versions the shim was built with, as "tailscale.com v1.104.7 go1.27.4": static, never freed.
NTS_API const char* nts_version(void);

// The names of a state and a reason ("ready", "control-unreachable"): static, never freed; "unknown" for any other.
NTS_API const char* nts_state_name(int state);
NTS_API const char* nts_reason_name(int reason);

// Makes dir (an absolute path) a node's state directory, as nts_start does first: refuses it while a running node holds
// it, creates it and any missing parents (each parent it creates owner-only too), makes it owner-only, and opens its
// log file, 0600. On macOS and Linux an existing directory another user owns is refused; on Windows the directory's
// ACL is rewritten to the current user alone whoever owns it, and refused only when this user may not change it.
// Touches no network. Returns 0, or -1 with the reason in err.
NTS_API int nts_prepare(const char* dir, char* err, size_t err_size);

#endif

// What nts_start is given. Unset pointers (NULL) are empty strings.
typedef struct nts_start_params {
    const char* dir;         // the state directory, an absolute path (prepared as nts_prepare does)
    const char* hostname;    // the node's name on the tailnet, not empty
    const char* control_url; // empty: Tailscale's hosted service; else the person's own control server, https only
    const char* auth_key;    // empty: interactive sign-in; else a key for the control server named (never the hosted one)
    int ephemeral;           // non-zero: control removes the node soon after it goes offline
} nts_start_params;

#ifndef NTS_TYPES_ONLY

// Starts a node. Refused before anything starts: a missing directory or hostname; a control_url that is not an
// https:// address (with a host, no user, query, fragment or path), or that names Tailscale's hosted service (leave
// it empty for that); an auth_key without a control_url (the hosted service signs in interactively only); a state
// directory that recorded another control server; a directory a running node holds; and any of tsnet's sign-in
// variables in the environment (TS_AUTHKEY, TS_AUTH_KEY, TS_CLIENT_SECRET, TS_CLIENT_ID, TS_ID_TOKEN, TS_AUDIENCE,
// TS_CONTROL_URL), since tsnet would act on them. A node with saved state comes back signed in; one without asks to
// sign in (nts_login_url) or uses the auth key. Contacts the control server, so it is never called from a unit test.
// Returns the node's handle, or 0 with the reason in err.
NTS_API int nts_start(const nts_start_params* params, char* err, size_t err_size);

// Closes the node, every handle it owns and its log file, and frees its state directory; its state is then stopped
// (reason closed) and the handle is not valid afterwards. Returns 0, or -1 with the reason in err.
NTS_API int nts_close(int node, char* err, size_t err_size);

#endif

// A node's state: what nts_status_get and nts_status_wait fill in.
typedef struct nts_status {
    int state;                     // NTS_STATE_*
    int reason;                    // NTS_REASON_*
    unsigned long long generation; // counts the changes of state, reason and text: a host that saw g has seen them all
    long long key_expiry;          // when the node's key expires, seconds since 1970 UTC; 0 when it does not or is not
                                   // known yet (control sets it; tagged nodes and many self-hosted servers never expire)
    char reason_text[512];         // the reason in plain words, or empty
} nts_status;

#ifndef NTS_TYPES_ONLY

// The node's state now, without waiting. Returns 0, or -1 with the reason in err.
NTS_API int nts_status_get(int node, nts_status* out, char* err, size_t err_size);

// Waits until the node's generation is past after (or timeout_ms passes), then fills out as nts_status_get does.
// Returns 0 when it changed, NTS_TIMEOUT when it did not (out is filled in either way), or -1 with the reason in err.
NTS_API int nts_status_wait(int node, unsigned long long after, int timeout_ms, nts_status* out, char* err,
                            size_t err_size);

// The sign-in URL (a secret: see the top of this file) into url, only while the state is needs-login and tsnet has
// one. Returns 0, or -1 with the reason in err: no URL now, or a buffer too small (the URL is never cut).
NTS_API int nts_login_url(int node, char* url, size_t url_size, char* err, size_t err_size);

#endif

// Called after a change of the node's state, with its new generation; see Threads at the top of this file. It should
// return soon: the next change waits for it.
typedef void (*nts_watch_fn)(void* user, int node, unsigned long long generation);

#ifndef NTS_TYPES_ONLY

// Sets the node's state callback (fn NULL removes it). Once set, it is called once at once and then after each change.
// After nts_close returns (from any other thread), no callback runs. Returns 0, or -1 with the reason in err.
NTS_API int nts_watch(int node, nts_watch_fn fn, void* user, char* err, size_t err_size);

// Asks for a new login: after expiry (the node keeps its identity: the same keys and state directory, a new sign-in
// URL), after a refusal the person has fixed, or while it needs a login and tsnet has no URL. Returns 0 once asked
// (nts_login_url then gives the URL when it comes), or -1 with the reason in err.
NTS_API int nts_login(int node, char* err, size_t err_size);

#endif

// A peer on the tailnet.
typedef struct nts_peer {
    char dns_name[256];   // the tailnet's name for it, with no trailing dot: unique, and what peers are matched by
    char host_name[256];  // the name it asked for (a control server may rename a duplicate in dns_name)
    char addresses[128];  // its tailnet addresses, separated by spaces, IPv4 first
    char tags[512];       // its tags, separated by spaces, or empty
    int online;           // non-zero when control says it is online
} nts_peer;

// What who-is answers for a connection's remote address.
typedef struct nts_whois {
    char login[256];        // the owner's login name ("tagged-devices" for a tagged node), or empty
    char display_name[256]; // the owner's display name, or empty
    char node_name[256];    // the node's tailnet name with no trailing dot
    char tags[512];         // the node's tags, separated by spaces, or empty
} nts_whois;

#ifndef NTS_TYPES_ONLY

// The node's peers, up to max of them into out (out may be NULL when max is 0), in the order of their DNS names.
// Returns how many peers there are (more than max when out was too short), or -1 with the reason in err.
NTS_API int nts_peers(int node, nts_peer* out, int max, int timeout_ms, char* err, size_t err_size);

// The peer whose DNS name is dns_name (a trailing dot or none; letter case ignored). Returns 1 with it in out, 0 when
// there is none, or -1 with the reason in err.
NTS_API int nts_peer_find(int node, const char* dns_name, nts_peer* out, int timeout_ms, char* err, size_t err_size);

// Who is behind remote ("ip:port", a connection's remote address on the tailnet). Returns 1 with the answer in out, 0
// when the address is no node this one knows ("unknown" is an answer, not an error), or -1 with the reason in err.
NTS_API int nts_whois_addr(int node, const char* remote, nts_whois* out, int timeout_ms, char* err, size_t err_size);

// TCP through the node's embedded stack. A listener on port (1..65535) of the node's tailnet addresses: its handle, or
// 0 with the reason in err.
NTS_API int nts_listen_tcp(int node, int port, char* err, size_t err_size);

// The next connection on a listener: its handle, NTS_TIMEOUT, or NTS_ERROR with the reason in err.
NTS_API int nts_accept(int listener, int timeout_ms, char* err, size_t err_size);

// A TCP connection to address ("name:port" or "ip:port"; a name is resolved through the tailnet's DNS, never the
// host's): its handle, or 0 with the reason in err (NTS_TIMEOUT's meaning is said in err).
NTS_API int nts_dial_tcp(int node, const char* address, int timeout_ms, char* err, size_t err_size);

// UDP through the node's embedded stack: a connected socket to address (as nts_dial_tcp), read and written with
// nts_read and nts_write, one datagram a call. Its handle, or 0 with the reason in err.
NTS_API int nts_dial_udp(int node, const char* address, int timeout_ms, char* err, size_t err_size);

// A UDP socket on port (1..65535) of the node's tailnet IPv4 address: its handle, or 0 with the reason in err.
NTS_API int nts_listen_udp(int node, int port, char* err, size_t err_size);

// Reads into buf from a connection (TCP: what has arrived, up to size; UDP: one datagram, cut to size). Returns the
// count (0 at the end of a TCP stream), NTS_TIMEOUT, or NTS_ERROR with the reason in err.
NTS_API long long nts_read(int conn, void* buf, size_t size, int timeout_ms, char* err, size_t err_size);

// Writes size bytes of buf to a connection (UDP: one datagram). Returns the count written, NTS_TIMEOUT (some bytes
// may have gone), or NTS_ERROR with the reason in err.
NTS_API long long nts_write(int conn, const void* buf, size_t size, int timeout_ms, char* err, size_t err_size);

// One datagram from a UDP socket into buf (cut to size), with its sender's "ip:port" into from. Returns the count,
// NTS_TIMEOUT, or NTS_ERROR with the reason in err.
NTS_API long long nts_recv_from(int socket, void* buf, size_t size, int timeout_ms, char* from, size_t from_size,
                                char* err, size_t err_size);

// One datagram from a UDP socket to address ("ip:port", or "name:port" resolved through the tailnet's peers). Returns
// the count sent, NTS_TIMEOUT, or NTS_ERROR with the reason in err.
NTS_API long long nts_send_to(int socket, const void* buf, size_t size, const char* address, int timeout_ms,
                              char* err, size_t err_size);

// A listener's, connection's or UDP socket's own address, and a connection's remote one, as "ip:port". Returns 0, or
// -1 with the reason in err.
NTS_API int nts_local_addr(int handle, char* out, size_t out_size, char* err, size_t err_size);
NTS_API int nts_remote_addr(int conn, char* out, size_t out_size, char* err, size_t err_size);

// Closes a listener, connection or UDP socket (a node is closed by nts_close). Returns 0, or -1 with the reason in err
// (closed already, or not such a handle).
NTS_API int nts_handle_close(int handle, char* err, size_t err_size);

#endif

#ifdef __cplusplus
}
#endif

#endif
