#include "frame_trace.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace nexus {
void FrameTrace::record(double wall, double mono, const std::array<double, 10>& v) {
    if (!write) return;
    const auto finite = [](double x) { return std::isfinite(x) ? x : 0.0; };
    char row[768];
    const int n = std::snprintf(row, sizeof(row),
        "{\"seq\":%llu,\"unix_ms\":%.3f,\"mono_ms\":%.3f,\"total_ms\":%.3f,\"wait_ms\":%.3f,"
        "\"build_ms\":%.3f,\"scene_ms\":%.3f,\"play_ms\":%.3f,\"render_ms\":%.3f,\"gap_ms\":%.3f,"
        "\"input_wait_ms\":%.3f,\"width\":%.0f,\"height\":%.0f}\n",
        static_cast<unsigned long long>(++sequence_), finite(wall), finite(mono), finite(v[0]), finite(v[1]),
        finite(v[2]), finite(v[3]), finite(v[4]), finite(v[5]), finite(v[6]), finite(v[7]), finite(v[8]), finite(v[9]));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(row)) return;
    if (since_ < 0) since_ = mono;
    pending_.append(row, static_cast<size_t>(n));
    if (pending_.size() >= 64 * 1024 || mono - since_ >= 2000) flush();
}
void FrameTrace::flush() {
    if (write && !pending_.empty()) write(pending_);
    pending_.clear();
    since_ = -1;
}
TabOrderStore frameTraceStore(const std::string& path, uint64_t maxBytes) {
    TabOrderStore store;
    store.where = path;
    store.write = [path, maxBytes](const std::string& text, std::string* error) {
        namespace fs = std::filesystem;
        const fs::path file(std::u8string(path.begin(), path.end()));
        auto previous = file;
        previous += ".previous";
        std::error_code ec;
        const auto status = fs::symlink_status(file, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) { *error = ec.message(); return false; }
        if (fs::exists(status) && !fs::is_regular_file(status)) { *error = "performance trace must be a regular file"; return false; }
        ec.clear();
        const auto bytes = fs::exists(status) ? fs::file_size(file, ec) : 0;
        if (ec) { *error = ec.message(); return false; }
        if (text.size() > maxBytes) { *error = "performance batch exceeds the file limit"; return false; }
        if (bytes > maxBytes - text.size()) {
            fs::remove(previous, ec);
            if (ec) { *error = ec.message(); return false; }
            fs::rename(file, previous, ec);
            if (ec) { *error = ec.message(); return false; }
        }
        std::ofstream out(file, std::ios::binary | std::ios::app);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) { *error = "performance trace could not be appended"; return false; }
        return true;
    };
    return store;
}
}
