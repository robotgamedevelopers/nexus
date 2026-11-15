// Package main is nexus's Tailscale shim: the C API of src/tailscale/nts.h over tsnet, built as a c-archive on macOS
// and Linux and as a DLL with an MSVC import library on Windows (cmake/tailscale.cmake). It is the same Go code
// everywhere. Layer 0 (docs/tailscale.md): the version, the state directory, start and close.
package main

/*
#include <stddef.h>
#include <stdlib.h>
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

	gvlog "gvisor.dev/gvisor/pkg/log"
	"tailscale.com/envknob"
	"tailscale.com/tsnet"
)

// The variables tsnet reads when its own fields are empty: an auth key, an OAuth client, workload identity, or another
// control server. nexus signs in interactively only (docs/tailscale.md, path A), so a start refuses while any is set.
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

// node is one started tsnet server, the state directory it holds and the log file it writes.
type node struct {
	server *tsnet.Server
	held   *dirLock
	log    *cappedLog
}

var (
	mu       sync.Mutex
	nodes    = map[int]*node{}
	order    []int // the running nodes' handles, oldest first: sink follows the last
	lastNode = 0

	// versionText is nts_version's answer, made once and never freed.
	versionText *C.char
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

// recoverTo is deferred first in every export: a panic in the call (not in a goroutine it started, which Go cannot
// recover from another goroutine) is stopped here, its text and stack go to sink only, the caller's buffer says what
// happened, and the export returns failed. Whatever the call had opened is released by its own defers first.
func recoverTo(name string, errOut *C.char, errSize C.size_t, rc *C.int, failed C.int) {
	r := recover()
	if r == nil {
		return
	}
	fmt.Fprintf(sink, "%s: panic: %v\n%s", name, r, debug.Stack())
	write(errOut, errSize, name+": stopped by an internal error (a Go panic); its text is in the newest running "+
		"node's nexus-tailscale.log, or dropped when no node runs")
	*rc = failed
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

//export nts_prepare
func nts_prepare(dir *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_prepare", errOut, errSize, &rc, -1)
	testPanic("nts_prepare")
	if dir == nil {
		write(errOut, errSize, "nts_prepare: no state directory given (a null path); pass an absolute directory")
		return -1
	}
	held, logFile, err := prepare(cleanDir(C.GoString(dir)))
	if err != nil {
		write(errOut, errSize, err.Error())
		return -1
	}
	logFile.Close()
	held.release()
	write(errOut, errSize, "")
	return 0
}

//export nts_start
func nts_start(dir, hostname *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_start", errOut, errSize, &rc, 0)
	return startNode("nts_start", dir, hostname, "", errOut, errSize)
}

// startNode is nts_start, with controlURL for tsnet's ControlURL (empty: Tailscale's own). In order: the environment
// refusals, the arguments, the state directory held (this process, then the OS lock), its log opened, tsnet started.
// Everything it took is given back if it fails or panics.
func startNode(name string, dir, hostname *C.char, controlURL string, errOut *C.char, errSize C.size_t) C.int {
	testPanic(name)
	// Checked before anything else, so that a start that would sign in by a key, or reach another control server,
	// never begins.
	for _, variable := range refusedEnvironment {
		if environmentHas(variable) {
			write(errOut, errSize, fmt.Sprintf("%s: %s is set in this process's environment, and tsnet would use "+
				"it; nexus signs in interactively only (docs/tailscale.md), so unset %s and start again", name,
				variable, variable))
			return 0
		}
	}
	if dir == nil || hostname == nil || C.GoString(hostname) == "" {
		write(errOut, errSize, name+": a state directory and a hostname are both needed; pass an absolute "+
			"directory and a non-empty name")
		return 0
	}
	stateDir := cleanDir(C.GoString(dir))
	held, logFile, err := prepare(stateDir)
	if err != nil {
		write(errOut, errSize, err.Error())
		return 0
	}
	var server *tsnet.Server
	started := false
	defer func() {
		if !started {
			if server != nil {
				server.Close()
			}
			logFile.Close()
			held.release()
		}
	}()
	// tsnet's user log goes to the file: never to stderr.
	logger := log.New(logFile, "", log.LstdFlags|log.LUTC|log.Lmicroseconds)
	server = &tsnet.Server{
		Dir:        stateDir,
		Hostname:   C.GoString(hostname),
		UserLogf:   logger.Printf,
		ControlURL: controlURL,
	}
	testPanic(name + " after open")
	if err := server.Start(); err != nil {
		write(errOut, errSize, fmt.Sprintf("%s: tsnet did not start on %s: %v; see %s", name, stateDir, err,
			filepath.Join(stateDir, logName)))
		return 0
	}
	mu.Lock()
	defer mu.Unlock()
	lastNode++
	nodes[lastNode] = &node{server: server, held: held, log: logFile}
	order = append(order, lastNode)
	sink.set(logFile)
	started = true
	write(errOut, errSize, "")
	return C.int(lastNode)
}

func find(handle C.int) *node {
	mu.Lock()
	defer mu.Unlock()
	return nodes[int(handle)]
}

//export nts_backend_state
func nts_backend_state(handle C.int, out *C.char, outSize C.size_t) (rc C.int) {
	defer recoverTo("nts_backend_state", out, outSize, &rc, -1)
	testPanic("nts_backend_state")
	n := find(handle)
	if n == nil {
		write(out, outSize, fmt.Sprintf("nts_backend_state: %d is not a started node's handle", int(handle)))
		return -1
	}
	client, err := n.server.LocalClient()
	if err != nil {
		write(out, outSize, fmt.Sprintf("nts_backend_state: no local client: %v", err))
		return -1
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	status, err := client.StatusWithoutPeers(ctx)
	if err != nil {
		write(out, outSize, fmt.Sprintf("nts_backend_state: status: %v", err))
		return -1
	}
	write(out, outSize, status.BackendState)
	return 0
}

//export nts_close
func nts_close(handle C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_close", errOut, errSize, &rc, -1)
	testPanic("nts_close")
	mu.Lock()
	n := nodes[int(handle)]
	if n != nil {
		delete(nodes, int(handle))
		for i, h := range order {
			if h == int(handle) {
				order = append(order[:i], order[i+1:]...)
				break
			}
		}
	}
	mu.Unlock()
	if n == nil {
		write(errOut, errSize, fmt.Sprintf("nts_close: %d is not a started node's handle (closed already, or never "+
			"started)", int(handle)))
		return -1
	}
	err := n.server.Close()
	// The library loggers move to the newest node still running (or nowhere) only once this node's server, which
	// may log while it closes, is down.
	mu.Lock()
	if len(order) == 0 {
		sink.set(nil)
	} else {
		sink.set(nodes[order[len(order)-1]].log)
	}
	mu.Unlock()
	n.log.Close()
	n.held.release()
	if err != nil {
		write(errOut, errSize, fmt.Sprintf("nts_close: %v", err))
		return -1
	}
	write(errOut, errSize, "")
	return 0
}

func main() {}
