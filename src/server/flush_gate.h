// A test seam on the store's filesystem, for tests/link/link_e2e.cpp's flush run, which checks on the real server that
// nothing of a batch leaves it before the batch's flush has returned, and that a batch lost before its flush is
// neither acknowledged nor told to a driver.
//
// With NEXUS_TEST_FLUSH_GATE=<path> in nexus-server's environment, each flush of a file the store keeps open (the
// journal's log: one write of the batch, then this flush) first looks at <path>:
//   absent    the flush goes on;
//   "hold"    the server writes <path>.reached and waits there, the batch written and not flushed, looking at <path>
//             again every 10 ms: removed, the flush goes on; "lose", as below;
//   "lose"    a crash between the write and the flush: the file is cut back to what its last flush made durable (what
//             a power cut takes) and the server ends by SIGKILL.
// Without the variable (always, outside that test) the store's filesystem is the plain one.

#pragma once

#include <memory>
#include <string>

#include "store/fs.h"

namespace nexus::server {

constexpr const char* kFlushGateVariable = "NEXUS_TEST_FLUSH_GATE";

// `fs` with the gate at `gate` in front of every kept-open file's flush.
std::unique_ptr<store::Fs> withFlushGate(std::unique_ptr<store::Fs> fs, const std::string& gate);

} // namespace nexus::server
