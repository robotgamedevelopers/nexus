package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// logName is the file under the state directory that the node's user log goes to (nts.h).
const logName = "nexus-tailscale.log"

// lockName is the file under the state directory that a running node holds an OS lock on (lockDir).
const lockName = "nexus-tailscale.lock"

// cleanDir is an absolute dir without its redundant parts ("..", ".", a trailing separator), so that one directory
// has one spelling from here on; anything else is left as given, for ensureDir to refuse.
func cleanDir(dir string) string {
	if dir == "" || !filepath.IsAbs(dir) {
		return dir
	}
	return filepath.Clean(dir)
}

// ensureDir makes dir a node's state directory: created with any missing parents, every directory it creates and dir
// itself owner-only (ownerOnly, per OS). It touches no network and opens nothing in dir.
func ensureDir(dir string) error {
	if dir == "" {
		return fmt.Errorf("no state directory given (an empty path); pass an absolute directory")
	}
	if !filepath.IsAbs(dir) {
		return fmt.Errorf("the state directory %q is not an absolute path; pass an absolute directory", dir)
	}
	if info, err := os.Stat(dir); err == nil && !info.IsDir() {
		return fmt.Errorf("the state directory %s is not a directory; remove what is there or give another path", dir)
	}
	// The directories MkdirAll will create, outermost first, so each is made owner-only once it exists.
	var created []string
	for d := filepath.Clean(dir); ; {
		if _, err := os.Lstat(d); err == nil {
			break
		}
		created = append([]string{d}, created...)
		parent := filepath.Dir(d)
		if parent == d {
			break
		}
		d = parent
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return fmt.Errorf("cannot create the state directory %s: %v; give a directory this user may create, "+
			"or fix the permissions of its parent", dir, err)
	}
	for _, d := range created[:max(len(created)-1, 0)] {
		if err := ownerOnlyCreated(d); err != nil {
			return err
		}
	}
	info, err := os.Stat(dir)
	if err != nil {
		return fmt.Errorf("cannot read the state directory %s: %v", dir, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("the state directory %s is not a directory; remove what is there or give another path", dir)
	}
	return ownerOnly(dir, info)
}

// prepare holds dir (hold: refused while a node uses it), then opens its log file for appending, owner-only. Both are
// the caller's to release and close.
func prepare(dir string) (*dirLock, *cappedLog, error) {
	held, err := hold(dir)
	if err != nil {
		return nil, nil, err
	}
	logFile, err := openLog(filepath.Join(dir, logName))
	if err != nil {
		held.release()
		return nil, nil, err
	}
	return held, logFile, nil
}
