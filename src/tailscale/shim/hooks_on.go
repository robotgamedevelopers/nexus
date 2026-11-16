//go:build nts_testhooks

// Test hooks, in the test program's build of the shim only (the nts_testhooks tag, set by cmake/tailscale.cmake for
// nexus_tailscale_testhooks and never for nexus_tailscale). They are declared in tests/tailscale/nts_test_hooks.h.

package main

/*
#include <stddef.h>
#define NTS_TYPES_ONLY
#include "../nts.h"
*/
import "C"

import (
	"fmt"
	"log"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"

	gvlog "gvisor.dev/gvisor/pkg/log"
	"tailscale.com/envknob"
	"tailscale.com/ipn"
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

// nts_test_start_offline is nts_start, every check and refusal the same, with the control server offlineControl (the
// params' control_url is ignored) and the port mapper off (TS_DISABLE_PORTMAPPER), so that the node touches nothing
// off this machine.
//
//export nts_test_start_offline
func nts_test_start_offline(params *C.nts_start_params, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_start_offline", errOut, errSize, &rc, 0)
	if params == nil {
		write(errOut, errSize, "nts_test_start_offline: no parameters given (a null pointer)")
		return 0
	}
	forcePortMapperOff()
	p := startParams{dir: goString(params.dir), hostname: goString(params.hostname), control: offlineControl,
		authKey: goString(params.auth_key), ephemeral: params.ephemeral != 0}
	return startNode("nts_test_start_offline", p, offlineControl, errOut, errSize)
}

// forcePortMapperOff turns the port mapper off for the test build's loopback nodes: TS_DISABLE_PORTMAPPER=true, which
// the start's environment check then takes as the test's own (testForced) rather than refusing it.
func forcePortMapperOff() {
	testForcedMu.Lock()
	testForced["TS_DISABLE_PORTMAPPER"] = "true"
	testForcedMu.Unlock()
	envknob.Setenv("TS_DISABLE_PORTMAPPER", "true")
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

// nts_test_net_calls is how many calls into tsnet that send (a dial, a datagram sent) the shim has made in this
// process: a refused dial or send leaves it as it was.
//
//export nts_test_net_calls
func nts_test_net_calls() C.longlong {
	return C.longlong(netCalls.Load())
}

// nts_test_inject_control_error hands the node text as if control had sent it as an error (tsnet's ErrMessage
// notification), through the same path a real one takes.
//
//export nts_test_inject_control_error
func nts_test_inject_control_error(id C.int, text *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_inject_control_error", errOut, errSize, &rc, -1)
	n := nodeOf("nts_test_inject_control_error", id, errOut, errSize)
	if n == nil {
		return -1
	}
	message := goString(text)
	n.fromNotify(ipn.Notify{ErrMessage: &message})
	write(errOut, errSize, "")
	return 0
}

// hostTraps are listeners on the host's loopback (TCP and UDP on one port) that count what reaches them: a test dials
// them through a node, and shows the dial was refused before anything was sent.
var (
	trapMu    sync.Mutex
	trapCount atomic.Int64
	trapTCP   net.Listener
	trapUDP   net.PacketConn
)

// nts_test_host_trap opens the traps on 127.0.0.1 (TCP and UDP on the same free port) and returns the port, or -1.
//
//export nts_test_host_trap
func nts_test_host_trap(errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_host_trap", errOut, errSize, &rc, -1)
	trapMu.Lock()
	defer trapMu.Unlock()
	if trapTCP != nil {
		write(errOut, errSize, "")
		return C.int(trapTCP.Addr().(*net.TCPAddr).Port)
	}
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		failf(errOut, errSize, "nts_test_host_trap", "%v", err)
		return -1
	}
	port := ln.Addr().(*net.TCPAddr).Port
	pc, err := net.ListenPacket("udp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		ln.Close()
		failf(errOut, errSize, "nts_test_host_trap", "%v", err)
		return -1
	}
	trapTCP, trapUDP = ln, pc
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			trapCount.Add(1)
			c.Close()
		}
	}()
	go func() {
		buf := make([]byte, 2048)
		for {
			if _, _, err := pc.ReadFrom(buf); err != nil {
				return
			}
			trapCount.Add(1)
		}
	}()
	write(errOut, errSize, "")
	return C.int(port)
}

// nts_test_host_trap_count is how many connections and datagrams have reached the traps.
//
//export nts_test_host_trap_count
func nts_test_host_trap_count() C.longlong {
	return C.longlong(trapCount.Load())
}

// nts_test_dial_after_target calls dialPeer directly on addrPort, as when a peer was in target()'s snapshot and left
// the node's map just before the dial. Any connection opened writes one byte on UDP (so a host UDP socket reaches the
// trap) and closes at once.
//
//export nts_test_dial_after_target
func nts_test_dial_after_target(id C.int, network, addrPort *C.char, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_dial_after_target", errOut, errSize, &rc, 0)
	n := nodeOf("nts_test_dial_after_target", id, errOut, errSize)
	if n == nil {
		return 0
	}
	to, err := netip.ParseAddrPort(goString(addrPort))
	if err != nil {
		failf(errOut, errSize, "nts_test_dial_after_target", "%v", err)
		return 0
	}
	netName := goString(network)
	ctx, cancel := within(n, timeoutMs)
	defer cancel()
	c, err := dialPeer(ctx, n, netName, to)
	if err != nil {
		failf(errOut, errSize, "nts_test_dial_after_target", "%v", err)
		return 0
	}
	if netName == "udp" {
		_, _ = c.Write([]byte{'x'})
	}
	c.Close()
	write(errOut, errSize, "")
	return 1
}
