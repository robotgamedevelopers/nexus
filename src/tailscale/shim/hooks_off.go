//go:build !nts_testhooks

package main

// testPanic is a test hook (hooks_on.go, built only with the nts_testhooks tag, which only the test program's build
// of the shim sets); in nexus's build it does nothing.
func testPanic(string) {}
