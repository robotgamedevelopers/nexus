// Force-included into Prax's lib/process/backends/posix/src/process_posix.cpp, and nothing else, on Linux only
// (CMakeLists.txt, "Prax's lib/process on Linux"). It goes away when Prax fixes its file; until then CMake refuses to
// configure once the line below is gone from it, so this cannot outlive the bug.
//
// The bug, at Prax 87bcd16a6657ab8d93573cdf1291ba14c169f0dc, process_posix.cpp lines 165-173: inside
// `namespace pr_impl { namespace { ... } }`, environment() declares `extern char** environ;` at block scope. A
// block-scope extern declaration names a member of the innermost enclosing namespace, so it names
// pr_impl::(anonymous)::environ, which nothing defines: clang emits the undefined reference
// _ZN7pr_impl12_GLOBAL__N_17environE, gcc _ZN7pr_impl12_GLOBAL__N_1L7environE, and the link fails. (On macOS the
// file reads _NSGetEnviron() instead, so the bug is Linux's alone.)
//
// The fix here, plain C++ for both compilers and any linker: a pointer to the C library's own `environ`, defined in
// that same unnamed namespace (a translation unit has one unnamed namespace per enclosing namespace), and a macro that
// turns Prax's two lines into `extern char** *nexus_prax_environ; return *nexus_prax_environ;`. The block-scope extern
// then redeclares this pointer, and environment() reads the live `environ` through it, after any setenv as before.
// <unistd.h> comes first so its own declaration of `environ` is read before the macro exists.

#pragma once

#include <unistd.h>

extern "C" char** environ;

namespace pr_impl {
namespace {
char*** nexus_prax_environ = &environ;
} // namespace
} // namespace pr_impl

#define environ *nexus_prax_environ
