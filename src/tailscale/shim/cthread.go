package main

// The C helpers the shim calls: the calling thread's id, and the host's state callback. They live in this file, which
// has no //export, because cgo copies the preamble of a file with exports into a second C file, and a function
// defined there twice would not link.

/*
#include <stdint.h>
#define NTS_TYPES_ONLY
#include "../nts.h"

#ifdef _WIN32
#include <windows.h>
static unsigned long long nts_thread_id(void) { return (unsigned long long)GetCurrentThreadId(); }
#else
#include <pthread.h>
static unsigned long long nts_thread_id(void) { return (unsigned long long)(uintptr_t)pthread_self(); }
#endif

static void nts_call_watch(nts_watch_fn fn, void* user, int node, unsigned long long generation) {
    fn(user, node, generation);
}
*/
import "C"

import "unsafe"

// threadID is the calling OS thread's id, as C sees it.
func threadID() uint64 {
	return uint64(C.nts_thread_id())
}

// callWatch calls the host's state callback.
func callWatch(fn C.nts_watch_fn, user unsafe.Pointer, node int, generation uint64) {
	C.nts_call_watch(fn, user, C.int(node), C.ulonglong(generation))
}
