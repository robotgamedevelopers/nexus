//go:build nts_testhooks

// Test hooks, in the test program's build of the shim only (the nts_testhooks tag, set by cmake/tailscale.cmake for
// nexus_tailscale_testhooks and never for nexus_tailscale). They are declared in tests/tailscale/nts_test_hooks.h.

package main

/*
#include <stddef.h>
*/
import "C"

import (
	"fmt"
	"log"
	"sync"

	gvlog "gvisor.dev/gvisor/pkg/log"
	"tailscale.com/envknob"
)

var (
	panicMu sync.Mutex
	panicAt string
)

// testPanic panics when where is the export nts_test_inject_panic named.
func testPanic(where string) {
	panicMu.Lock()
	at := panicAt
	panicMu.Unlock()
	if at != "" && at == where {
		panic(fmt.Sprintf("nts test panic injected at %s", where))
	}
}

// nts_test_inject_panic makes the export named where (e.g. "nts_start") panic on every later call, until it is
// called again with NULL.
//
//export nts_test_inject_panic
func nts_test_inject_panic(where *C.char) {
	panicMu.Lock()
	defer panicMu.Unlock()
	if where == nil {
		panicAt = ""
		return
	}
	panicAt = C.GoString(where)
}

// offlineControl is a control server nothing answers: the discard port on loopback, refused at once. A node started
// with it never reaches Tailscale or any other machine.
const offlineControl = "http://127.0.0.1:9"

// nts_test_start_offline is nts_start, every check and refusal the same, with the control server offlineControl and the
// port mapper off (TS_DISABLE_PORTMAPPER), so that the node touches nothing off this machine.
//
//export nts_test_start_offline
func nts_test_start_offline(dir, hostname *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_start_offline", errOut, errSize, &rc, 0)
	envknob.Setenv("TS_DISABLE_PORTMAPPER", "true")
	return startNode("nts_test_start_offline", dir, hostname, offlineControl, errOut, errSize)
}

// nts_test_log_libraries writes text through each process-wide library logger the shim redirects: Go's log package
// and gVisor's logger (at warning and info).
//
//export nts_test_log_libraries
func nts_test_log_libraries(text *C.char) {
	line := C.GoString(text)
	log.Printf("go log: %s", line)
	gvlog.Warningf("gvisor warning: %s", line)
	gvlog.Infof("gvisor info: %s", line)
}
