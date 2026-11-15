//go:build windows

package main

import (
	"errors"
	"fmt"
	"os"

	"golang.org/x/sys/windows"
)

// lockFile takes an exclusive lock on the first byte of the open lock file, failing at once if any other handle (in
// this process or another) holds it; Windows drops the lock when the handle is closed or the process ends.
func lockFile(f *os.File) error {
	var overlapped windows.Overlapped
	err := windows.LockFileEx(windows.Handle(f.Fd()), windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY,
		0, 1, 0, &overlapped)
	if errors.Is(err, windows.ERROR_LOCK_VIOLATION) || errors.Is(err, windows.ERROR_IO_PENDING) {
		return errInUse
	}
	if err != nil {
		return fmt.Errorf("cannot lock %s: %v", f.Name(), err)
	}
	return nil
}
