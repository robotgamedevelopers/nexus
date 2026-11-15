//go:build !windows

package main

import (
	"errors"
	"fmt"
	"os"
	"syscall"
)

// lockFile takes an exclusive, non-blocking flock on the open lock file. flock locks belong to the open file, so a
// second open of the same file, in this process or another, is refused while the first is held; the OS drops the lock
// when the file is closed or the process ends.
func lockFile(f *os.File) error {
	err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
	if errors.Is(err, syscall.EWOULDBLOCK) {
		return errInUse
	}
	if err != nil {
		return fmt.Errorf("cannot lock %s: %v", f.Name(), err)
	}
	return nil
}
