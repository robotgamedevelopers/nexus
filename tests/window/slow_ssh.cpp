// A stand-in for ssh in the frame-budget test (test_frames.cpp): a host that never answers. It hangs for three seconds,
// as ssh does before a connection times out, then fails as ssh does. It connects nowhere.

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::fprintf(stderr, "ssh: connect to host stalled.test port 22: Operation timed out\n");
    return 255;
}
