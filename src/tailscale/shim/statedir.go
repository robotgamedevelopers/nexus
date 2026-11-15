package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// logName is the file under the state directory that tsnet's log goes to (nts.h).
const logName = "nexus-tailscale.log"

// prepare makes dir a node's state directory: created with any missing parents, owner-only (ownerOnly, per OS), and
// its log file opened for appending, owner-only too. It touches no network. The file is the caller's to close.
func prepare(dir string) (*os.File, error) {
	if dir == "" {
		return nil, fmt.Errorf("no state directory given (an empty path); pass an absolute directory")
	}
	if !filepath.IsAbs(dir) {
		return nil, fmt.Errorf("the state directory %q is not an absolute path; pass an absolute directory", dir)
	}
	if info, err := os.Stat(dir); err == nil && !info.IsDir() {
		return nil, fmt.Errorf("the state directory %s is not a directory; remove what is there or give another path",
			dir)
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("cannot create the state directory %s: %v; give a directory this user may create, "+
			"or fix the permissions of its parent", dir, err)
	}
	info, err := os.Stat(dir)
	if err != nil {
		return nil, fmt.Errorf("cannot read the state directory %s: %v", dir, err)
	}
	if !info.IsDir() {
		return nil, fmt.Errorf("the state directory %s is not a directory; remove what is there or give another path",
			dir)
	}
	if err := ownerOnly(dir, info); err != nil {
		return nil, err
	}
	path := filepath.Join(dir, logName)
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o600)
	if err != nil {
		return nil, fmt.Errorf("cannot write the log file %s: %v; the state directory must be writable by this user",
			path, err)
	}
	if err := ownerOnlyFile(f); err != nil {
		f.Close()
		return nil, err
	}
	return f, nil
}
