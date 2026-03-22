#pragma once
// Virtual Microphone — feeds PCM audio to a shared memory region
// that a DirectShow audio source filter DLL reads from.
// The bridge writes here; conferencing apps capture from "CHDK Microphone".

#include <cstdint>

namespace webcam {

class VirtualMic {
public:
    VirtualMic();
    ~VirtualMic();

    // Initialize shared memory for audio
    bool init(uint32_t sample_rate = 44100, uint16_t channels = 1, uint16_t bits = 16);

    // Write PCM samples to shared memory (consumed by DirectShow filter)
    void write(const uint8_t* data, size_t size);

    // Shut down
    void shutdown();

    bool is_initialized() const { return initialized_; }

private:
    struct Impl;
    Impl* impl_;
    bool initialized_;
};

} // namespace webcam
