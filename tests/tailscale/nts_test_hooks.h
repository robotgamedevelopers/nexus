// The test hooks of the shim's test build (src/tailscale/shim/hooks_on.go, the nts_testhooks build tag), linked only
// into the test program (nexus_link_tailscale(<target> TESTHOOKS), cmake/tailscale.cmake). nexus's build has none of
// them.
#ifndef NEXUS_TAILSCALE_NTS_TEST_HOOKS_H
#define NEXUS_TAILSCALE_NTS_TEST_HOOKS_H

#include "nts.h"

#ifdef __cplusplus
extern "C" {
#endif

// Makes the export named where ("nts_start", "nts_close", ...; "nts_test_start_offline after open" panics after the
// state directory is held and its log opened) panic on every later call, until called again with NULL.
NTS_API void nts_test_inject_panic(const char* where);

// nts_start with the control server http://127.0.0.1:9 (nothing listens; refused at once) and the port mapper off: a
// node that reaches no other machine. The same checks and refusals as nts_start.
NTS_API int nts_test_start_offline(const char* dir, const char* hostname, char* err, size_t err_size);

// Writes text through Go's log package and gVisor's logger (warning and info).
NTS_API void nts_test_log_libraries(const char* text);

#ifdef __cplusplus
}
#endif

#endif
