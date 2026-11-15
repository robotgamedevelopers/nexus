// The test hooks of the shim's test build (src/tailscale/shim/hooks_on.go and hooks_control.go, the nts_testhooks
// build tag), linked only into the test program (nexus_link_tailscale(<target> TESTHOOKS), cmake/tailscale.cmake).
// nexus's build has none of them.
#ifndef NEXUS_TAILSCALE_NTS_TEST_HOOKS_H
#define NEXUS_TAILSCALE_NTS_TEST_HOOKS_H

#include "nts.h"

#ifdef __cplusplus
extern "C" {
#endif

// Makes the export named where ("nts_start", "nts_close", ...; "nts_start after open" panics after the state directory
// is held and its log opened) panic on every later call, until called again with NULL.
NTS_API void nts_test_inject_panic(const char* where);

// nts_start with the control server http://127.0.0.1:9 (nothing listens; refused at once; params->control_url is
// ignored) and the port mapper off: a node that reaches no other machine. The same checks and refusals as nts_start.
NTS_API int nts_test_start_offline(const nts_start_params* params, char* err, size_t err_size);

// Writes text through Go's log package and gVisor's logger (warning and info).
NTS_API void nts_test_log_libraries(const char* text);

// A control server of the test program's own, on 127.0.0.1 in this process: Tailscale's in-tree test control server
// (tstest/integration/testcontrol at the pinned tailscale.com) with a DERP relay and a STUN server beside it. One at a
// time. Starting it turns the port mapper off and makes tailscale's sockets plain ones, for the rest of the process.
// Its URL (http://127.0.0.1:<port>) goes into url, and nts_start accepts it as a control URL while it runs. With
// require_auth, a node without a key gets a sign-in URL; with auth_key non-empty, a node must present that key.
NTS_API int nts_test_control_start(int require_auth, const char* auth_key, char* url, size_t url_size, char* err,
                                   size_t err_size);

// Completes the sign-in at login_url, as a person approving it would; first gives an unexpired key back to every node
// whose key expired, as an admin would.
NTS_API int nts_test_control_approve(const char* login_url, char* err, size_t err_size);

// Expires the key of the node named hostname (a minute ago).
NTS_API int nts_test_control_expire(const char* hostname, char* err, size_t err_size);

// Gives the node named hostname the tags in tags, separated by spaces ("tag:a tag:b").
NTS_API int nts_test_control_set_tags(const char* hostname, const char* tags, char* err, size_t err_size);

// Takes the control server off the network (its port closed, its connections cut; the relay stays), and brings it
// back on the same port with everything it knew.
NTS_API int nts_test_control_down(char* err, size_t err_size);
NTS_API int nts_test_control_up(char* err, size_t err_size);

// The DERP relay's https URL (https://127.0.0.1:<port>), whose certificate no one trusts: a control URL that a node
// refuses on its certificate.
NTS_API int nts_test_control_untrusted_url(char* url, size_t url_size, char* err, size_t err_size);

// Stops the control server, its relay and its STUN server.
NTS_API int nts_test_control_stop(char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
