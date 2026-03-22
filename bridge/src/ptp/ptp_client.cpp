#include "ptp_client.h"
#include <libusb.h>
#include <cstring>
#include <chrono>
#include <thread>

namespace ptp {

// USB transfer timeout in milliseconds
constexpr int USB_TIMEOUT = 2000;

// PTP packet header sizes
constexpr int PTP_HEADER_SIZE = 12;
constexpr int PTP_MAX_PARAMS = 5;

struct PTPClient::Impl {
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint8_t ep_int = 0;
    uint32_t transaction_id = 0;
    bool connected = false;
    std::string last_error;
    CameraInfo camera_info{};

    // Pending zoom delta (piggybacked on next get_frame)
    int pending_zoom = 0;

    // USB error counters
    int usb_send_errors = 0;
    int usb_recv_errors = 0;
    int usb_timeout_errors = 0;
    int usb_io_errors = 0;

    void classify_usb_error(int libusb_rc, bool is_send) {
        if (libusb_rc == LIBUSB_ERROR_TIMEOUT) {
            usb_timeout_errors++;
        } else if (libusb_rc == LIBUSB_ERROR_IO) {
            usb_io_errors++;
        } else if (is_send) {
            usb_send_errors++;
        } else {
            usb_recv_errors++;
        }
    }

    // PTP session
    uint32_t session_id = 1;

    bool init_libusb() {
        int r = libusb_init(&ctx);
        if (r < 0) {
            last_error = "Failed to initialize libusb: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
            return false;
        }
        return true;
    }

    void cleanup_libusb() {
        if (handle) {
            libusb_release_interface(handle, 0);
            libusb_close(handle);
            handle = nullptr;
        }
        if (ctx) {
            libusb_exit(ctx);
            ctx = nullptr;
        }
        connected = false;
    }

    bool find_canon_camera() {
        libusb_device** devs;
        ssize_t cnt = libusb_get_device_list(ctx, &devs);
        if (cnt < 0) {
            last_error = "Failed to get USB device list";
            return false;
        }

        bool found = false;
        for (ssize_t i = 0; i < cnt; i++) {
            libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(devs[i], &desc) < 0) continue;

            if (desc.idVendor == CANON_VID) {
                fprintf(stderr, "  Found Canon device PID=0x%04X, opening...\n", desc.idProduct);
                int r = libusb_open(devs[i], &handle);
                if (r < 0) {
                    fprintf(stderr, "  libusb_open failed: %s\n", libusb_strerror(static_cast<libusb_error>(r)));
                    continue;
                }
                fprintf(stderr, "  libusb_open OK\n");

                camera_info.vendor_id = desc.idVendor;
                camera_info.product_id = desc.idProduct;

                // Find PTP endpoints
                libusb_config_descriptor* config;
                if (libusb_get_active_config_descriptor(devs[i], &config) == 0) {
                    fprintf(stderr, "  Config: %d interfaces\n", config->bNumInterfaces);
                    for (int j = 0; j < config->bNumInterfaces; j++) {
                        const libusb_interface& iface = config->interface[j];
                        for (int k = 0; k < iface.num_altsetting; k++) {
                            const libusb_interface_descriptor& alt = iface.altsetting[k];
                            fprintf(stderr, "  Interface %d alt %d: class=%d subclass=%d protocol=%d endpoints=%d\n",
                                    j, k, alt.bInterfaceClass, alt.bInterfaceSubClass,
                                    alt.bInterfaceProtocol, alt.bNumEndpoints);
                            // PTP class: 6 (Image), subclass: 1, protocol: 1
                            if (alt.bInterfaceClass == 6 || alt.bInterfaceClass == 0xFF) {
                                for (int e = 0; e < alt.bNumEndpoints; e++) {
                                    const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                                    fprintf(stderr, "    EP 0x%02X: attr=0x%02X maxpkt=%d\n",
                                            ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize);
                                    if ((ep.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
                                        if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                                            ep_in = ep.bEndpointAddress;
                                        } else {
                                            ep_out = ep.bEndpointAddress;
                                        }
                                    } else if ((ep.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                                        ep_int = ep.bEndpointAddress;
                                    }
                                }
                            }
                        }
                    }
                    libusb_free_config_descriptor(config);
                }

                if (ep_in && ep_out) {
                    found = true;
                    break;
                }

                // Didn't find endpoints, close and try next device
                libusb_close(handle);
                handle = nullptr;
                ep_in = ep_out = ep_int = 0;
            }
        }

        libusb_free_device_list(devs, 1);

        if (!found) {
            last_error = "No Canon camera found. Ensure camera is connected and libusb-win32 driver is installed (use Zadig).";
        }
        return found;
    }

    bool claim_interface() {
        // Detach kernel driver if attached (Linux)
#ifdef LIBUSB_HAS_DETACH_KERNEL_DRIVER
        if (libusb_kernel_driver_active(handle, 0) == 1) {
            libusb_detach_kernel_driver(handle, 0);
        }
#endif
        // Set configuration (may be needed on Windows with WinUSB)
        int r = libusb_set_configuration(handle, 1);
        if (r < 0 && r != LIBUSB_ERROR_BUSY) {
            fprintf(stderr, "  set_configuration(1): %s\n", libusb_strerror(static_cast<libusb_error>(r)));
        } else {
            fprintf(stderr, "  set_configuration(1): OK (r=%d)\n", r);
        }

        r = libusb_claim_interface(handle, 0);
        if (r < 0) {
            last_error = "Failed to claim USB interface: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
            return false;
        }
        fprintf(stderr, "  claim_interface(0): OK\n");
        return true;
    }

    // Send a PTP command (no data phase)
    bool send_command(uint16_t opcode, const uint32_t* params, int num_params) {
        uint8_t buf[PTP_HEADER_SIZE + PTP_MAX_PARAMS * 4];
        int len = PTP_HEADER_SIZE + num_params * 4;

        transaction_id++;

        // Build PTP container
        memcpy(buf + 0, &len, 4);              // Length
        uint16_t type = PTP_TYPE_COMMAND;
        memcpy(buf + 4, &type, 2);             // Type = Command
        memcpy(buf + 6, &opcode, 2);           // Operation Code
        memcpy(buf + 8, &transaction_id, 4);   // Transaction ID

        for (int i = 0; i < num_params; i++) {
            memcpy(buf + PTP_HEADER_SIZE + i * 4, &params[i], 4);
        }

        int transferred = 0;
        int r = libusb_bulk_transfer(handle, ep_out, buf, len, &transferred, USB_TIMEOUT);
        if (r < 0) {
            classify_usb_error(r, true);
            last_error = "USB send error: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
            return false;
        }
        return true;
    }

