#pragma once
// AV Recorder — saves H.264 video + PCM audio to MKV file using FFmpeg libavformat

#include <cstdint>
#include <string>

namespace webcam {

class AVRecorder {
public:
    AVRecorder();
    ~AVRecorder();

    // Open output file. Returns true on success.
    bool open(const std::string& filename,
              uint32_t width = 640, uint32_t height = 480, uint32_t fps = 30,
              uint32_t audio_rate = 44100, uint16_t audio_channels = 1);

    // Write one H.264 video frame (AVCC format — will be converted to Annex B)
    void write_video(const uint8_t* data, size_t size, bool is_idr);

    // Write PCM audio samples
    void write_audio(const uint8_t* data, size_t size);

    // Close and finalize the file
    void close();

    bool is_open() const { return opened_; }

private:
    struct Impl;
    Impl* impl_;
    bool opened_;
};

} // namespace webcam
