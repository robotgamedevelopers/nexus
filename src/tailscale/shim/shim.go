// Package main is nexus's Tailscale shim: the C API of src/tailscale/nts.h over tsnet, built as a c-archive on macOS
// and Linux and as a DLL with an MSVC import library on Windows (cmake/tailscale.cmake). It is the same Go code
// everywhere. Layer 1 (docs/tailscale.md): start and close a node, its state (src/tailscale/lifecycle), peers, who-is,
// and TCP and UDP through the embedded stack (net.go).
package main

/*
#include <stddef.h>
#include <stdlib.h>
#define NTS_TYPES_ONLY
#include "../nts.h"
*/
import "C"

import (
	"context"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"sync"
	"time"
	"unsafe"

	"github.com/robotgamedevelopers/nexus/src/tailscale/lifecycle"
	gvlog "gvisor.dev/gvisor/pkg/log"
	"tailscale.com/envknob"
	"tailscale.com/tsnet"
)

// The variables tsnet reads when its own fields are empty: an auth key, an OAuth client, workload identity, or another
// control server. nexus gives tsnet its sign-in and control server only from nts_start's parameters, so a start
// refuses while any is set.
var refusedEnvironment = []string{
	"TS_AUTHKEY", "TS_AUTH_KEY", "TS_CLIENT_SECRET", "TS_CLIENT_ID", "TS_ID_TOKEN", "TS_AUDIENCE", "TS_CONTROL_URL",
}

// environmentHas reports whether name is set, as Go sees the environment (what tsnet reads) or as the C library does
// (what the host program has set). Either is enough to refuse.
func environmentHas(name string) bool {
	if _, set := os.LookupEnv(name); set {
		return true
	}
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	return C.getenv(cname) != nil
}

var (
	// orderMu guards order: the running nodes, oldest first. The library loggers' sink follows the last.
	orderMu sync.Mutex
	order   []*node

	// The static strings nts_version, nts_state_name and nts_reason_name return, made once and never freed.
	versionText *C.char
	stateNames  []*C.char
	reasonNames []*C.char
	unknownText *C.char
)

func init() {
	// Nothing is ever uploaded to Tailscale's log service: set before any tsnet code runs.
	envknob.SetNoLogsNoSupport()
	// The process-wide library loggers, Go's log package and gVisor's (which tailscale never redirects, and which
	// writes to stderr by default), write to sink: the newest running node's log file, or nowhere.
	log.SetOutput(sink)
	log.SetFlags(log.LstdFlags | log.LUTC | log.Lmicroseconds)
	gvlog.SetTarget(gvlog.GoogleEmitter{Writer: &gvlog.Writer{Next: sink}})
	versionText = C.CString(versions())
	for p := lifecycle.NotStarted; p <= lifecycle.Failed; p++ {
		stateNames = append(stateNames, C.CString(p.String()))
	}
	for r := lifecycle.ReasonNone; r <= lifecycle.ReasonControlStopped; r++ {
		reasonNames = append(reasonNames, C.CString(r.String()))
	}
	unknownText = C.CString("unknown")
}

// versions names the tailscale.com module and the Go toolchain this build linked, from the build's own record.
func versions() string {
	tailscale := "tailscale.com (unknown version)"
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, dep := range info.Deps {
			if dep.Path == "tailscale.com" {
				tailscale = "tailscale.com " + dep.Version
				if dep.Replace != nil {
					tailscale += " (replaced by " + dep.Replace.Path + " " + dep.Replace.Version + ")"
				}
			}
		}
	}
	return tailscale + " " + runtime.Version()
}

// write copies text into the C buffer out of size bytes, cut to fit and NUL-terminated. A null buffer or size 0 is
// allowed.
func write(out *C.char, size C.size_t, text string) {
	if out == nil || size == 0 {
		return
	}
	buffer := unsafe.Slice((*byte)(unsafe.Pointer(out)), int(size))
	n := copy(buffer[:len(buffer)-1], text)
	buffer[n] = 0
}

// recovered logs a panic stopped in the export name, and says so in the caller's buffer.
func recovered(name string, r any, errOut *C.char, errSize C.size_t) {
	fmt.Fprintf(sink, "%s: panic: %v\n%s", name, r, debug.Stack())
	write(errOut, errSize, name+": stopped by an internal error (a Go panic); its text is in the newest running "+
		"node's nexus-tailscale.log, or dropped when no node runs")
}

