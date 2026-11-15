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
	"io"
	"log"
	"os"
	"runtime"
	"runtime/debug"
	"sync"
	"time"
	"unsafe"

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

// node is one started tsnet server and the log file it writes.
type node struct {
	server *tsnet.Server
	log    *os.File
}

var (
	mu       sync.Mutex
	nodes    = map[int]*node{}
	lastNode = 0

	// versionText is nts_version's answer, made once and never freed.
	versionText *C.char
)

func init() {
	// Nothing is ever uploaded to Tailscale's log service: set before any tsnet code runs.
	envknob.SetNoLogsNoSupport()
	// Go's log package writes to a node's log file while one runs (nts_start), and nowhere otherwise: never stderr.
	log.SetOutput(io.Discard)
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

//export nts_version
func nts_version() *C.char {
	return versionText
}

//export nts_prepare
func nts_prepare(dir *C.char, errOut *C.char, errSize C.size_t) C.int {
	if dir == nil {
		write(errOut, errSize, "nts_prepare: no state directory given (a null path); pass an absolute directory")
		return -1
	}
	f, err := prepare(C.GoString(dir))
	if err != nil {
		write(errOut, errSize, err.Error())
		return -1
	}
	f.Close()
	write(errOut, errSize, "")
	return 0
}

//export nts_start
func nts_start(dir, hostname *C.char, errOut *C.char, errSize C.size_t) C.int {
	// Checked before anything else, so that a start that would sign in by a key, or reach another control server,
	// never begins.
	for _, name := range refusedEnvironment {
		if environmentHas(name) {
			write(errOut, errSize, fmt.Sprintf("nts_start: %s is set in this process's environment, and tsnet would use "+
				"it; nexus signs in interactively only (docs/tailscale.md), so unset %s and start again", name, name))
			return 0
		}
	}
	if dir == nil || hostname == nil || C.GoString(hostname) == "" {
		write(errOut, errSize, "nts_start: a state directory and a hostname are both needed; pass an absolute "+
			"directory and a non-empty name")
		return 0
	}
	stateDir := C.GoString(dir)
	logFile, err := prepare(stateDir)
	if err != nil {
		write(errOut, errSize, err.Error())
		return 0
	}
	// tsnet's own lines go to the file: never to stderr.
	logger := log.New(logFile, "", log.LstdFlags|log.LUTC|log.Lmicroseconds)
	server := &tsnet.Server{
		Dir:      stateDir,
		Hostname: C.GoString(hostname),
		UserLogf: logger.Printf,
	}
	if err := server.Start(); err != nil {
		server.Close()
		logFile.Close()
		write(errOut, errSize, fmt.Sprintf("nts_start: tsnet did not start on %s: %v; see %s", stateDir, err,
			logFile.Name()))
		return 0
	}
	mu.Lock()
	defer mu.Unlock()
	lastNode++
	nodes[lastNode] = &node{server: server, log: logFile}
	// Anything tsnet's packages print through Go's log package goes to the newest node's file too.
	log.SetOutput(logFile)
	log.SetFlags(log.LstdFlags | log.LUTC | log.Lmicroseconds)
	write(errOut, errSize, "")
	return C.int(lastNode)
}

func find(handle C.int) *node {
	mu.Lock()
	defer mu.Unlock()
	return nodes[int(handle)]
}

//export nts_backend_state
func nts_backend_state(handle C.int, out *C.char, outSize C.size_t) C.int {
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
func nts_close(handle C.int, errOut *C.char, errSize C.size_t) C.int {
	mu.Lock()
	n := nodes[int(handle)]
	delete(nodes, int(handle))
	if n != nil {
		quiet := io.Writer(io.Discard)
		for _, other := range nodes {
			quiet = other.log
			break
		}
		log.SetOutput(quiet)
	}
	mu.Unlock()
	if n == nil {
		write(errOut, errSize, fmt.Sprintf("nts_close: %d is not a started node's handle (closed already, or never "+
			"started)", int(handle)))
		return -1
	}
	err := n.server.Close()
	n.log.Close()
	if err != nil {
		write(errOut, errSize, fmt.Sprintf("nts_close: %v", err))
		return -1
	}
	write(errOut, errSize, "")
	return 0
}

func main() {}
