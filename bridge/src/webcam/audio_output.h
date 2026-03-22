#pragma once
// WASAPI audio output — plays PCM audio through default speakers
// Usage: init(sample_rate, channels, bits), write(data, size), shutdown()

#include <cstdint>
#include <string>

namespace webcam {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // Initialize WASAPI output device
    // device_name: substring match for output device (empty = default)
    // Returns true on success
    bool init(uint32_t sample_rate = 44100, uint16_t channels = 1, uint16_t bits = 16,
              const std::string& device_name = "");

    // Write PCM samples to output buffer
    void write(const uint8_t* data, size_t size);

    // Clean up
    void shutdown();

    bool is_initialized() const { return initialized_; }

private:
    struct Impl;
    Impl* impl_;
    bool initialized_;
};

} // namespace webcam