    // Receive PTP response (command response + optional data)
    bool receive_response(PTPContainer& resp, std::vector<uint8_t>* data = nullptr, int timeout_ms = USB_TIMEOUT) {
        // Buffer for receiving - large enough for data + headers
        constexpr int MAX_BUF = 512 * 1024; // 512 KB max per transfer
        std::vector<uint8_t> buf(MAX_BUF);
        int transferred = 0;

        // First read: may contain data or response
        int r = libusb_bulk_transfer(handle, ep_in, buf.data(), MAX_BUF, &transferred, timeout_ms);
        if (r < 0) {
            classify_usb_error(r, false);
            last_error = "USB receive error: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
            return false;
        }

        if (transferred < PTP_HEADER_SIZE) {
            last_error = "Short PTP response";
            return false;
        }

        // Parse header
        uint32_t pkt_len;
        uint16_t pkt_type, pkt_code;
        uint32_t pkt_tid;
        memcpy(&pkt_len, buf.data() + 0, 4);
        memcpy(&pkt_type, buf.data() + 4, 2);
        memcpy(&pkt_code, buf.data() + 6, 2);
        memcpy(&pkt_tid, buf.data() + 8, 4);

        if (pkt_type == PTP_TYPE_DATA) {
            // This is a data phase - collect or drain all data
            uint32_t data_total = pkt_len - PTP_HEADER_SIZE;
            int data_in_first = transferred - PTP_HEADER_SIZE;
            if (data) {
                data->resize(data_total);
                if (data_in_first > 0 && data_in_first <= static_cast<int>(data_total)) {
                    memcpy(data->data(), buf.data() + PTP_HEADER_SIZE, data_in_first);
                }
            }

            // Read remaining data if needed (or drain it if data is NULL)
            uint32_t total_read = (data_in_first > 0) ? static_cast<uint32_t>(data_in_first) : 0;
            while (total_read < data_total) {
                r = libusb_bulk_transfer(handle, ep_in, buf.data(), MAX_BUF, &transferred, timeout_ms);
                if (r < 0) {
                    classify_usb_error(r, false);
                    last_error = "USB data receive error: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
                    return false;
                }
                uint32_t to_copy = std::min(static_cast<uint32_t>(transferred), data_total - total_read);
                if (data) {
                    memcpy(data->data() + total_read, buf.data(), to_copy);
                }
                total_read += to_copy;
            }

            // Now read the actual response
            r = libusb_bulk_transfer(handle, ep_in, buf.data(), MAX_BUF, &transferred, timeout_ms);
            if (r < 0) {
                classify_usb_error(r, false);
                last_error = "USB response receive error: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
                return false;
            }
            if (transferred < PTP_HEADER_SIZE) {
                last_error = "Short PTP response after data";
                return false;
            }
            memcpy(&pkt_len, buf.data() + 0, 4);
            memcpy(&pkt_type, buf.data() + 4, 2);
            memcpy(&pkt_code, buf.data() + 6, 2);
            memcpy(&pkt_tid, buf.data() + 8, 4);
        }

        if (pkt_type != PTP_TYPE_RESPONSE) {
            last_error = "Unexpected PTP packet type: " + std::to_string(pkt_type);
            return false;
        }

        resp.length = pkt_len;
        resp.type = pkt_type;
        resp.code = pkt_code;
        resp.transaction_id = pkt_tid;
        resp.num_params = (pkt_len - PTP_HEADER_SIZE) / 4;

        for (int i = 0; i < resp.num_params && i < PTP_MAX_PARAMS; i++) {
            memcpy(&resp.params[i], buf.data() + PTP_HEADER_SIZE + i * 4, 4);
        }

        return true;
    }

    // Reset USB device to clear any stale state
    void reset_device() {
        if (handle) {
            libusb_reset_device(handle);
        }
    }

    // Open PTP session
    bool open_session() {
        fprintf(stderr, "  PTP: endpoints in=0x%02X out=0x%02X int=0x%02X\n", ep_in, ep_out, ep_int);

        // Clear any stalled endpoints
        {
            int r;
            r = libusb_clear_halt(handle, ep_out);
            fprintf(stderr, "  PTP: clear_halt(OUT)=%d\n", r);
            r = libusb_clear_halt(handle, ep_in);
            fprintf(stderr, "  PTP: clear_halt(IN)=%d\n", r);
        }

        // Clear any stale data on the IN endpoint (non-blocking drain)
        {
            uint8_t drain[512];
            int transferred = 0;
            int r;
            do {
                r = libusb_bulk_transfer(handle, ep_in, drain, sizeof(drain), &transferred, 100);
                if (r == 0) {
                    fprintf(stderr, "  PTP: drained %d stale bytes from IN endpoint\n", transferred);
                }
            } while (r == 0 && transferred > 0);
        }

        uint32_t params[1] = { session_id };
        // OpenSession opcode = 0x1002
        uint16_t opcode = 0x1002;

        uint8_t buf[PTP_HEADER_SIZE + 4];
        int len = PTP_HEADER_SIZE + 4;
        transaction_id = 0; // Reset for new session

        transaction_id++;
        memcpy(buf + 0, &len, 4);
        uint16_t type = PTP_TYPE_COMMAND;
        memcpy(buf + 4, &type, 2);
        memcpy(buf + 6, &opcode, 2);
        memcpy(buf + 8, &transaction_id, 4);
        memcpy(buf + PTP_HEADER_SIZE, &params[0], 4);

        fprintf(stderr, "  PTP: sending OpenSession (%d bytes)...\n", len);
        int transferred = 0;
        int r = libusb_bulk_transfer(handle, ep_out, buf, len, &transferred, USB_TIMEOUT);
        if (r < 0) {
            last_error = "Failed to send OpenSession: " + std::string(libusb_strerror(static_cast<libusb_error>(r)));
            return false;
        }
        fprintf(stderr, "  PTP: sent %d bytes, waiting for response...\n", transferred);

        // Read response
        PTPContainer resp{};
        if (!receive_response(resp)) {
            last_error = "Failed to read OpenSession response: " + last_error;
            return false;
        }
        fprintf(stderr, "  PTP: OpenSession response code=0x%04X params=%d\n", resp.code, resp.num_params);
        if (resp.code != PTP_RC_OK) {
            // Session may already be open
            if (resp.code == 0x201E) { // SessionAlreadyOpen
                fprintf(stderr, "  PTP: session already open, reusing\n");
                return true;
            }
            char hex[16];
            snprintf(hex, sizeof(hex), "0x%04X", resp.code);
            last_error = "OpenSession failed with code " + std::string(hex);
            return false;
        }
        return true;
    }

