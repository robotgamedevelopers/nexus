//go:build !windows

package main

import (
	"fmt"
	"os"
	"syscall"
)

// ownerOnly refuses a state directory another user owns, and makes one this user owns exactly 0700.
func ownerOnly(dir string, info os.FileInfo) error {
	if stat, ok := info.Sys().(*syscall.Stat_t); ok && int(stat.Uid) != os.Geteuid() {
		return fmt.Errorf("the state directory %s belongs to user id %d, not this user (%d); give a directory of "+
			"this user's own", dir, stat.Uid, os.Geteuid())
	}
	if info.Mode().Perm() != 0o700 {
		if err := os.Chmod(dir, 0o700); err != nil {
			return fmt.Errorf("cannot make the state directory %s owner-only (0700): %v", dir, err)
		}
	}
	return nil
}

// ownerOnlyFile makes the log file exactly 0600, whatever mode an older file had.
func ownerOnlyFile(f *os.File) error {
	info, err := f.Stat()
	if err != nil {
		return fmt.Errorf("cannot read the log file %s: %v", f.Name(), err)
	}
	if info.Mode().Perm() != 0o600 {
		if err := f.Chmod(0o600); err != nil {
			return fmt.Errorf("cannot make the log file %s owner-only (0600): %v", f.Name(), err)
		}
	}
	return nil
}
