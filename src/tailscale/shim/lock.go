package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

// errInUse is lockFile's answer when another open of the lock file holds it.
var errInUse = errors.New("in use")

// A state directory is one node's at a time: two tsnet servers on one directory would share its keys and state files
// and corrupt both. A start holds the directory for the node's life in two ways, each checked before anything in the
// directory is opened: an entry in reserved (this process), then an OS lock on <dir>/nexus-tailscale.lock (any
// process; flock on macOS and Linux, LockFileEx on Windows, dropped by the OS if the process ends).
var (
	reservedMu sync.Mutex
	reserved   = map[string]bool{}
)

// dirKey is the name a directory is reserved under: cleaned, and on Windows, whose paths ignore case, lower-cased.
func dirKey(dir string) string {
	key := filepath.Clean(dir)
	if runtime.GOOS == "windows" {
		key = strings.ToLower(key)
	}
	return key
}

// dirLock is a state directory held by this process.
type dirLock struct {
	key  string
	file *os.File
}

// reserve claims dir in this process, or refuses if a node here holds it. It touches nothing on disk.
func reserve(dir string) (*dirLock, error) {
	key := dirKey(dir)
	reservedMu.Lock()
	defer reservedMu.Unlock()
	if reserved[key] {
		return nil, fmt.Errorf("the state directory %s is in use by a node this process started; close that node "+
			"first, or give another directory", dir)
	}
	reserved[key] = true
	return &dirLock{key: key}, nil
}

// lockOS takes the OS lock on dir's lock file, which ensureDir has made exist. It refuses if another process holds it.
func (l *dirLock) lockOS(dir string) error {
	path := filepath.Join(dir, lockName)
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		return fmt.Errorf("cannot open the lock file %s: %v; the state directory must be writable by this user",
			path, err)
	}
	if err := ownerOnlyFile(f); err != nil {
		f.Close()
		return err
	}
	if err := lockFile(f); err != nil {
		f.Close()
		if errors.Is(err, errInUse) {
			return fmt.Errorf("the state directory %s is in use by another process's node (it holds %s); stop "+
				"that process's node first, or give another directory", dir, path)
		}
		return err
	}
	l.file = f
	return nil
}

// release drops the OS lock (closing the file drops it) and the reservation. The lock file itself is left in place:
// removing it would let a start that opened it a moment before lock a file no longer in the directory.
func (l *dirLock) release() {
	if l == nil {
		return
	}
	if l.file != nil {
		l.file.Close()
		l.file = nil
	}
	reservedMu.Lock()
	delete(reserved, l.key)
	reservedMu.Unlock()
}

// hold is reserve, ensureDir and lockOS in turn, released again on any failure: what a start or a prepare does before
// it opens anything else in dir.
func hold(dir string) (*dirLock, error) {
	l, err := reserve(dir)
	if err != nil {
		return nil, err
	}
	if err := ensureDir(dir); err != nil {
		l.release()
		return nil, err
	}
	if err := l.lockOS(dir); err != nil {
		l.release()
		return nil, err
	}
	return l, nil
}