    // Send CHDK command with params, receive response + optional data
    bool chdk_command(ChdkCommand cmd, uint32_t p2, uint32_t p3, uint32_t p4,
                      PTPContainer& resp, std::vector<uint8_t>* data = nullptr,
                      int timeout_ms = USB_TIMEOUT) {
        uint32_t params[5] = { static_cast<uint32_t>(cmd), p2, p3, p4, 0 };
        if (!send_command(PTP_OC_CHDK, params, 4)) return false;
        if (!receive_response(resp, data, timeout_ms)) return false;
        return true;
    }
};

PTPClient::PTPClient() : impl_(std::make_unique<Impl>()) {}

PTPClient::~PTPClient() {
    disconnect();
}

bool PTPClient::connect() {
    if (!impl_->init_libusb()) return false;
    if (!impl_->find_canon_camera()) {
        impl_->cleanup_libusb();
        return false;
    }
    if (!impl_->claim_interface()) {
        impl_->cleanup_libusb();
        return false;
    }
    if (!impl_->open_session()) {
        // Try USB reset and retry once
        fprintf(stderr, "Session open failed (%s), resetting USB device...\n", impl_->last_error.c_str());
        impl_->reset_device();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!impl_->claim_interface()) {
            impl_->cleanup_libusb();
            return false;
        }
        if (!impl_->open_session()) {
            impl_->cleanup_libusb();
            return false;
        }
    }

    impl_->connected = true;
    impl_->camera_info.description = "Canon Camera (PID: 0x" +
        ([](uint16_t v) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%04X", v);
            return std::string(buf);
        })(impl_->camera_info.product_id) + ")";

    // Get CHDK version
    get_chdk_version(impl_->camera_info.chdk_major, impl_->camera_info.chdk_minor);

    return true;
}

void PTPClient::disconnect() {
    if (impl_->connected) {
        // Try to close session gracefully
        uint16_t opcode = 0x1003; // CloseSession
        impl_->send_command(opcode, nullptr, 0);
        PTPContainer resp{};
        impl_->receive_response(resp);
    }
    impl_->cleanup_libusb();
}

bool PTPClient::is_connected() const {
    return impl_->connected;
}

CameraInfo PTPClient::get_camera_info() const {
    return impl_->camera_info;
}

bool PTPClient::get_chdk_version(int& major, int& minor) {
    PTPContainer resp{};
    if (!impl_->chdk_command(CHDK_Version, 0, 0, 0, resp)) {
        return false;
    }
    if (resp.code != PTP_RC_OK || resp.num_params < 2) {
        return false;
    }
    major = resp.params[0];
    minor = resp.params[1];
    return true;
}

