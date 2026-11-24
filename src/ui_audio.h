#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "pr/audio.h"

namespace nexus {
struct Chirp;

// A single voice for short UI tones. Rapid navigation coalesces to the newest pending cue.
class UiAudio {
public:
    ~UiAudio();
    void play(const Chirp& cue);
    void setQuiet(bool quiet);
private:
    static void render(void* context, float* samples, uint32_t frames);
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool quitting_ = false, quiet_ = false;
    uint64_t requested_ = 0;
    std::atomic<bool> idle_{true};
    std::atomic<uint64_t> pending_{0};
    uint32_t rate_ = 48000, channels_ = 2, position_ = 0, length_ = 0;
    bool synth_ = false;
    double start_ = 0, delta_ = 0, gain_ = 0, phase_ = 0;
};
}
