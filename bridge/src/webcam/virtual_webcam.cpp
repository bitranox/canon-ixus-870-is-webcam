#include "virtual_webcam.h"
#include <cstring>
#include <vector>

#ifdef _WIN32

// Windows implementation using softcam DirectShow filter.
//
// Softcam is a virtual webcam library for Windows that creates a DirectShow
// source filter visible to all video capture applications. We use its
// shared-memory API: the library creates a named shared memory region
// that the softcam DLL (registered via regsvr32) reads from.
//
// If softcam is not available, we fall back to a shared memory + named pipe
// implementation that can be consumed by OBS or a custom capture tool.

// Try to include softcam header; if not found, use fallback
#if __has_include(<softcam/softcam.h>)
#include <softcam/softcam.h>
#define HAS_SOFTCAM 1
#else
#define HAS_SOFTCAM 0
#endif

#include <windows.h>

namespace webcam {

struct VirtualWebcam::Impl {
    VirtualWebcamConfig config;
    std::string last_error;
    bool active = false;

#if HAS_SOFTCAM
    scCamera camera = nullptr;
#else
    // Fallback: shared memory for frame data
    HANDLE hMapFile = nullptr;
    uint8_t* shared_buf = nullptr;
    HANDLE hMutex = nullptr;
    static constexpr const char* SHMEM_NAME = "CHDKWebcamFrame";
    static constexpr const char* MUTEX_NAME = "CHDKWebcamMutex";
    int frame_data_size = 0;

    // Shared memory layout:
    // [0..3]   uint32_t width
    // [4..7]   uint32_t height
    // [8..11]  uint32_t stride
    // [12..15] uint32_t frame_counter
    // [16..]   RGB24 pixel data
    static constexpr int HEADER_SIZE = 16;
#endif
};

VirtualWebcam::VirtualWebcam() : impl_(std::make_unique<Impl>()) {}
VirtualWebcam::~VirtualWebcam() { shutdown(); }

bool VirtualWebcam::init(const VirtualWebcamConfig& config) {
    impl_->config = config;

#if HAS_SOFTCAM
    // Use framerate=0 for immediate delivery — camera is our real-time source,
    // so scSendFrame should not sleep to pace output.
    impl_->camera = scCreateCamera(config.width, config.height, 0.0f);
    if (!impl_->camera) {
        impl_->last_error = "Failed to create softcam camera";
        return false;
    }
    impl_->active = true;
    return true;
#else
    // Fallback: create shared memory
    int stride = config.width * 3;
    // Pad stride to 4-byte boundary for DirectShow compatibility
    stride = (stride + 3) & ~3;
    impl_->frame_data_size = Impl::HEADER_SIZE + stride * config.height;

    impl_->hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, impl_->frame_data_size, Impl::SHMEM_NAME);

    if (!impl_->hMapFile) {
        impl_->last_error = "Failed to create shared memory: " + std::to_string(GetLastError());
        return false;
    }

    impl_->shared_buf = static_cast<uint8_t*>(
        MapViewOfFile(impl_->hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, impl_->frame_data_size));

    if (!impl_->shared_buf) {
        impl_->last_error = "Failed to map shared memory";
        CloseHandle(impl_->hMapFile);
        impl_->hMapFile = nullptr;
        return false;
    }

    impl_->hMutex = CreateMutexA(nullptr, FALSE, Impl::MUTEX_NAME);
    if (!impl_->hMutex) {
        impl_->last_error = "Failed to create mutex";
        UnmapViewOfFile(impl_->shared_buf);
        CloseHandle(impl_->hMapFile);
        impl_->shared_buf = nullptr;
        impl_->hMapFile = nullptr;
        return false;
    }

    // Initialize header
    memset(impl_->shared_buf, 0, impl_->frame_data_size);

    impl_->active = true;
    return true;
#endif
}

bool VirtualWebcam::send_frame(const uint8_t* rgb_data, int width, int height, int stride) {
    if (!impl_->active) {
        impl_->last_error = "Virtual webcam not initialized";
        return false;
    }

#if HAS_SOFTCAM
    // softcam expects bottom-up BGR (DirectShow convention)
    int dst_stride = width * 3;
    std::vector<uint8_t> bgr_buf(dst_stride * height);

    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = rgb_data + y * stride;
        // Flip vertically: DirectShow expects bottom-up
        uint8_t* dst_row = bgr_buf.data() + (height - 1 - y) * dst_stride;
        for (int x = 0; x < width; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 2]; // B
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; // G
            dst_row[x * 3 + 2] = src_row[x * 3 + 0]; // R
        }
    }

    scSendFrame(impl_->camera, bgr_buf.data());
    return true;
#else
    // Shared memory fallback
    if (WaitForSingleObject(impl_->hMutex, 100) != WAIT_OBJECT_0) {
        return false;
    }

    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    int dst_stride = (width * 3 + 3) & ~3;
    uint32_t s = static_cast<uint32_t>(dst_stride);

    // Update frame counter
    uint32_t counter;
    memcpy(&counter, impl_->shared_buf + 12, 4);
    counter++;

    memcpy(impl_->shared_buf + 0, &w, 4);
    memcpy(impl_->shared_buf + 4, &h, 4);
    memcpy(impl_->shared_buf + 8, &s, 4);
    memcpy(impl_->shared_buf + 12, &counter, 4);

    // Copy pixel data (handle stride difference)
    uint8_t* dst = impl_->shared_buf + Impl::HEADER_SIZE;
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = rgb_data + y * stride;
        memcpy(dst + y * dst_stride, src_row, width * 3);
    }

    ReleaseMutex(impl_->hMutex);
    return true;
#endif
}

void VirtualWebcam::shutdown() {
    if (!impl_->active) return;

#if HAS_SOFTCAM
    if (impl_->camera) {
        scDeleteCamera(impl_->camera);
        impl_->camera = nullptr;
    }
#else
    if (impl_->shared_buf) {
        UnmapViewOfFile(impl_->shared_buf);
        impl_->shared_buf = nullptr;
    }
    if (impl_->hMapFile) {
        CloseHandle(impl_->hMapFile);
        impl_->hMapFile = nullptr;
    }
    if (impl_->hMutex) {
        CloseHandle(impl_->hMutex);
        impl_->hMutex = nullptr;
    }
#endif

    impl_->active = false;
}

bool VirtualWebcam::is_active() const {
    return impl_->active;
}

std::string VirtualWebcam::get_last_error() const {
    return impl_->last_error;
}

} // namespace webcam

#else // !_WIN32

// Stub for non-Windows platforms
namespace webcam {

struct VirtualWebcam::Impl {
    std::string last_error = "Virtual webcam only supported on Windows";
    bool active = false;
};

VirtualWebcam::VirtualWebcam() : impl_(std::make_unique<Impl>()) {}
VirtualWebcam::~VirtualWebcam() = default;

bool VirtualWebcam::init(const VirtualWebcamConfig&) {
    impl_->last_error = "Virtual webcam only supported on Windows";
    return false;
}

bool VirtualWebcam::send_frame(const uint8_t*, int, int, int) { return false; }
void VirtualWebcam::shutdown() {}
bool VirtualWebcam::is_active() const { return false; }
std::string VirtualWebcam::get_last_error() const { return impl_->last_error; }

} // namespace webcam

#endif