// recoverTo is deferred first in every export that returns an int: a panic in the call (not in a goroutine it
// started, which Go cannot recover from another goroutine) is stopped here, its text and stack go to sink only, the
// caller's buffer says what happened, and the export returns failed. Whatever the call had opened is released by its
// own defers first.
func recoverTo(name string, errOut *C.char, errSize C.size_t, rc *C.int, failed C.int) {
	if r := recover(); r != nil {
		recovered(name, r, errOut, errSize)
		*rc = failed
	}
}

// recoverLong is recoverTo for the exports that return a long long.
func recoverLong(name string, errOut *C.char, errSize C.size_t, rc *C.longlong) {
	if r := recover(); r != nil {
		recovered(name, r, errOut, errSize)
		*rc = C.NTS_ERROR
	}
}

// failf writes name's reason into the caller's buffer.
func failf(errOut *C.char, errSize C.size_t, name, format string, args ...any) {
	write(errOut, errSize, name+": "+fmt.Sprintf(format, args...))
}

// deadline is a call's timeout in milliseconds as a channel that fires when it passes: nil (never) for a negative
// one.
func deadline(timeoutMs C.int) (<-chan time.Time, func()) {
	if timeoutMs < 0 {
		return nil, func() {}
	}
	t := time.NewTimer(time.Duration(timeoutMs) * time.Millisecond)
	return t.C, func() { t.Stop() }
}

// within is a context for a call on n that ends with the call's timeout (none when negative) or when n closes.
func within(n *node, timeoutMs C.int) (context.Context, context.CancelFunc) {
	if timeoutMs < 0 {
		return context.WithCancel(n.ctx)
	}
	return context.WithTimeout(n.ctx, time.Duration(timeoutMs)*time.Millisecond)
}

//export nts_version
func nts_version() (v *C.char) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(sink, "nts_version: panic: %v\n%s", r, debug.Stack())
			v = versionText
		}
	}()
	testPanic("nts_version")
	return versionText
}

//export nts_state_name
func nts_state_name(state C.int) (v *C.char) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(sink, "nts_state_name: panic: %v\n%s", r, debug.Stack())
			v = unknownText
		}
	}()
	testPanic("nts_state_name")
	if state < 0 || int(state) >= len(stateNames) {
		return unknownText
	}
	return stateNames[state]
}

//export nts_reason_name
func nts_reason_name(reason C.int) (v *C.char) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(sink, "nts_reason_name: panic: %v\n%s", r, debug.Stack())
			v = unknownText
		}
	}()
	testPanic("nts_reason_name")
	if reason < 0 || int(reason) >= len(reasonNames) {
		return unknownText
	}
	return reasonNames[reason]
}

//export nts_prepare
func nts_prepare(dir *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_prepare", errOut, errSize, &rc, -1)
	testPanic("nts_prepare")
	if dir == nil {
		write(errOut, errSize, "nts_prepare: no state directory given (a null path); pass an absolute directory")
		return -1
	}
	held, logFile, err := prepare(cleanDir(C.GoString(dir)), issue())
	if err != nil {
		write(errOut, errSize, err.Error())
		return -1
	}
	logFile.Close()
	held.release()
	write(errOut, errSize, "")
	return 0
}

// startParams is nts_start_params in Go.
type startParams struct {
	dir, hostname, control, authKey string
	ephemeral                       bool
}

func goString(s *C.char) string {
	if s == nil {
		return ""
	}
	return C.GoString(s)
}

//export nts_start
func nts_start(params *C.nts_start_params, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_start", errOut, errSize, &rc, 0)
	testPanic("nts_start")
	if params == nil {
		write(errOut, errSize, "nts_start: no parameters given (a null pointer); pass an nts_start_params")
		return 0
	}
	p := startParams{dir: goString(params.dir), hostname: goString(params.hostname),
		control: goString(params.control_url), authKey: goString(params.auth_key), ephemeral: params.ephemeral != 0}
	return startNode("nts_start", p, plainControl(), errOut, errSize)
}

var (
	plainMu    sync.Mutex
	plainURL   string
)

// plainControl is the one plain http control URL nts_start accepts: the test build's own loopback control server
// while it runs (hooks_control.go), and never any in nexus's build, where nothing sets it.
func plainControl() string {
	plainMu.Lock()
	defer plainMu.Unlock()
	return plainURL
}

