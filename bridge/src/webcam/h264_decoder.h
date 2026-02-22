#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace webcam {

struct RGBFrame;  // forward decl from frame_processor.h

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    // Initialize decoder. Call once before decode().
    // output_width/height: desired RGB output size (0 = use decoded size)
    bool init(int output_width = 0, int output_height = 0);

    // Decode an H.264 frame (AVCC or Annex B format).
    // Converts AVCC length prefixes to Annex B start codes internally.
    // Returns true if a decoded frame is available in rgb_out.
    // May return false if the decoder needs more data (e.g., waiting for IDR).
    bool decode(const uint8_t* data, size_t size, RGBFrame& rgb_out);

    // Flush decoder (get any remaining buffered frames)
    bool flush(RGBFrame& rgb_out);

    // Reset decoder and re-feed stored IDR to recover from lost P-frames.
    // Returns true if IDR was successfully re-decoded.
    bool reinject_idr(RGBFrame& rgb_out);

    void shutdown();

    std::string get_last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webcam
