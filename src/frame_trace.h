// Numeric-only frame telemetry. Frame callbacks buffer bounded batches; the existing store writer performs I/O.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include "tab_order.h"

namespace nexus {
class FrameTrace {
public:
    std::function<void(const std::string&)> write;
    // total, swap wait, build, scene, play, render, gap, input wait, width, height. Times are milliseconds.
    void record(double wallMs, double monoMs, const std::array<double, 10>& values);
    void flush();
private:
    std::string pending_;
    uint64_t sequence_ = 0;
    double since_ = -1;
};
// Two bounded files, current and previous. This store is used only behind StoreWriter::wrap: coalesced batches
// under extreme disk pressure leave explicit sequence gaps instead of blocking the UI or growing memory unbounded.
TabOrderStore frameTraceStore(const std::string& path, uint64_t maxBytes = 8 * 1024 * 1024);
}
