#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <functional>

// PTP/CHDK protocol constants
namespace ptp {

// Canon vendor ID
constexpr uint16_t CANON_VID = 0x04A9;

// PTP opcodes
constexpr uint16_t PTP_OC_CHDK = 0x9999;

// PTP response codes
constexpr uint16_t PTP_RC_OK = 0x2001;
constexpr uint16_t PTP_RC_GeneralError = 0x2002;

// CHDK sub-commands (param1 of opcode 0x9999)
enum ChdkCommand : uint32_t {
    CHDK_Version = 0,
    CHDK_GetMemory = 1,
    CHDK_SetMemory = 2,
    CHDK_CallFunction = 3,
    CHDK_TempData = 4,
    CHDK_UploadFile = 5,
    CHDK_DownloadFile = 6,
    CHDK_ExecuteScript = 7,
    CHDK_ScriptStatus = 8,
    CHDK_ScriptSupport = 9,
    CHDK_ReadScriptMsg = 10,
    CHDK_WriteScriptMsg = 11,
    CHDK_GetDisplayData = 12,
    CHDK_RemoteCaptureIsReady = 13,
    CHDK_RemoteCaptureGetData = 14,
    CHDK_GetMJPEGFrame = 15,
};

// Webcam streaming flags
constexpr uint32_t WEBCAM_START = 0x1;
constexpr uint32_t WEBCAM_STOP  = 0x2;

// PTP container (command/response)
struct PTPContainer {
    uint32_t length;
    uint16_t type;
    uint16_t code;
    uint32_t transaction_id;
    uint32_t params[5];
    int num_params;
};

// PTP container types
constexpr uint16_t PTP_TYPE_COMMAND  = 1;
constexpr uint16_t PTP_TYPE_DATA    = 2;
constexpr uint16_t PTP_TYPE_RESPONSE = 3;

// Frame format identifiers (matches WEBCAM_FMT_* in webcam.h)
constexpr uint32_t FRAME_FMT_JPEG = 0;
constexpr uint32_t FRAME_FMT_UYVY = 1;
constexpr uint32_t FRAME_FMT_H264 = 2;
constexpr uint32_t FRAME_FMT_DEBUG = 3;
constexpr uint32_t FRAME_FMT_H264_MULTI = 4;  // Multiple H.264 frames batched

// Frame received from camera (JPEG, raw UYVY, or H.264)
struct MJPEGFrame {
    std::vector<uint8_t> data;  // Frame data (JPEG or raw UYVY)
    uint32_t width;             // Frame width
    uint32_t height;            // Frame height
    uint32_t frame_num;         // Frame counter from camera
    uint32_t format;            // FRAME_FMT_JPEG or FRAME_FMT_UYVY
};

// Connection info
struct CameraInfo {
    uint16_t vendor_id;
    uint16_t product_id;
    std::string description;
    int chdk_major;
    int chdk_minor;
};

class PTPClient {
public:
    PTPClient();
    ~PTPClient();

    // Disable copy
    PTPClient(const PTPClient&) = delete;
    PTPClient& operator=(const PTPClient&) = delete;

    // Connect to the first Canon camera found via libusb
    // Returns true on success
    bool connect();

    // Disconnect from camera
    void disconnect();

    // Check if connected
    bool is_connected() const;

    // Get camera info (after connect)
    CameraInfo get_camera_info() const;

    // Get CHDK version
    bool get_chdk_version(int& major, int& minor);

    // Start webcam streaming on camera
    // quality: JPEG quality 1-100
    bool start_webcam(int quality);

    // Stop webcam streaming on camera
    bool stop_webcam();

    // Get a single MJPEG frame from camera
    // Returns true if frame received, false on error/no frame
    bool get_frame(MJPEGFrame& frame);

    // Execute a Lua script on camera
    bool execute_script(const std::string& script);

    // Upload a file to camera's SD card
    // remote_path: path on camera, e.g. "A/CHDK/MODULES/webcam.flt"
    // data: file contents
    bool upload_file(const std::string& remote_path, const std::vector<uint8_t>& data);

    // Read a script message from camera (after execute_script)
    // Returns the message string, or empty if no message available
    bool read_script_msg(std::string& msg);

    // Read camera memory via CHDK_GetMemory
    // address: camera RAM address to read from
    // size: number of bytes to read
    // data: output buffer
    bool read_memory(uint32_t address, uint32_t size, std::vector<uint8_t>& data);

    // Get last error message
    std::string get_last_error() const;

    // USB error statistics
    struct USBStats {
        int send_errors = 0;
        int recv_errors = 0;
        int timeout_errors = 0;
        int io_errors = 0;
    };
    USBStats get_usb_stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ptp
