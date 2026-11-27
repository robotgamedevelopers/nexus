#include "frame_trace.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
using namespace nexus;
int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string path = argv[1];
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".previous");
        const auto store = frameTraceStore(path, 100);
        std::string why;
        if (!store.write(std::string(70, 'a'), &why) || !store.write(std::string(40, 'b'), &why)) return 1;
        if (std::filesystem::file_size(path) != 40 || std::filesystem::file_size(path + ".previous") != 70) return 2;
        if (store.write(std::string(101, 'c'), &why) || why.empty()) return 3;
        if (std::filesystem::file_size(path) != 40) return 4;
        std::filesystem::remove(path); std::filesystem::remove(path + ".previous");
        return 0;
    }
    FrameTrace recorder;
    std::vector<std::string> batches;
    recorder.write = [&](const std::string& text) { batches.push_back(text); };
    std::array<double, 10> sample{9, 1, 2, 1, 1, 4, 0, 3, 1000, 780};
    recorder.record(1000000, 0, sample);
    sample[5] = 250;
    recorder.record(1000500, 500, sample);
    if (!batches.empty()) return 5;
    recorder.record(1002000, 2000, sample);
    if (batches.size() != 1 || batches[0].find("\"render_ms\":250.000") == std::string::npos ||
        batches[0].find("\"seq\":1,") == std::string::npos || batches[0].find("\"seq\":3,") == std::string::npos) return 6;
    sample[0] = std::numeric_limits<double>::quiet_NaN();
    recorder.record(1002001, 2001, sample);
    recorder.flush();
    if (batches.size() != 2 || batches[1].find("nan") != std::string::npos) return 7;
    for (int i = 0; i < 1000; ++i) recorder.record(1002002, 2002, sample);
    recorder.flush();
    for (const auto& batch : batches) if (batch.size() > 64 * 1024 + 768) return 8;
    std::puts("frame history retains preceding samples, flushes stalls and bounds batches");
}