func setPlainControl(url string) {
	plainMu.Lock()
	defer plainMu.Unlock()
	plainURL = url
}

// startNode is nts_start, with plain the one http control URL a test build allows ("" in nexus's build). In order:
// the environment's refusals, the parameters', the state directory held (this process, then the OS lock), its log
// opened, its control server checked against its record, tsnet started. Everything it took is given back if it fails
// or panics.
func startNode(name string, p startParams, plain string, errOut *C.char, errSize C.size_t) C.int {
	testPanic(name)
	// Checked before anything else, so that a start that would sign in by another key, or reach another control
	// server, never begins.
	for _, variable := range refusedEnvironment {
		if environmentHas(variable) {
			failf(errOut, errSize, name, "%s is set in this process's environment, and tsnet would use it; nexus "+
				"takes the sign-in and the control server only from nts_start's parameters (docs/tailscale.md), so "+
				"unset %s and start again", variable, variable)
			return 0
		}
	}
	if p.dir == "" || p.hostname == "" {
		failf(errOut, errSize, name, "a state directory and a hostname are both needed; pass an absolute directory "+
			"and a non-empty name")
		return 0
	}
	control, err := lifecycle.ControlURL(p.control, plain)
	if err != nil {
		failf(errOut, errSize, name, "%v", err)
		return 0
	}
	if err := lifecycle.CheckAuthKey(control, p.authKey); err != nil {
		failf(errOut, errSize, name, "%v", err)
		return 0
	}
	stateDir := cleanDir(p.dir)
	n := newNode(issue())
	held, logFile, err := prepare(stateDir, n.id)
	if err != nil {
		write(errOut, errSize, err.Error())
		return 0
	}
	started := false
	defer func() {
		if !started {
			n.cancel()
			if n.server != nil {
				n.server.Close()
			}
			logFile.Close()
			held.release()
		}
	}()
	if err := checkControl(stateDir, control); err != nil {
		failf(errOut, errSize, name, "%v", err)
		return 0
	}
	n.held, n.log = held, logFile
	n.server = &tsnet.Server{
		Dir:        stateDir,
		Hostname:   p.hostname,
		ControlURL: control,
		AuthKey:    p.authKey,
		Ephemeral:  p.ephemeral,
		// tsnet's user log goes to the file, URLs withheld; its backend log nowhere (backendLog).
		UserLogf: userLog(logFile),
		Logf:     n.backendLog,
	}
	n.apply(lifecycle.Event{Kind: lifecycle.Start})
	testPanic(name + " after open")
	if err := n.server.Start(); err != nil {
		failf(errOut, errSize, name, "tsnet did not start on %s: %v; see %s", stateDir, err,
			filepath.Join(stateDir, logName))
		return 0
	}
	handlesMu.Lock()
	handles[n.id] = &handle{id: n.id, kind: kindNode, node: n}
	handlesMu.Unlock()
	orderMu.Lock()
	order = append(order, n)
	sink.set(logFile)
	orderMu.Unlock()
	started = true
	go n.notify()
	n.watch()
	write(errOut, errSize, "")
	return C.int(n.id)
}

// nodeOf finds the open node id names.
func nodeOf(name string, id C.int, errOut *C.char, errSize C.size_t) *node {
	h, err := lookup(int(id), kindNode)
	if err != nil {
		failf(errOut, errSize, name, "%v", err)
		return nil
	}
	return h.node
}

//export nts_close
func nts_close(id C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_close", errOut, errSize, &rc, -1)
	testPanic("nts_close")
	handlesMu.Lock()
	h, err := lookupLocked(int(id), kindNode)
	var children []*handle
	if err == nil {
		delete(handles, h.id)
		for cid, child := range h.node.children {
			delete(handles, cid)
			children = append(children, child)
		}
		h.node.children = map[int]*handle{}
	}
	handlesMu.Unlock()
	if err != nil {
		failf(errOut, errSize, "nts_close", "%v", err)
		return -1
	}
	n := h.node
	n.apply(lifecycle.Event{Kind: lifecycle.Close})
	n.cancel()
	for _, child := range children {
		child.shut()
	}
	close(n.stop)
	if threadID() != n.notifier.Load() {
		// Called from any thread but the callback's own: no callback runs once this returns.
		<-n.notifierDone
	}
	closeErr := n.server.Close()
	n.watchersDone.Wait()
	// The library loggers move to the newest node still running (or nowhere) only once this node's server, which
	// may log while it closes, is down.
	orderMu.Lock()
	for i, o := range order {
		if o == n {
			order = append(order[:i], order[i+1:]...)
			break
		}
	}
	if len(order) == 0 {
		sink.set(nil)
	} else {
		sink.set(order[len(order)-1].log)
	}
	orderMu.Unlock()
	n.log.Close()
	n.held.release()
	if closeErr != nil {
		failf(errOut, errSize, "nts_close", "%v", closeErr)
		return -1
	}
	write(errOut, errSize, "")
	return 0
}

