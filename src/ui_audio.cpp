#include "ui_audio.h"
#include "window_app.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace nexus {
static_assert(std::atomic<uint64_t>::is_always_lock_free);

UiAudio::~UiAudio() {
    {
        std::lock_guard lock(mutex_);
        quitting_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void UiAudio::setQuiet(bool quiet) {
    std::lock_guard lock(mutex_);
    quiet_ = quiet;
    if (quiet) requested_ = 0;
    wake_.notify_one();
}

void UiAudio::play(const Chirp& cue) {
    if (cue.gain <= 0) return;
    std::lock_guard lock(mutex_);
    if (quiet_) return;
    auto field = [](float value) { return uint64_t(std::lround(value)); };
    requested_ = field(cue.startHz) | (field(cue.endHz) << 16) |
        (field(cue.durationMs) << 32) | (uint64_t(cue.synth) << 47) | (field(cue.gain * 65535) << 48);
    if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    wake_.notify_one();
}

void UiAudio::run() {
    using namespace std::chrono_literals;
    std::unique_lock lock(mutex_);
    while (!quitting_) {
        wake_.wait(lock, [&] { return quitting_ || requested_; });
        if (quitting_) break;
        lock.unlock();
        auto* output = prAudioOutputCreate(PrAudioOutputBackend_Default, {48000, 2});
        bool started = false;
        if (output) {
            const auto format = prAudioOutputFormat(output);
            rate_ = format.sampleRate;
            channels_ = format.channels;
            position_ = length_ = 0;
            started = prAudioOutputStart(output, render, this);
        }
        lock.lock();
        while (started && !quitting_ && !quiet_) {
            if (requested_) {
                pending_.store(requested_);
                requested_ = 0;
            }
            // Release the device after the last tone has drained. No callbacks or timer run between bursts.
            if (!wake_.wait_for(lock, 150ms, [&] { return quitting_ || quiet_ || requested_; }) &&
                    idle_.load() && !pending_.load()) break;
        }
        if (!started) requested_ = 0;
        lock.unlock();
        prAudioOutputDestroy(output);
        pending_.store(0);
        lock.lock();
    }
}

void UiAudio::render(void* context, float* samples, uint32_t frames) {
    auto& voice = *static_cast<UiAudio*>(context);
    for (uint32_t i = 0; i < frames; ++i) {
        if (voice.position_ == voice.length_) {
            const uint64_t next = voice.pending_.exchange(0);
            if (!next) {
                std::fill_n(samples, (frames - i) * voice.channels_, 0.f);
                voice.idle_.store(true);
                return;
            }
            voice.idle_.store(false);
            voice.start_ = static_cast<double>(next & 65535);
            voice.delta_ = double((next >> 16) & 65535) - voice.start_;
            voice.synth_ = ((next >> 47) & 1) != 0;
            voice.length_ = std::max(1u, uint32_t((next >> 32) & 32767) * voice.rate_ / 1000);
            voice.gain_ = double(next >> 48) / 65535;
            voice.position_ = 0;
            voice.phase_ = 0;
        }
        const double t = double(voice.position_++) / voice.length_;
        const double envelope = std::min(1.0, t * 10) * (1 - t) * (1 - t);
        double wave = std::sin(voice.phase_);
        double amplitude = envelope;
        if (voice.synth_) {
            const auto phase = voice.phase_;
            wave = 0.55 * wave + 0.25 * std::sin(phase * 1.501) + 0.14 * std::sin(phase * 2.008)
                + 0.06 * std::sin(phase * 3);
            const double shape = std::sin(std::numbers::pi * t);
            amplitude = shape * shape;
        }
        const float value = float(wave * amplitude * voice.gain_);
        voice.phase_ += 2 * std::numbers::pi * (voice.start_ + t * voice.delta_) / voice.rate_;
        std::fill_n(samples, voice.channels_, value);
        samples += voice.channels_;
    }
}
}