bool PTPClient::start_webcam(int quality) {
    PTPContainer resp{};
    std::vector<uint8_t> dummy;

    fprintf(stderr, "  start_webcam: sending CHDK_GetMJPEGFrame quality=%d sub=WEBCAM_START...\n", quality);

    // Start command triggers module load + mode switch on camera, which can take ~5s
    if (!impl_->chdk_command(CHDK_GetMJPEGFrame, quality, WEBCAM_START, 0, resp, &dummy, 10000)) {
        fprintf(stderr, "  start_webcam: chdk_command failed: %s\n", impl_->last_error.c_str());
        return false;
    }

    fprintf(stderr, "  start_webcam: resp code=0x%04X params=%d data=%zu bytes\n",
            resp.code, resp.num_params, dummy.size());
    for (int i = 0; i < resp.num_params && i < 5; i++) {
        fprintf(stderr, "    param[%d] = 0x%08X (%u)\n", i, resp.params[i], resp.params[i]);
    }
    if (!dummy.empty()) {
        fprintf(stderr, "    data: ");
        for (size_t i = 0; i < dummy.size() && i < 64; i++) {
            fprintf(stderr, "%02X ", dummy[i]);
        }
        fprintf(stderr, "\n");
    }

    // If start failed (0xDEAD marker), report error string from camera
    if (resp.num_params >= 3 && resp.params[2] == 0xDEAD) {
        if (!dummy.empty() && dummy[0] != 0) {
            std::string err_msg(dummy.begin(), dummy.end());
            impl_->last_error = "Camera module error: " + err_msg;
            fprintf(stderr, "  start_webcam: ERROR: %s\n", err_msg.c_str());
        } else {
            impl_->last_error = "Camera start failed (rc=" + std::to_string(static_cast<int>(resp.params[1])) + ")";
            fprintf(stderr, "  start_webcam: FAILED rc=%d\n", static_cast<int>(resp.params[1]));
        }
    }

    // Parse startup diagnostics (0xBEEF marker = start success with DMA chain info)
    if (resp.num_params >= 4 && resp.params[3] == 0xBEEF && !dummy.empty() && dummy.size() >= 104) {
        auto u32 = [&](int off) -> uint32_t {
            uint32_t v; memcpy(&v, dummy.data() + off, 4); return v;
        };
        fprintf(stderr, "\n=== DMA CHAIN DIAGNOSTICS (after recording start) ===\n");
        fprintf(stderr, "  STATE STRUCT @ 0x70D8:\n");
        fprintf(stderr, "    +0x48  MJPEG active   = %u\n", u32(0));
        fprintf(stderr, "    +0x4C  (paired flag)  = %u\n", u32(4));
        fprintf(stderr, "    +0x54  DMA status     = %u\n", u32(8));
        fprintf(stderr, "    +0x58  DMA frame idx  = %u\n", u32(12));
        fprintf(stderr, "    +0x5C  DMA req state  = %u (3=req, 4=stop)\n", u32(16));
        fprintf(stderr, "    +0x64  VRAM buf addr  = 0x%08X\n", u32(20));
        fprintf(stderr, "    +0xA0  DMA callback   = 0x%08X\n", u32(24));
        fprintf(stderr, "    +0xEC  pipeline active= %u\n", u32(28));
        fprintf(stderr, "    +0xD4  video mode     = %u (2=VGA)\n", u32(32));
        fprintf(stderr, "    +0x114 rec callback 1 = 0x%08X  %s\n", u32(36),
                u32(36) ? "(sub_FF8C3BFC ran!)" : "(=0, pipeline NOT connected!)");
        fprintf(stderr, "    +0x6C  rec buffer     = 0x%08X  %s\n", u32(40),
                u32(40) ? "(set)" : "(=0, NOT set!)");
        fprintf(stderr, "    +0x118 rec callback 2 = 0x%08X  %s\n", u32(44),
                u32(44) ? "(set)" : "(=0, NOT set!)");
        fprintf(stderr, "  JPCORE flag @0x7228C    = %u\n", u32(48));
        fprintf(stderr, "  movie_status @0x51E4    = %u  %s\n", u32(52),
                u32(52) == 4 ? "(=4, RECORDING)" :
                u32(52) == 1 ? "(=1, STOPPED)" :
                u32(52) == 5 ? "(=5, STOPPING)" : "");
        fprintf(stderr, "  wait retries used       = %u / 40\n", u32(56));
        {
            uint32_t jpwr_packed = u32(60);
            if ((jpwr_packed >> 24) == 0xAB) {
                uint32_t init_flag = jpwr_packed & 0xFF;
                uint32_t ref_count = (jpwr_packed >> 8) & 0xFF;
                uint32_t sec_flag = (jpwr_packed >> 16) & 0xFF;
                fprintf(stderr, "  JPCORE POWER @0x8028:\n");
                fprintf(stderr, "    init flag            = %u  %s\n", init_flag,
                        init_flag ? "(ok)" : "*** NOT INITIALIZED! FUN_ff8eeb6c is NO-OP! ***");
                fprintf(stderr, "    ref count            = %u\n", ref_count);
                fprintf(stderr, "    secondary flag       = %u\n", sec_flag);
            }
        }
        fprintf(stderr, "  VRAM@0x40EA23D0: ");
        for (int i = 64; i < 80 && i < (int)dummy.size(); i++)
            fprintf(stderr, "%02X ", dummy[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "  MjpegActiveCheck()      = %u\n", u32(80));
        fprintf(stderr, "  dims                    = %ux%u\n", u32(84), u32(88));
        if (dummy.size() >= 128) {
            uint32_t jpcore_out = u32(104);
            fprintf(stderr, "  JPCORE out buf @0x2564  = 0x%08X\n", jpcore_out);
            fprintf(stderr, "  data@JPCORE out: %02X %02X %02X %02X",
                    dummy[108], dummy[109], dummy[110], dummy[111]);
            if (dummy[108] == 0xFF && dummy[109] == 0xD8)
                fprintf(stderr, "  << JPEG SOI FOUND!");
            fprintf(stderr, "\n");
        }
        if (dummy.size() >= 192) {
            fprintf(stderr, "  piVar1[3] DMA active   = %u  %s\n", u32(128),
                    u32(128) == 1 ? "(JPCORE active)" : "(NOT active!)");
            fprintf(stderr, "  piVar1[10] mode index  = %u\n", u32(172));
            fprintf(stderr, "  +0xF0 frame skip       = %u\n", u32(184));
        }
        if (dummy.size() >= 256) {
            uint32_t cmask = u32(196);
            fprintf(stderr, "  PS3 completion mask    = 0x%X  %s\n", cmask,
                    cmask == 6 ? "(=6, JPCORE started OK)" :
                    cmask == 7 ? "(=7, JPCORE_DMA_Start FAILED)" : "(incomplete)");
            fprintf(stderr, "    bit0(JPCORE_fail)=%u  bit1(pre)=%u  bit2(post)=%u\n",
                    cmask & 1, (cmask >> 1) & 1, (cmask >> 2) & 1);
            fprintf(stderr, "  JPCORE_DMA_Start ret   = %u  %s\n", u32(200),
                    u32(200) == 0 ? "(=0, OK)" : "(=1, FAILED)");
            fprintf(stderr, "  JPCORE HW 0xC0F04908   = 0x%08X\n", u32(244));
            uint32_t soi_off = u32(248);
            if (soi_off == 0xFFFFFFFF)
                fprintf(stderr, "  SOI scan in piVar1[4]: NOT FOUND in 8KB\n");
            else
                fprintf(stderr, "  SOI scan in piVar1[4]: FOUND at offset %u\n", soi_off);
            uint32_t isp_src = u32(252);
            fprintf(stderr, "  ISP src 0xC0F110C4     = 0x%08X  %s\n", isp_src,
                    isp_src == 5 ? "(=5, VIDEO — JPCORE routed!)" :
                    isp_src == 4 ? "*** =4, EVF — JPCORE NOT routed! ***" :
                    "(unknown)");
        }
        // Recording start diagnostics (Option D: UIFS_StartMovieRecord)
        if (dummy.size() >= 288 && u32(256) == 0xD0D0D0D0) {
            fprintf(stderr, "\n  --- RECORDING START (Option D) ---\n");
            uint32_t rec_ret = u32(260);
            fprintf(stderr, "  UIFS_StartMovieRecord   = 0x%08X  %s\n", rec_ret,
                    rec_ret == 0 ? "(OK, event posted)" :
                    rec_ret == 0xFFFFFFF9 ? "*** FAIL: state+8 == 0 ***" :
                    rec_ret == 0xFFFFFFFD ? "*** FAIL: state+0x10 != 0 ***" :
                    "(unknown error)");
            fprintf(stderr, "  movie_status (final)    = %u  %s\n", u32(264),
                    u32(264) == 4 ? "(=4, RECORDING!)" :
                    u32(264) == 1 ? "(=1, STOPPED)" :
                    u32(264) == 0 ? "(=0, NEVER STARTED)" : "");
            fprintf(stderr, "  recording_active        = %u\n", u32(268));
            fprintf(stderr, "  raw movie_status        = %u\n", u32(272));
            fprintf(stderr, "  DAT_ff8834f0 precond:\n");
            if (u32(280) != 0xDEADDEAD) {
                fprintf(stderr, "    state+0x08 (!=0?)     = %u\n", u32(276));
                fprintf(stderr, "    state+0x10 (==0?)     = %u\n", u32(280));
            } else {
                fprintf(stderr, "    DAT_ff8834f0 = 0x%08X  (invalid pointer!)\n", u32(276));
            }
        }
        fprintf(stderr, "=== END DMA CHAIN DIAGNOSTICS ===\n\n");
    }

    return (resp.code == PTP_RC_OK);
}

bool PTPClient::stop_webcam() {
    PTPContainer resp{};
    std::vector<uint8_t> dummy;
    if (!impl_->chdk_command(CHDK_GetMJPEGFrame, 0, WEBCAM_STOP, 0, resp, &dummy)) {
        return false;
    }
    return (resp.code == PTP_RC_OK);
}

bool PTPClient::zoom(int delta) {
    // Accumulate zoom delta — will be sent piggybacked on next get_frame()
    impl_->pending_zoom += delta;
    return true;
}

bool PTPClient::get_frame(MJPEGFrame& frame) {
    PTPContainer resp{};
    std::vector<uint8_t> data;

    // Piggyback pending zoom on frame request (zero-cost zoom)
    uint32_t flags = 0;
    uint32_t zoom_p4 = 0;
    if (impl_->pending_zoom != 0) {
        flags = WEBCAM_ZOOM;
        zoom_p4 = static_cast<uint32_t>(impl_->pending_zoom);
        impl_->pending_zoom = 0;
    }

    if (!impl_->chdk_command(CHDK_GetMJPEGFrame, 0, flags, zoom_p4, resp, &data)) {
        impl_->last_error = "chdk_command failed: " + impl_->last_error;
        return false;
    }

    if (resp.code != PTP_RC_OK) {
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%04X", resp.code);
        impl_->last_error = std::string("PTP resp code=") + hex;
        return false;
    }

    if (resp.num_params < 1 || resp.params[0] == 0) {
        static int dbg_count = 0;
        if (false && dbg_count++ < 2) {
            // Decode hw diagnostics from param4: call|soi|eoi|hw_avail (8 bits each)
            uint32_t hw_diag = (resp.num_params >= 4) ? resp.params[3] : 0;
            fprintf(stderr, "  get_frame: active=%u gf_rc=%d hw_avail=%u fail:call=%u soi=%u eoi=%u\n",
                    resp.num_params >= 2 ? resp.params[1] : 0,
                    resp.num_params >= 3 ? static_cast<int>(resp.params[2]) : 0,
                    hw_diag & 0xFF,
                    (hw_diag >> 24) & 0xFF,
                    (hw_diag >> 16) & 0xFF,
                    (hw_diag >> 8) & 0xFF);

            // Parse HW diagnostic buffer v11 (512 bytes — 8 blocks)
            if (data.size() >= 64) {
                auto u32 = [&](int off) -> uint32_t {
                    if (off + 4 > (int)data.size()) return 0xDEADDEAD;
                    uint32_t v; memcpy(&v, data.data() + off, 4); return v;
                };
                auto hex16 = [&](int off) {
                    for (int i = off; i < off + 16 && i < (int)data.size(); i++)
                        fprintf(stderr, "%02X ", data[i]);
                };

                // ---- Block 0 (0-63): MJPEG State @ 0x70D8 ----
                fprintf(stderr, "  --- Block 0: MJPEG State @ 0x70D8 ---\n");
                fprintf(stderr, "    +0x48  MJPEG active   = %u\n", u32(0));
                fprintf(stderr, "    +0x4C  paired flag    = %u\n", u32(4));
                fprintf(stderr, "    +0x54  DMA status     = %u\n", u32(8));
                fprintf(stderr, "    +0x58  DMA frame idx  = %u\n", u32(12));
                fprintf(stderr, "    +0x5C  DMA req state  = %u\n", u32(16));
                fprintf(stderr, "    +0x60  ring buf addr  = 0x%08X\n", u32(20));
                fprintf(stderr, "    +0x64  VRAM buf addr  = 0x%08X\n", u32(24));
                fprintf(stderr, "    +0x6C  rec buffer     = 0x%08X\n", u32(28));
                fprintf(stderr, "    +0x80  cleanup cb     = 0x%08X\n", u32(32));
                fprintf(stderr, "    +0xA0  DMA callback   = 0x%08X\n", u32(36));
                fprintf(stderr, "    +0xB0  event flag     = 0x%08X\n", u32(40));
                fprintf(stderr, "    +0xD4  video mode     = %u (2=VGA)\n", u32(44));
                fprintf(stderr, "    +0xEC  pipeline active= %u\n", u32(48));
                fprintf(stderr, "    +0xF0  frame skip     = %u\n", u32(52));
                fprintf(stderr, "    +0x114 rec callback 1 = 0x%08X\n", u32(56));
                fprintf(stderr, "    +0x118 rec callback 2 = 0x%08X\n", u32(60));

                // ---- Block 1 (64-127): Movie Task @ 0x51A8 ----
                if (data.size() >= 128) {
                    fprintf(stderr, "  --- Block 1: Movie Task @ 0x51A8 ---\n");
                    fprintf(stderr, "    +0x00 base           = 0x%08X\n", u32(64));
                    fprintf(stderr, "    +0x1C msg queue      = 0x%08X\n", u32(68));
                    fprintf(stderr, "    +0x24 flag           = %u\n", u32(72));
                    fprintf(stderr, "    +0x28 counter        = %u\n", u32(76));
                    fprintf(stderr, "    dbg_queue write_idx  = %u\n", u32(80));
                    fprintf(stderr, "    dbg_queue read_idx   = %u\n", u32(84));
                    fprintf(stderr, "    +0x38 counter        = %u\n", u32(88));
                    fprintf(stderr, "    +0x3C STATE          = %u  %s\n", u32(92),
                            u32(92) == 0 ? "(idle)" :
                            u32(92) == 3 ? "(init)" :
                            u32(92) == 4 ? "(RECORDING)" :
                            u32(92) == 5 ? "(stopping)" : "");
                    fprintf(stderr, "    +0x48                = 0x%08X\n", u32(96));
                    fprintf(stderr, "    +0x4C                = 0x%08X\n", u32(100));
                    fprintf(stderr, "    +0x50 frame counter  = %u\n", u32(104));
                    fprintf(stderr, "    +0x54 error code     = 0x%08X\n", u32(108));
                    fprintf(stderr, "    +0xA0 callback       = 0x%08X\n", u32(112));
                    fprintf(stderr, "    +0x68                = 0x%08X\n", u32(116));
                    fprintf(stderr, "    +0x6C                = 0x%08X\n", u32(120));
                    fprintf(stderr, "    movie_status @0x51E4 = %u\n", u32(124));
                }

                // ---- Block 2 (128-191): Ring Buffer ----
                if (data.size() >= 192) {
                    uint32_t rb = u32(128);
                    fprintf(stderr, "  --- Block 2: Ring Buffer @ 0x%08X ---\n", rb);
                    if (u32(132) != 0xDEAD0000) {
                        fprintf(stderr, "    +0x1C write ptr      = 0x%08X\n", u32(132));
                        fprintf(stderr, "    +0x28 frame count    = %u\n", u32(136));
                        fprintf(stderr, "    +0x40 max frames     = %u\n", u32(140));
                        fprintf(stderr, "    +0x5C frame data sz  = %u\n", u32(144));
                        fprintf(stderr, "    +0x70 total frame sz = %u\n", u32(148));
                        fprintf(stderr, "    +0xBC index          = %u\n", u32(152));
                        fprintf(stderr, "    +0xC0 buf start      = 0x%08X\n", u32(156));
                        fprintf(stderr, "    +0xC4 buf end        = 0x%08X\n", u32(160));
                        fprintf(stderr, "    +0xC8 status         = %u\n", u32(164));
                        fprintf(stderr, "    +0xD4 value          = 0x%08X\n", u32(168));
                        fprintf(stderr, "    +0x148 remaining lo  = %u\n", u32(172));
                        fprintf(stderr, "    +0x14C remaining hi  = %u\n", u32(176));
                        fprintf(stderr, "    +0x150 used lo       = %u\n", u32(180));
                        fprintf(stderr, "    +0x154 used hi       = %u\n", u32(184));
                        fprintf(stderr, "    +0x94 divisor        = %u\n", u32(188));
                    } else {
                        fprintf(stderr, "    (ring buffer invalid)\n");
                    }
                }

                // ---- Block 3 (192-255): JPCORE + HW regs ----
                if (data.size() >= 256) {
                    fprintf(stderr, "  --- Block 3: JPCORE + HW ---\n");
                    fprintf(stderr, "    JPCORE flag @0x7228C = %u\n", u32(192));
                    fprintf(stderr, "    piVar1[0] init       = 0x%08X  %s\n", u32(196),
                            u32(196) == 0 ? "(NOT INIT!)" : "(ok)");
                    fprintf(stderr, "    piVar1[3] active     = %u  %s\n", u32(200),
                            u32(200) == 1 ? "(JPCORE active)" : "(NOT active!)");
                    fprintf(stderr, "    piVar1[4] output buf = 0x%08X\n", u32(204));
                    fprintf(stderr, "    piVar1[5]            = 0x%08X  %s\n", u32(208),
                            u32(208) == 0xFFFFFFFF ? "(=-1, BLOCKED!)" : "(ok)");
                    fprintf(stderr, "    piVar1[10] index     = %u\n", u32(212));
                    fprintf(stderr, "    buf[2] JPCORE out    = 0x%08X\n", u32(216));
                    uint32_t cmask = u32(220);
                    fprintf(stderr, "    PS3 completion mask  = 0x%X  %s\n", cmask,
                            cmask == 6 ? "(JPCORE OK)" :
                            cmask == 7 ? "(JPCORE FAIL)" : "");
                    fprintf(stderr, "    HW 0xC0F04908 DMA    = 0x%08X\n", u32(224));
                    fprintf(stderr, "    dbl-buf[0] +0x144    = 0x%08X\n", u32(228));
                    fprintf(stderr, "    dbl-buf[1] +0x148    = 0x%08X\n", u32(232));
                    fprintf(stderr, "    +0x120 param3        = 0x%08X\n", u32(236));
                    fprintf(stderr, "    +0xA4 (StopCont)     = 0x%08X\n", u32(240));
                    fprintf(stderr, "    +0xB8 semaphore      = 0x%08X\n", u32(244));
                    fprintf(stderr, "    iVar5 @0x12850       = 0x%08X\n", u32(248));
                    fprintf(stderr, "    source               = %u (0=none,1=spy,2=VRAM,3=db0,4=db1,5=pi4,6=b2,7=cb2)\n", u32(252));
                }

                // ---- Block 4 (256-319): Spy Results ----
                if (data.size() >= 320) {
                    fprintf(stderr, "  --- Block 4: SPY RESULTS ---\n");
                    uint32_t spy_magic = u32(256);
                    fprintf(stderr, "    spy magic            = 0x%08X  %s\n", spy_magic,
                            spy_magic == 0x52455753 ? "(\"SREW\" = FRAMES CAPTURED!)" : "(no frames yet)");
                    fprintf(stderr, "    spy jpeg_ptr         = 0x%08X\n", u32(260));
                    fprintf(stderr, "    spy jpeg_size        = %u\n", u32(264));
                    fprintf(stderr, "    spy frame_count      = %u\n", u32(268));
                    uint32_t init_flag = u32(272);
                    fprintf(stderr, "    spy init_flag        = 0x%08X  %s\n", init_flag,
                            init_flag == 0xCAFE0001 ? "(init case ran!)" : "(init NOT reached)");
                    fprintf(stderr, "    spy last error       = 0x%08X\n", u32(276));
                    fprintf(stderr, "    spy error count      = %u\n", u32(280));
                    fprintf(stderr, "    spy metadata1        = 0x%08X\n", u32(284));
                    fprintf(stderr, "    spy metadata2        = 0x%08X\n", u32(288));
                    fprintf(stderr, "    spy task_state       = %u\n", u32(292));
                    fprintf(stderr, "    spy task_0xA0        = 0x%08X\n", u32(296));
                    fprintf(stderr, "    rec_cb_count         = %u\n", u32(300));
                    fprintf(stderr, "    rec_cb_arg0          = 0x%08X\n", u32(304));
                    fprintf(stderr, "    rec_cb_arg1          = 0x%08X\n", u32(308));
                    fprintf(stderr, "    rec_cb_arg2          = 0x%08X\n", u32(312));
                    fprintf(stderr, "    rec_cb_arg3          = 0x%08X\n", u32(316));
                }

                // ---- Block 5 (320-383): Buffer Samples ----
                if (data.size() >= 384) {
                    fprintf(stderr, "  --- Block 5: Buffer Samples ---\n");
                    fprintf(stderr, "    VRAM@0x40EA23D0: "); hex16(320); fprintf(stderr, "\n");
                    fprintf(stderr, "    spy jpeg_ptr:    "); hex16(336); fprintf(stderr, "\n");
                    fprintf(stderr, "    rec_cb_arg2:     "); hex16(352); fprintf(stderr, "\n");
                    fprintf(stderr, "    rec_cb_arg2+256: "); hex16(368); fprintf(stderr, "\n");
                    // Check for JPEG SOI in any sample
                    for (int s = 0; s < 4; s++) {
                        int base = 320 + s * 16;
                        if (data[base] == 0xFF && data[base+1] == 0xD8)
                            fprintf(stderr, "    *** JPEG SOI found in sample %d! ***\n", s);
                    }
                }

                // ---- Block 6 (384-415): IDR Injection Debug ----
                if (data.size() >= 416) {
                    uint32_t idr_state = u32(384);
                    fprintf(stderr, "  --- Block 6: IDR Injection Debug ---\n");
                    fprintf(stderr, "    idr_dbg_state  = 0x%X  %s\n", idr_state,
                            idr_state == 0 ? "(not tried)" :
                            idr_state == 1 ? "(entered)" :
                            idr_state == 2 ? "(ptr ok)" :
                            idr_state == 3 ? "(scanning)" :
                            idr_state == 4 ? "(SUCCESS)" :
                            idr_state == 0xE0 ? "(ERR: ptr invalid)" :
                            idr_state == 0xE1 ? "(ERR: bad NAL len)" :
                            idr_state == 0xE2 ? "(ERR: buf overflow)" :
                            idr_state == 0xE3 ? "(ERR: bad start code)" :
                            idr_state == 0xE4 ? "(ERR: no IDR found)" : "(unknown)");
                    fprintf(stderr, "    idr_bytes24_27 = 0x%08X\n", u32(388));
                    fprintf(stderr, "    idr_dbg_ptr    = 0x%08X\n", u32(392));
                    fprintf(stderr, "    idr_dbg_size   = %u (0x%X)\n", u32(396), u32(396));
                    fprintf(stderr, "    idr_dbg_nals   = %u\n", u32(400));
                    fprintf(stderr, "    idr_dbg_dst    = %u\n", u32(404));
                    fprintf(stderr, "    idr_bytes28_31 = 0x%08X\n", u32(408));
                    uint32_t nal2 = u32(412);
                    fprintf(stderr, "    idr_nal2_info  = 0x%08X  (byte=0x%02X type=%u len_hi=0x%02X end_at_limit=%s)\n",
                            nal2, (nal2 >> 24) & 0xFF, (nal2 >> 16) & 0xFF,
                            (nal2 >> 8) & 0xFF, (nal2 & 0xFF) == 0xFF ? "YES" : "no");
                    fprintf(stderr, "    idr_injected   = %u\n", u32(416));
                }

                // ---- Block 7 (456-511): More JPCORE ----
                if (data.size() >= 512) {
                    fprintf(stderr, "  --- Block 7: JPCORE extended ---\n");
                    fprintf(stderr, "    piVar1[1]            = 0x%08X\n", u32(456));
                    fprintf(stderr, "    piVar1[2]            = 0x%08X\n", u32(460));
                    fprintf(stderr, "    piVar1[6]            = 0x%08X\n", u32(464));
                    fprintf(stderr, "    piVar1[7] sem        = 0x%08X\n", u32(468));
                    fprintf(stderr, "    piVar1[9]            = 0x%08X\n", u32(472));
                    fprintf(stderr, "    buf[0]               = 0x%08X\n", u32(476));
                    fprintf(stderr, "    buf[1]               = 0x%08X\n", u32(480));
                    fprintf(stderr, "    buf[4]               = 0x%08X\n", u32(484));
                    fprintf(stderr, "    buf[6]               = 0x%08X\n", u32(488));
                    fprintf(stderr, "    PS3 frame param      = 0x%08X\n", u32(492));
                    fprintf(stderr, "    JPCORE_DMA_Start ret = %u\n", u32(496));
                    fprintf(stderr, "    PS3 step2            = %u\n", u32(500));
                    fprintf(stderr, "    PS3 step3            = %u\n", u32(504));
                    fprintf(stderr, "    HW 0xC0F04900        = 0x%08X\n", u32(508));
                }

                // ---- Block 8 (512-575): HW or SW path diagnostics ----
                if (data.size() >= 552) {
                    uint32_t marker = u32(544);
                    if ((marker & 0xFFFF0000) == 0x53570000) {
                        // SW path diagnostics (marker = "SW" + hw_avail)
                        fprintf(stderr, "  --- Block 8: SW Path Diagnostics ---\n");
                        fprintf(stderr, "    sw_total_calls       = %u\n", u32(512));
                        fprintf(stderr, "    sw_fail_null (vp)    = %u\n", u32(516));
                        fprintf(stderr, "    sw_fail_stale        = %u\n", u32(520));
                        fprintf(stderr, "    sw_fail_encode       = %u\n", u32(524));
                        fprintf(stderr, "    sw_last_vp_addr      = 0x%08X\n", u32(528));
                        fprintf(stderr, "    last_vp_checksum     = 0x%08X\n", u32(532));
                        fprintf(stderr, "    frame_count          = %u\n", u32(536));
                        fprintf(stderr, "    last_jpeg_size       = %u\n", u32(540));
                        fprintf(stderr, "    hw_fail_total        = %u\n", u32(548));
                    } else {
                        // HW path diagnostics (ISR callback)
                        fprintf(stderr, "  --- Block 8: Callback Addrs + Counters ---\n");
                        fprintf(stderr, "    DMA reg 0xC0F04908   = 0x%08X\n", u32(512));
                        fprintf(stderr, "    arg2 value           = 0x%08X\n", u32(516));
                        fprintf(stderr, "    db0 addr             = 0x%08X\n", u32(520));
                        fprintf(stderr, "    db1 addr             = 0x%08X\n", u32(524));
                        fprintf(stderr, "    buf[2] addr          = 0x%08X\n", u32(528));
                        fprintf(stderr, "    callback count       = %u\n", u32(532));
                        fprintf(stderr, "    hw_fail_soi          = %u\n", u32(536));
                        fprintf(stderr, "    hw_fail_eoi          = %u\n", u32(540));
                        fprintf(stderr, "    hw_fail_total        = %u\n", u32(544));
                        fprintf(stderr, "    hw_frame_index       = %u\n", u32(548));
                    }
                    // JPCORE power struct @ 0x8028 (bytes 552-575, added in v14)
                    if (data.size() >= 576) {
                        uint32_t pwr_init = u32(552);
                        uint32_t pwr_sem = u32(556);
                        uint32_t pwr_sec = u32(560);
                        uint32_t pwr_ref = u32(564);
                        if (pwr_init != 0 || pwr_sem != 0 || pwr_ref != 0) {
                            fprintf(stderr, "  --- JPCORE Power @0x8028 ---\n");
                            fprintf(stderr, "    init flag            = %u  %s\n", pwr_init,
                                    pwr_init ? "(ok)" : "*** NOT INITIALIZED! ***");
                            fprintf(stderr, "    semaphore            = 0x%08X\n", pwr_sem);
                            fprintf(stderr, "    secondary flag       = %u\n", pwr_sec);
                            fprintf(stderr, "    ref count            = %u\n", pwr_ref);
                            fprintf(stderr, "    offset 0x08          = 0x%08X\n", u32(568));
                            fprintf(stderr, "    offset 0x0C          = 0x%08X\n", u32(572));
                        }
                    }
                }
            }
        }
        {
            int gf_rc = resp.num_params >= 3 ? static_cast<int>(resp.params[2]) : 0;
            impl_->last_error = "no frame (gf_rc=" + std::to_string(gf_rc) + ")";
        }
        return false;
    }

    // Audio piggybacked: last 2940 bytes of data are PCM audio
    // (44100Hz mono 16-bit, fixed size = 88200 / 30fps)
    constexpr uint32_t AUDIO_PER_FRAME = 2940;
    if (data.size() > AUDIO_PER_FRAME) {
        size_t video_sz = data.size() - AUDIO_PER_FRAME;
        frame.audio_data.assign(data.begin() + video_sz, data.end());
        data.resize(video_sz);
    }
    frame.data = std::move(data);
    frame.width = (resp.num_params >= 2) ? resp.params[1] : 0;
    frame.height = (resp.num_params >= 3) ? resp.params[2] : 0;
    // Format is encoded in high byte of param4, frame_num in low 24 bits
    if (resp.num_params >= 4) {
        frame.format = (resp.params[3] >> 24) & 0xFF;
        frame.frame_num = resp.params[3] & 0x00FFFFFF;
    } else {
        frame.format = FRAME_FMT_JPEG;
        frame.frame_num = 0;
    }

    if (frame.data.empty()) {
        impl_->last_error = "empty frame data";
        return false;
    }

    return true;
}

bool PTPClient::execute_script(const std::string& script) {
    // CHDK ExecuteScript: command with data phase containing null-terminated script.
    // param1 = CHDK_ExecuteScript, param2 = language (0=Lua)
    uint32_t params[5] = { CHDK_ExecuteScript, 0, 0, 0, 0 };

    // Send command first (this increments transaction_id)
    if (!impl_->send_command(PTP_OC_CHDK, params, 2)) return false;

    // Build data packet with SAME transaction_id as command
    std::vector<uint8_t> pkt;
    int script_len = static_cast<int>(script.size()) + 1;  // include null terminator
    int data_len = PTP_HEADER_SIZE + script_len;
    pkt.resize(data_len);

    memcpy(pkt.data() + 0, &data_len, 4);
    uint16_t type = PTP_TYPE_DATA;
    memcpy(pkt.data() + 4, &type, 2);
    uint16_t opcode = PTP_OC_CHDK;
    memcpy(pkt.data() + 6, &opcode, 2);
    uint32_t tid = impl_->transaction_id;  // same as command
    memcpy(pkt.data() + 8, &tid, 4);
    memcpy(pkt.data() + PTP_HEADER_SIZE, script.data(), script.size());
    pkt[PTP_HEADER_SIZE + script.size()] = 0;  // null terminate

    // Send data
    int transferred = 0;
    int r = libusb_bulk_transfer(impl_->handle, impl_->ep_out,
                                  pkt.data(), data_len, &transferred, USB_TIMEOUT);
    if (r < 0) return false;

    // Get response
    PTPContainer resp{};
    return impl_->receive_response(resp) && resp.code == PTP_RC_OK;
}

bool PTPClient::read_script_msg(std::string& msg) {
    PTPContainer resp{};
    std::vector<uint8_t> data;
    if (!impl_->chdk_command(CHDK_ReadScriptMsg, 0, 0, 0, resp, &data)) {
        return false;
    }
    if (resp.code != PTP_RC_OK || data.size() < 2) {
        msg.clear();
        return true;  // no message available
    }
    // CHDK on this camera returns raw message data (no header)
    // data.size() == 1 means "no message" (single null byte)
    msg.assign(reinterpret_cast<const char*>(data.data()), data.size());
    while (!msg.empty() && msg.back() == '\0') msg.pop_back();
    return true;
}

bool PTPClient::read_memory(uint32_t address, uint32_t size, std::vector<uint8_t>& data) {
    PTPContainer resp{};
    if (!impl_->chdk_command(CHDK_GetMemory, address, size, 0, resp, &data)) {
        return false;
    }
    return (resp.code == PTP_RC_OK && !data.empty());
}

bool PTPClient::upload_file(const std::string& remote_path, const std::vector<uint8_t>& data) {
    // CHDK UploadFile protocol:
    // Command: opcode=0x9999, param1=CHDK_UploadFile
    // Data phase: [4-byte path_len][path bytes (no null)][file data]
    uint32_t path_len = static_cast<uint32_t>(remote_path.size());
    uint32_t params[5] = { CHDK_UploadFile, 0, 0, 0, 0 };

    // Build data packet: PTP header + 4-byte path_len + path + file data
    int payload_size = 4 + static_cast<int>(path_len) + static_cast<int>(data.size());
    int data_total = PTP_HEADER_SIZE + payload_size;
    std::vector<uint8_t> pkt(data_total);

    uint32_t pkt_len = static_cast<uint32_t>(data_total);
    memcpy(pkt.data() + 0, &pkt_len, 4);
    uint16_t type = PTP_TYPE_DATA;
    memcpy(pkt.data() + 4, &type, 2);
    uint16_t opcode = PTP_OC_CHDK;
    memcpy(pkt.data() + 6, &opcode, 2);
    uint32_t tid = impl_->transaction_id + 1;
    memcpy(pkt.data() + 8, &tid, 4);
    // Payload: 4-byte LE path length + path string + file data
    memcpy(pkt.data() + PTP_HEADER_SIZE, &path_len, 4);
    memcpy(pkt.data() + PTP_HEADER_SIZE + 4, remote_path.data(), path_len);
    if (!data.empty())
        memcpy(pkt.data() + PTP_HEADER_SIZE + 4 + path_len, data.data(), data.size());

    // Send command
    if (!impl_->send_command(PTP_OC_CHDK, params, 1)) {
        impl_->last_error = "upload_file: send_command failed";
        return false;
    }

    // Send data (may be large, use longer timeout)
    int transferred = 0;
    int r = libusb_bulk_transfer(impl_->handle, impl_->ep_out,
                                  pkt.data(), data_total, &transferred, 10000);
    if (r < 0) {
        impl_->last_error = "upload_file: bulk transfer failed: " +
                            std::string(libusb_strerror(static_cast<libusb_error>(r)));
        return false;
    }

    // Get response
    PTPContainer resp{};
    if (!impl_->receive_response(resp) || resp.code != PTP_RC_OK) {
        impl_->last_error = "upload_file: camera rejected (code=" +
                            std::to_string(resp.code) + ")";
        return false;
    }
    return true;
}

std::string PTPClient::get_last_error() const {
    return impl_->last_error;
}

PTPClient::USBStats PTPClient::get_usb_stats() const {
    USBStats s;
    s.send_errors = impl_->usb_send_errors;
    s.recv_errors = impl_->usb_recv_errors;
    s.timeout_errors = impl_->usb_timeout_errors;
    s.io_errors = impl_->usb_io_errors;
    return s;
}

} // namespace ptp
