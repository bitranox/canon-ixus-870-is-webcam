// Virtual Microphone — shared memory audio producer
// The bridge writes PCM audio here. A companion DirectShow DLL
// (chdk_mic.dll) reads from the same shared memory and presents
// it as a "CHDK Microphone" audio capture device.

#include "virtual_mic.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace webcam {

// Shared memory layout:
// [0..3]    uint32_t magic (0x4D494348 = "MICH")
// [4..7]    uint32_t sample_rate
// [8..9]    uint16_t channels
// [10..11]  uint16_t bits_per_sample
// [12..15]  uint32_t write_pos (byte offset into ring buffer)
// [16..19]  uint32_t read_pos (byte offset, updated by consumer)
// [20..23]  uint32_t ring_size (bytes)
// [24..27]  uint32_t frame_counter (incremented each write)
// [28..31]  reserved
// [32..]    ring buffer data

static constexpr uint32_t MAGIC = 0x4D494348; // "MICH"
static constexpr int HEADER_SIZE = 32;
static constexpr int RING_SIZE = 88200 * 4; // ~4 seconds at 44100Hz mono 16-bit
static constexpr int SHMEM_SIZE = HEADER_SIZE + RING_SIZE;
static constexpr const char* SHMEM_NAME = "CHDKMicrophoneAudio";

struct VirtualMic::Impl {
    HANDLE hMapFile = nullptr;
    uint8_t* buf = nullptr;
};

VirtualMic::VirtualMic() : impl_(new Impl), initialized_(false) {}
VirtualMic::~VirtualMic() { shutdown(); delete impl_; }

bool VirtualMic::init(uint32_t sample_rate, uint16_t channels, uint16_t bits) {
    if (initialized_) return true;

    impl_->hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, SHMEM_SIZE, SHMEM_NAME);
    if (!impl_->hMapFile) {
        fprintf(stderr, "VirtualMic: Failed to create shared memory\n");
        return false;
    }

    impl_->buf = (uint8_t*)MapViewOfFile(
        impl_->hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, SHMEM_SIZE);
    if (!impl_->buf) {
        CloseHandle(impl_->hMapFile);
        impl_->hMapFile = nullptr;
        fprintf(stderr, "VirtualMic: Failed to map shared memory\n");
        return false;
    }

    // Initialize header
    memset(impl_->buf, 0, SHMEM_SIZE);
    *(uint32_t*)(impl_->buf + 0) = MAGIC;
    *(uint32_t*)(impl_->buf + 4) = sample_rate;
    *(uint16_t*)(impl_->buf + 8) = channels;
    *(uint16_t*)(impl_->buf + 10) = bits;
    *(uint32_t*)(impl_->buf + 12) = 0; // write_pos
    *(uint32_t*)(impl_->buf + 16) = 0; // read_pos
    *(uint32_t*)(impl_->buf + 20) = RING_SIZE;
    *(uint32_t*)(impl_->buf + 24) = 0; // frame_counter

    initialized_ = true;
    fprintf(stderr, "VirtualMic: shared memory ready (%s, %u Hz, %u ch)\n",
            SHMEM_NAME, sample_rate, channels);
    return true;
}

void VirtualMic::write(const uint8_t* data, size_t size) {
    if (!initialized_ || !data || size == 0) return;

    uint32_t write_pos = *(volatile uint32_t*)(impl_->buf + 12);
    uint8_t* ring = impl_->buf + HEADER_SIZE;

    // Write to ring buffer with wrap-around
    size_t remaining = size;
    const uint8_t* src = data;
    while (remaining > 0) {
        size_t space = RING_SIZE - write_pos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(ring + write_pos, src, chunk);
        write_pos = (write_pos + (uint32_t)chunk) % RING_SIZE;
        src += chunk;
        remaining -= chunk;
    }

    // Update write position and frame counter (atomic-ish)
    *(volatile uint32_t*)(impl_->buf + 12) = write_pos;
    (*(volatile uint32_t*)(impl_->buf + 24))++;
}

void VirtualMic::shutdown() {
    if (!initialized_) return;
    if (impl_->buf) {
        *(uint32_t*)(impl_->buf) = 0; // clear magic
        UnmapViewOfFile(impl_->buf);
        impl_->buf = nullptr;
    }
    if (impl_->hMapFile) {
        CloseHandle(impl_->hMapFile);
        impl_->hMapFile = nullptr;
    }
    initialized_ = false;
}

} // namespace webcam

#else
namespace webcam {
VirtualMic::VirtualMic() : impl_(nullptr), initialized_(false) {}
VirtualMic::~VirtualMic() {}
bool VirtualMic::init(uint32_t, uint16_t, uint16_t) { return false; }
void VirtualMic::write(const uint8_t*, size_t) {}
void VirtualMic::shutdown() {}
}
#endif
