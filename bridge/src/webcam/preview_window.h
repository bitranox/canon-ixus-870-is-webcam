#pragma once

#include <cstdint>
#include <string>
#include <memory>

namespace webcam {

struct PreviewConfig {
    int width = 640;            // Initial window client width
    int height = 480;           // Initial window client height
    std::string title = "CHDK Webcam Preview";
};

class PreviewWindow {
public:
    PreviewWindow();
    ~PreviewWindow();

    // Create and show the window. Returns true on success.
    bool init(const PreviewConfig& config);

    // Display an RGB24 frame. Stretches to fit current window size.
    void show_frame(const uint8_t* rgb_data, int width, int height, int stride);

    // Pump window messages. Call once per frame from main loop.
    // Returns false if the window was closed.
    bool pump_messages();

    // Get pending zoom delta from keyboard/mouse wheel input.
    // Returns +1 for zoom in, -1 for zoom out, 0 for no change.
    // Resets after reading.
    int get_zoom_delta();

    // Close the window.
    void shutdown();

    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webcam
