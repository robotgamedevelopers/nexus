package main

import (
	"fmt"
	"io"
	"os"
	"sync"

	"github.com/robotgamedevelopers/nexus/src/tailscale/lifecycle"
)

// logCap is the size at which a node's log file is rotated: nexus-tailscale.log becomes nexus-tailscale.log.1
// (replacing an older one) and a new nexus-tailscale.log is begun, so a node's log takes at most twice this. 1 MiB is
// a guess, not a measurement: room for many days of tsnet's user log at its usual few lines an hour.
const logCap = 1 << 20

// cappedLog is a node's log file, written by its tsnet logger and, while it is the newest node, by every library
// logger in the process (sink). Writes are serialized.
type cappedLog struct {
	mu   sync.Mutex
	path string
	f    *os.File
	size int64
}

// openLog opens (creating) the log file at path for appending, owner-only.
func openLog(path string) (*cappedLog, error) {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o600)
	if err != nil {
		return nil, fmt.Errorf("cannot write the log file %s: %v; the state directory must be writable by this user",
			path, err)
	}
	if err := ownerOnlyFile(f); err != nil {
		f.Close()
		return nil, err
	}
	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("cannot read the log file %s: %v", path, err)
	}
	return &cappedLog{path: path, f: f, size: info.Size()}, nil
}

// Write appends p scrubbed (scrub: no URL, no auth key of a node of this process, no Tailscale key), first rotating
// the file when it would take it past logCap. Everything that reaches a log file comes through here: tsnet's user log,
// Go's log package, gVisor's logger and the text of a recovered panic. If the new file cannot be opened, the log stops
// (its lines are dropped) rather than going anywhere else. It reports all of p written.
func (l *cappedLog) Write(p []byte) (int, error) {
	text := []byte(scrub(string(p)))
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.f == nil {
		return len(p), nil
	}
	if l.size > 0 && l.size+int64(len(text)) > logCap {
		l.rotate()
		if l.f == nil {
			return len(p), nil
		}
	}
	n, err := l.f.Write(text)
	l.size += int64(n)
	if err != nil {
		return 0, err
	}
	return len(p), nil
}

// The auth keys of this process's nodes, by node id, from the start until the node closes: scrub withholds them
// wherever they would be written or shown.
var (
	secretsMu sync.Mutex
	secrets   = map[int]string{}
)

func addSecret(id int, key string) {
	if key == "" {
		return
	}
	secretsMu.Lock()
	secrets[id] = key
	secretsMu.Unlock()
}

func dropSecret(id int) {
	secretsMu.Lock()
	delete(secrets, id)
	secretsMu.Unlock()
}

// scrub is text with every URL, every auth key of a node of this process and every Tailscale key withheld
// (lifecycle.Scrub), uncut.
func scrub(text string) string {
	return scrubCut(text, 0)
}

// scrubCut is scrub, cut to at most max bytes (none when max is 0).
func scrubCut(text string, max int) string {
	secretsMu.Lock()
	list := make([]string, 0, len(secrets))
	for _, s := range secrets {
		list = append(list, s)
	}
	secretsMu.Unlock()
	return lifecycle.Scrub(text, list, max)
}

// rotate moves the file to path.1 and begins a new one. Windows cannot rename over an existing file, hence the
// remove first.
func (l *cappedLog) rotate() {
	old := l.path + ".1"
	l.f.Close()
	l.f = nil
	os.Remove(old)
	os.Rename(l.path, old)
	next, err := openLog(l.path)
	if err != nil {
		return
	}
	l.f, l.size = next.f, next.size
}

// Close closes the file; later writes are dropped.
func (l *cappedLog) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.f == nil {
		return nil
	}
	err := l.f.Close()
	l.f = nil
	return err
}

// switchWriter is a writer whose target can be changed while others write to it.
type switchWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (s *switchWriter) Write(p []byte) (int, error) {
	s.mu.Lock()
	w := s.w
	s.mu.Unlock()
	return w.Write(p)
}

// set makes w the target; nil is nowhere.
func (s *switchWriter) set(w *cappedLog) {
	var target io.Writer = io.Discard
	if w != nil {
		target = w
	}
	s.mu.Lock()
	s.w = target
	s.mu.Unlock()
}

// sink is where the process-wide library loggers write (Go's log package and gVisor's logger, both pointed here in
// init): the newest running node's log file, or nowhere while none runs. Never stderr or stdout.
var sink = &switchWriter{w: io.Discard}