//export nts_status_get
func nts_status_get(id C.int, out *C.nts_status, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_status_get", errOut, errSize, &rc, -1)
	testPanic("nts_status_get")
	if out == nil {
		write(errOut, errSize, "nts_status_get: no nts_status to fill (a null pointer)")
		return -1
	}
	n := nodeOf("nts_status_get", id, errOut, errSize)
	if n == nil {
		return -1
	}
	state, expiry, _ := n.snapshot()
	fill(out, state, expiry)
	write(errOut, errSize, "")
	return 0
}

//export nts_status_wait
func nts_status_wait(id C.int, after C.ulonglong, timeoutMs C.int, out *C.nts_status, errOut *C.char,
	errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_status_wait", errOut, errSize, &rc, -1)
	testPanic("nts_status_wait")
	if out == nil {
		write(errOut, errSize, "nts_status_wait: no nts_status to fill (a null pointer)")
		return -1
	}
	n := nodeOf("nts_status_wait", id, errOut, errSize)
	if n == nil {
		return -1
	}
	timeout, stop := deadline(timeoutMs)
	defer stop()
	for {
		state, expiry, changed := n.snapshot()
		if state.Gen > uint64(after) {
			fill(out, state, expiry)
			write(errOut, errSize, "")
			return 0
		}
		select {
		case <-changed:
		case <-timeout:
			fill(out, state, expiry)
			write(errOut, errSize, "")
			return C.NTS_TIMEOUT
		}
	}
}

//export nts_login_url
func nts_login_url(id C.int, url *C.char, urlSize C.size_t, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_login_url", errOut, errSize, &rc, -1)
	testPanic("nts_login_url")
	n := nodeOf("nts_login_url", id, errOut, errSize)
	if n == nil {
		return -1
	}
	state, _, _ := n.snapshot()
	if state.Phase != lifecycle.NeedsLogin || state.URL == "" {
		failf(errOut, errSize, "nts_login_url", "no sign-in URL now (the node is %s); one is given only in "+
			"needs-login, once tsnet has it", state.Phase)
		return -1
	}
	if url == nil || int(urlSize) < len(state.URL)+1 {
		failf(errOut, errSize, "nts_login_url", "the buffer holds %d bytes and the URL needs %d; the URL is never "+
			"cut", int(urlSize), len(state.URL)+1)
		return -1
	}
	write(url, urlSize, state.URL)
	write(errOut, errSize, "")
	return 0
}

//export nts_watch
func nts_watch(id C.int, fn C.nts_watch_fn, user unsafe.Pointer, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_watch", errOut, errSize, &rc, -1)
	testPanic("nts_watch")
	n := nodeOf("nts_watch", id, errOut, errSize)
	if n == nil {
		return -1
	}
	n.mu.Lock()
	n.watchFn, n.watchUser, n.poked = fn, user, fn != nil
	n.mu.Unlock()
	select {
	case n.poke <- struct{}{}:
	default:
	}
	write(errOut, errSize, "")
	return 0
}

//export nts_login
func nts_login(id C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_login", errOut, errSize, &rc, -1)
	testPanic("nts_login")
	n := nodeOf("nts_login", id, errOut, errSize)
	if n == nil {
		return -1
	}
	ctx, cancel := context.WithTimeout(n.ctx, 30*time.Second)
	defer cancel()
	if err := n.login(ctx); err != nil {
		failf(errOut, errSize, "nts_login", "tsnet did not start a login: %v", err)
		return -1
	}
	write(errOut, errSize, "")
	return 0
}

func main() {}
