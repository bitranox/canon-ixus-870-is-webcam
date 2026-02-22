// chdk-webcam: Bridge between Canon IXUS 870 IS (CHDK) and virtual webcam
//
// Connects to a Canon camera running CHDK via PTP/USB, retrieves MJPEG frames
// from the webcam module, decodes and upscales them, and feeds them into a
// virtual webcam device visible to Zoom, Teams, OBS, etc.
//
// Usage:
//   chdk-webcam [options]
//
// Options:
//   -q, --quality N     JPEG quality on camera (1-100, default 50)
//   -w, --width N       Output width (default 1280)
//   -h, --height N      Output height (default 720)
//   -f, --fps N         Target frame rate (default 30)
//   --flip-h            Flip image horizontally (mirror)
//   --flip-v            Flip image vertically
//   --no-webcam         Don't create virtual webcam (display only)
//   --verbose           Show frame statistics
//   --timeout N         Exit after N seconds (graceful shutdown)

#include "ptp/ptp_client.h"
#include "webcam/frame_processor.h"
#include "webcam/preview_window.h"
#include "webcam/virtual_webcam.h"
#ifdef HAS_FFMPEG
#include "webcam/h264_decoder.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <csignal>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// Global flag for graceful shutdown
static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_running = false;
        // Give cleanup time to run before Windows kills us
        Sleep(5000);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static FILE* g_debug_log = nullptr;

static void print_debug_frame(const uint8_t* data, size_t size)
{
    if (size < 12 || data[0] != 'D' || data[1] != 'B' || data[2] != 'G' || data[3] != '!') {
        fprintf(stderr, "  [DBG] Invalid debug frame (%zu bytes)\n", size);
        return;
    }

    uint32_t seq;
    uint16_t count;
    memcpy(&seq, data + 4, 4);
    memcpy(&count, data + 8, 2);

    fprintf(stderr, "\n=== DEBUG FRAME #%u (%u entries, %zu bytes) ===\n", seq, count, size);

    size_t off = 12;
    for (uint16_t i = 0; i < count && off + 8 <= size; i++) {
        char tag[5] = { (char)data[off], (char)data[off+1],
                        (char)data[off+2], (char)data[off+3], 0 };
        uint32_t val;
        memcpy(&val, data + off + 4, 4);
        fprintf(stderr, "  %-4s = 0x%08X  (%u)\n", tag, val, val);
        off += 8;
    }
    fprintf(stderr, "=== END DEBUG ===\n\n");

    // Append to log file
    if (!g_debug_log)
        g_debug_log = fopen("debug_frames.log", "a");
    if (g_debug_log) {
        fprintf(g_debug_log, "DEBUG #%u (%u entries):\n", seq, count);
        off = 12;
        for (uint16_t i = 0; i < count && off + 8 <= size; i++) {
            char tag[5] = { (char)data[off], (char)data[off+1],
                            (char)data[off+2], (char)data[off+3], 0 };
            uint32_t val;
            memcpy(&val, data + off + 4, 4);
            fprintf(g_debug_log, "  %-4s = 0x%08X  (%u)\n", tag, val, val);
            off += 8;
        }
        fprintf(g_debug_log, "\n");
        fflush(g_debug_log);
    }
}

struct Options {
    int jpeg_quality = 50;
    int output_width = 1280;
    int output_height = 720;
    int target_fps = 30;
    bool flip_h = false;
    bool flip_v = false;
    bool no_webcam = false;
    bool no_preview = false;
    bool verbose = false;
    int timeout_sec = 0;
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "CHDK Webcam Bridge v1.0\n"
        "Streams video from Canon IXUS 870 IS (CHDK) as a virtual webcam.\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -q, --quality N     JPEG quality on camera (1-100, default 50)\n"
        "  -w, --width N       Output width (default 1280)\n"
        "  -h, --height N      Output height (default 720)\n"
        "  -f, --fps N         Target FPS (default 30)\n"
        "  --flip-h            Mirror horizontally\n"
        "  --flip-v            Flip vertically\n"
        "  --no-webcam         Skip virtual webcam creation\n"
        "  --no-preview        Skip preview window\n"
        "  --verbose           Show per-frame statistics\n"
        "  --timeout N         Exit after N seconds (graceful shutdown)\n"
        "  --help              Show this help\n"
        "\n"
        "Prerequisites:\n"
        "  1. Camera running CHDK with webcam module\n"
        "  2. libusb-win32 driver installed (use Zadig)\n"
        "  3. No other PTP clients running (Canon software, Windows photo import)\n"
        "\n", prog);
}

static Options parse_args(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-q" || arg == "--quality") && i + 1 < argc) {
            opts.jpeg_quality = atoi(argv[++i]);
        } else if ((arg == "-w" || arg == "--width") && i + 1 < argc) {
            opts.output_width = atoi(argv[++i]);
        } else if ((arg == "-h" || arg == "--height") && i + 1 < argc) {
            opts.output_height = atoi(argv[++i]);
        } else if ((arg == "-f" || arg == "--fps") && i + 1 < argc) {
            opts.target_fps = atoi(argv[++i]);
        } else if (arg == "--flip-h") {
            opts.flip_h = true;
        } else if (arg == "--flip-v") {
            opts.flip_v = true;
        } else if (arg == "--no-webcam") {
            opts.no_webcam = true;
        } else if (arg == "--no-preview") {
            opts.no_preview = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--timeout" && i + 1 < argc) {
            opts.timeout_sec = atoi(argv[++i]);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            exit(1);
        }
    }
    return opts;
}

int main(int argc, char* argv[]) {
    Options opts = parse_args(argc, argv);

    // Install signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

#ifdef _WIN32
    // Enable ANSI escape codes on Windows 10+
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(hConsole, &mode)) {
        SetConsoleMode(hConsole, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif

    printf("CHDK Webcam Bridge v1.0\n");
    printf("Output: %dx%d @ %d FPS, JPEG quality: %d\n\n",
           opts.output_width, opts.output_height, opts.target_fps, opts.jpeg_quality);

    // --- Connect to camera ---
    printf("Connecting to camera...\n");
    ptp::PTPClient client;
    if (!client.connect()) {
        fprintf(stderr, "ERROR: %s\n", client.get_last_error().c_str());
        return 1;
    }

    auto info = client.get_camera_info();
    printf("Connected: %s\n", info.description.c_str());
    printf("CHDK protocol: %d.%d\n\n", info.chdk_major, info.chdk_minor);

    // --- Initialize frame processor ---
    webcam::FrameProcessor processor;
    {
        webcam::ProcessorConfig pconf;
        pconf.output_width = opts.output_width;
        pconf.output_height = opts.output_height;
        pconf.flip_horizontal = opts.flip_h;
        pconf.flip_vertical = opts.flip_v;
        processor.configure(pconf);
    }

    // --- Initialize H.264 decoder ---
#ifdef HAS_FFMPEG
    webcam::H264Decoder h264dec;
    {
        if (!h264dec.init(opts.output_width, opts.output_height)) {
            fprintf(stderr, "WARNING: H.264 decoder init failed: %s\n", h264dec.get_last_error().c_str());
            fprintf(stderr, "H.264 frames will be skipped.\n");
        } else {
            printf("H.264 decoder ready (FFmpeg)\n");
        }
    }
#endif

    // --- Initialize virtual webcam ---
    webcam::VirtualWebcam vwebcam;
    if (!opts.no_webcam) {
        printf("Creating virtual webcam...\n");
        webcam::VirtualWebcamConfig wconf;
        wconf.width = opts.output_width;
        wconf.height = opts.output_height;
        wconf.fps = opts.target_fps;
        wconf.name = "CHDK Webcam";
        if (!vwebcam.init(wconf)) {
            fprintf(stderr, "WARNING: Virtual webcam init failed: %s\n", vwebcam.get_last_error().c_str());
            fprintf(stderr, "Continuing without virtual webcam output.\n");
        } else {
            printf("Virtual webcam active: \"%s\"\n", wconf.name.c_str());
        }
    }

    // --- Initialize preview window ---
    webcam::PreviewWindow preview;
    if (!opts.no_preview) {
        printf("Opening preview window...\n");
        webcam::PreviewConfig pvc;
        pvc.width = 640;
        pvc.height = 480;
        pvc.title = "CHDK Webcam Preview";
        if (!preview.init(pvc)) {
            fprintf(stderr, "WARNING: Preview window failed to open.\n");
        }
    }

    // --- Start webcam streaming on camera ---
    printf("Starting webcam on camera (quality=%d)...\n", opts.jpeg_quality);
    bool start_ok = client.start_webcam(opts.jpeg_quality);
    if (!start_ok) {
        fprintf(stderr, "WARNING: start_webcam returned false: %s\n", client.get_last_error().c_str());
        fprintf(stderr, "Will try to get frames anyway (module may auto-load).\n");
    }
    if (opts.timeout_sec > 0)
        printf("\nStreaming for %d seconds. Press Ctrl+C to stop early.\n\n", opts.timeout_sec);
    else
        printf("\nStreaming. Press Ctrl+C to stop.\n\n");

    // --- Main streaming loop ---
    auto start_time = std::chrono::steady_clock::now();
    auto stats_interval = std::chrono::seconds(2);
    auto last_stats = std::chrono::steady_clock::now();
    int frames_received = 0;
    int frames_dropped = 0;
    int frames_skipped = 0;      // frames produced by camera but never received (frame_num gaps)
    int total_bytes = 0;
    uint32_t last_frame_num = 0;

    // Cumulative stats for final summary
    int total_received = 0;
    int total_dropped = 0;
    int total_skipped = 0;

    // Frame data uniqueness tracking: detect if camera sends identical data
    uint32_t prev_data_hash = 0;
    int unique_frames = 0;
    int duplicate_frames = 0;

    while (g_running) {
        // Check timeout
        if (opts.timeout_sec > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= std::chrono::seconds(opts.timeout_sec)) {
                printf("\nTimeout reached (%d seconds).\n", opts.timeout_sec);
                g_running = false;
                break;
            }
        }

        // Pump window messages first (must run every iteration, not just on successful frames)
        if (preview.is_open() && !preview.pump_messages()) {
            g_running = false;
            break;
        }

        auto frame_start = std::chrono::steady_clock::now();

        // Get frame from camera
        ptp::MJPEGFrame mjpeg;
        if (!client.get_frame(mjpeg)) {
            frames_dropped++;
            // Print stats even when all frames drop
            auto now_check = std::chrono::steady_clock::now();
            if (now_check - last_stats >= stats_interval) {
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_check - last_stats).count();
                printf("\r[Stats] FPS: 0.0 | Recv: 0 | Drop: %d | Skip: %d | %s    \n",
                       frames_dropped, frames_skipped, client.get_last_error().c_str());
                total_dropped += frames_dropped;
                total_skipped += frames_skipped;
                frames_dropped = 0;
                frames_skipped = 0;
                last_stats = now_check;
            }
            // Sleep before retry (failure path only — rarely hit with seqlock).
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Check for duplicate frames
        if (mjpeg.frame_num == last_frame_num && last_frame_num != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Detect frame gaps (frames produced by camera but never received)
        if (last_frame_num != 0 && mjpeg.frame_num > last_frame_num + 1) {
            int gap = mjpeg.frame_num - last_frame_num - 1;
            frames_skipped += gap;
            fprintf(stderr, "FRAME GAP: %d frames lost (cam #%u -> #%u)\n",
                    gap, last_frame_num, mjpeg.frame_num);
        }
        last_frame_num = mjpeg.frame_num;

        // Handle debug frames — not video, just diagnostic data
        if (mjpeg.format == ptp::FRAME_FMT_DEBUG) {
            print_debug_frame(mjpeg.data.data(), mjpeg.data.size());

            // Memory probes disabled — IDR injection working since v23
            if (false && mjpeg.data.size() >= 20 && mjpeg.data[12] == 'S' && mjpeg.data[13] == 'r') {
                uint32_t src_val;
                memcpy(&src_val, mjpeg.data.data() + 16, 4);
                if (src_val == 0x4D362E32) {  // "M6.2"
                    // Search debug frame entries by tag name (robust against layout changes)
                    auto find_tag = [&](const char tag[4]) -> uint32_t {
                        size_t pos = 12;  // skip header
                        while (pos + 8 <= mjpeg.data.size()) {
                            if (mjpeg.data[pos] == tag[0] && mjpeg.data[pos+1] == tag[1] &&
                                mjpeg.data[pos+2] == tag[2] && mjpeg.data[pos+3] == tag[3]) {
                                uint32_t v; memcpy(&v, mjpeg.data.data() + pos + 4, 4);
                                return v;
                            }
                            pos += 8;
                        }
                        return 0;
                    };
                    uint32_t rb_base = find_tag("RBas");
                    uint32_t idr_ptr = find_tag("IdrP");
                    uint32_t idr_size = find_tag("IdrS");
                    fprintf(stderr, "\n=== MEMORY PROBE (after M6.2) ===\n");
                    fprintf(stderr, "  rb_base=0x%08X  IdrP=0x%08X  IdrS=%u\n", rb_base, idr_ptr, idr_size);

                    // Probe 1: ring buffer struct fields +0xC0 through +0xE0
                    if (rb_base && rb_base < 0x40000000) {
                        std::vector<uint8_t> mem;
                        if (client.read_memory(rb_base + 0xC0, 32, mem)) {
                            fprintf(stderr, "  rb_base+0xC0 (32 bytes):");
                            for (size_t i = 0; i < mem.size(); i++) {
                                if (i % 4 == 0) fprintf(stderr, " ");
                                fprintf(stderr, "%02X", mem[i]);
                            }
                            fprintf(stderr, "\n");
                            for (int i = 0; i + 4 <= (int)mem.size(); i += 4) {
                                uint32_t v; memcpy(&v, mem.data() + i, 4);
                                fprintf(stderr, "    +0x%02X = 0x%08X  (%u)\n", 0xC0 + i, v, v);
                            }
                        } else {
                            fprintf(stderr, "  rb_base+0xC0: read failed: %s\n", client.get_last_error().c_str());
                        }
                    }

                    // Probe 2: data at raw IdrP address (first 32 bytes)
                    if (idr_ptr && idr_ptr < 0x40000000) {
                        std::vector<uint8_t> mem;
                        if (client.read_memory(idr_ptr, 32, mem)) {
                            fprintf(stderr, "  @IdrP 0x%08X (32 bytes):", idr_ptr);
                            for (size_t i = 0; i < mem.size(); i++) {
                                if (i % 4 == 0) fprintf(stderr, " ");
                                fprintf(stderr, "%02X", mem[i]);
                            }
                            fprintf(stderr, "\n");
                        } else {
                            fprintf(stderr, "  @IdrP 0x%08X: read failed: %s\n", idr_ptr, client.get_last_error().c_str());
                        }
                    }

                    // Probe 3: read +0xD0 value then use it as base for IdrP offset
                    if (rb_base && rb_base < 0x40000000) {
                        std::vector<uint8_t> d0_mem;
                        if (client.read_memory(rb_base + 0xD0, 4, d0_mem) && d0_mem.size() >= 4) {
                            uint32_t d0_val;
                            memcpy(&d0_val, d0_mem.data(), 4);
                            fprintf(stderr, "  +0xD0 = 0x%08X\n", d0_val);
                            if (d0_val && idr_ptr) {
                                uint32_t idr_abs = d0_val + idr_ptr;
                                fprintf(stderr, "  @(+0xD0)+IdrP 0x%08X (32 bytes):", idr_abs);
                                std::vector<uint8_t> mem;
                                if (client.read_memory(idr_abs, 32, mem)) {
                                    for (size_t i = 0; i < mem.size(); i++) {
                                        if (i % 4 == 0) fprintf(stderr, " ");
                                        fprintf(stderr, "%02X", mem[i]);
                                    }
                                    fprintf(stderr, "\n");
                                } else {
                                    fprintf(stderr, " read failed: %s\n", client.get_last_error().c_str());
                                }
                            }
                        }
                    }

                    // Probe 4: read +0xC0 value then use it as base for IdrP offset
                    if (rb_base && rb_base < 0x40000000) {
                        std::vector<uint8_t> c0_mem;
                        if (client.read_memory(rb_base + 0xC0, 4, c0_mem) && c0_mem.size() >= 4) {
                            uint32_t c0_val;
                            memcpy(&c0_val, c0_mem.data(), 4);
                            fprintf(stderr, "  +0xC0 = 0x%08X\n", c0_val);
                            if (c0_val && idr_ptr) {
                                uint32_t idr_abs = c0_val + idr_ptr;
                                fprintf(stderr, "  @(+0xC0)+IdrP 0x%08X (32 bytes):", idr_abs);
                                std::vector<uint8_t> mem;
                                if (client.read_memory(idr_abs, 32, mem)) {
                                    for (size_t i = 0; i < mem.size(); i++) {
                                        if (i % 4 == 0) fprintf(stderr, " ");
                                        fprintf(stderr, "%02X", mem[i]);
                                    }
                                    fprintf(stderr, "\n");
                                } else {
                                    fprintf(stderr, " read failed: %s\n", client.get_last_error().c_str());
                                }
                            }
                        }
                    }
                    // Probe 5: uncached version of +0xD0 base (0x40000000 | addr)
                    if (rb_base && rb_base < 0x40000000) {
                        std::vector<uint8_t> d0_mem;
                        if (client.read_memory(rb_base + 0xD0, 4, d0_mem) && d0_mem.size() >= 4) {
                            uint32_t d0_val;
                            memcpy(&d0_val, d0_mem.data(), 4);
                            uint32_t uncached = (d0_val | 0x40000000) + idr_ptr;
                            fprintf(stderr, "  @uncached 0x%08X (32 bytes):", uncached);
                            std::vector<uint8_t> mem;
                            if (client.read_memory(uncached, 32, mem)) {
                                for (size_t i = 0; i < mem.size(); i++) {
                                    if (i % 4 == 0) fprintf(stderr, " ");
                                    fprintf(stderr, "%02X", mem[i]);
                                }
                                fprintf(stderr, "\n");
                            } else {
                                fprintf(stderr, " read failed: %s\n", client.get_last_error().c_str());
                            }
                        }
                    }

                    // Probe 6: read first P-frame pointer from hdr[1] and show its address
                    {
                        std::vector<uint8_t> hdr_mem;
                        if (client.read_memory(0x000FF004, 8, hdr_mem) && hdr_mem.size() >= 8) {
                            uint32_t frame_ptr, frame_sz;
                            memcpy(&frame_ptr, hdr_mem.data(), 4);
                            memcpy(&frame_sz, hdr_mem.data() + 4, 4);
                            fprintf(stderr, "  P-frame ptr=0x%08X size=%u\n", frame_ptr, frame_sz);
                            if (frame_ptr && frame_ptr < 0x80000000) {
                                std::vector<uint8_t> mem;
                                if (client.read_memory(frame_ptr, 16, mem)) {
                                    fprintf(stderr, "  @P-frame (16 bytes):");
                                    for (size_t i = 0; i < mem.size(); i++) {
                                        if (i % 4 == 0) fprintf(stderr, " ");
                                        fprintf(stderr, "%02X", mem[i]);
                                    }
                                    fprintf(stderr, "\n");
                                }
                            }
                        }
                    }
                    fprintf(stderr, "=== END MEMORY PROBE ===\n\n");
                }
            }

            continue;  // Not a video frame — don't count or decode
        }

        // Track frame data uniqueness (simple FNV-1a hash of first 256 bytes)
        {
            uint32_t h = 0x811c9dc5;
            size_t n = (mjpeg.data.size() < 256) ? mjpeg.data.size() : 256;
            for (size_t i = 0; i < n; i++) {
                h ^= mjpeg.data[i];
                h *= 0x01000193;
            }
            if (h == prev_data_hash && prev_data_hash != 0) {
                duplicate_frames++;
            } else {
                unique_frames++;
                // Log first 16 bytes and NAL type for first 10 unique frames
                if (unique_frames <= 10) {
                    fprintf(stderr, "UNIQUE #%d (cam#%u, %zu bytes): ",
                            unique_frames, mjpeg.frame_num, mjpeg.data.size());
                    for (size_t i = 0; i < 16 && i < mjpeg.data.size(); i++)
                        fprintf(stderr, "%02X ", mjpeg.data[i]);
                    if (mjpeg.data.size() >= 5)
                        fprintf(stderr, " NAL=0x%02X (type %d)", mjpeg.data[4], mjpeg.data[4] & 0x1F);
                    fprintf(stderr, "\n");
                }
            }
            prev_data_hash = h;
        }

        // Decode frame (JPEG, raw UYVY, or H.264)
        webcam::RGBFrame rgb;
        bool decode_ok;
        if (mjpeg.format == ptp::FRAME_FMT_H264) {
#ifdef HAS_FFMPEG
            decode_ok = h264dec.decode(mjpeg.data.data(), mjpeg.data.size(), rgb);
            if (!decode_ok) {
                // Decoder lost sync (dropped P-frames). Re-inject stored IDR
                // to reset reference pictures, then retry the current P-frame.
                static int idr_resets = 0;
                if (h264dec.reinject_idr(rgb)) {
                    idr_resets++;
                    if (idr_resets <= 5 || idr_resets % 50 == 0) {
                        fprintf(stderr, "H.264: IDR re-inject #%d, retrying P-frame\n", idr_resets);
                    }
                    // IDR decoded OK — now retry the P-frame that failed
                    decode_ok = h264dec.decode(mjpeg.data.data(), mjpeg.data.size(), rgb);
                }
                if (!decode_ok) {
                    static int h264_skip = 0;
                    h264_skip++;
                    if (h264_skip <= 5 || h264_skip % 100 == 0) {
                        fprintf(stderr, "H.264 FAIL #%d: %zu bytes, err=%s\n",
                                h264_skip, mjpeg.data.size(), h264dec.get_last_error().c_str());
                    }
                    frames_dropped++;
                    continue;
                }
            }
#else
            static int h264_count = 0;
            h264_count++;
            if (h264_count <= 3 || h264_count % 100 == 0) {
                fprintf(stderr, "H.264 frame (%zu bytes) — no FFmpeg, skipping.\n", mjpeg.data.size());
            }
            frames_dropped++;
            continue;
#endif
        } else if (mjpeg.format == ptp::FRAME_FMT_UYVY) {
            decode_ok = processor.process_uyvy(mjpeg.data.data(), static_cast<int>(mjpeg.data.size()),
                                               mjpeg.width, mjpeg.height, rgb);
        } else {
            decode_ok = processor.process(mjpeg.data.data(), static_cast<int>(mjpeg.data.size()), rgb);
        }
        if (!decode_ok) {
            if (opts.verbose) {
                fprintf(stderr, "Decode error: %s\n", processor.get_last_error().c_str());
            }
            frames_dropped++;
            continue;
        }

        // Send to virtual webcam
        if (vwebcam.is_active()) {
            vwebcam.send_frame(rgb.data.data(), rgb.width, rgb.height, rgb.stride);
        }

        // Send to preview window
        if (preview.is_open()) {
            preview.show_frame(rgb.data.data(), rgb.width, rgb.height, rgb.stride);
        }

        // Log format on first frame
        if (frames_received == 0) {
            const char* fmt_name = "JPEG";
            if (mjpeg.format == ptp::FRAME_FMT_UYVY) fmt_name = "UYVY (raw)";
            else if (mjpeg.format == ptp::FRAME_FMT_H264) fmt_name = "H.264";
            else if (mjpeg.format == ptp::FRAME_FMT_DEBUG) fmt_name = "DEBUG";
            printf("First frame: %ux%u, %zu bytes, format=%s\n",
                   mjpeg.width, mjpeg.height, mjpeg.data.size(), fmt_name);
        }

        frames_received++;
        total_bytes += static_cast<int>(mjpeg.data.size());

        if (opts.verbose) {
            printf("\rFrame %u: %dx%d, %zu bytes, cam=%ux%u  ",
                   mjpeg.frame_num, rgb.width, rgb.height,
                   mjpeg.data.size(), mjpeg.width, mjpeg.height);
            fflush(stdout);
        }

        // Print stats periodically
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats >= stats_interval) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats).count();
            float fps = (elapsed_ms > 0) ? (frames_received * 1000.0f / elapsed_ms) : 0;
            float kbps = (elapsed_ms > 0) ? (total_bytes * 8.0f / elapsed_ms) : 0;
            printf("\r[Stats] FPS: %.1f | Recv: %d | Drop: %d | Skip: %d | %.0f kbps | %.1f KB/f    \n",
                   fps, frames_received, frames_dropped, frames_skipped, kbps,
                   frames_received > 0 ? (total_bytes / 1024.0f / frames_received) : 0.0f);
            total_received += frames_received;
            total_dropped += frames_dropped;
            total_skipped += frames_skipped;
            frames_received = 0;
            frames_dropped = 0;
            frames_skipped = 0;
            total_bytes = 0;
            last_stats = now;
        }

        // No frame rate limiter — camera-side TakeSemaphore paces at 30fps.
        // Adding sleep here would only cause frame loss.
    }

    // --- Cleanup ---
    // Flush remaining window stats into cumulative totals
    total_received += frames_received;
    total_dropped += frames_dropped;
    total_skipped += frames_skipped;

    printf("\n=== SESSION SUMMARY ===\n");
    printf("  Received: %d frames\n", total_received);
    printf("  Dropped:  %d (decode failures)\n", total_dropped);
    printf("  Skipped:  %d (camera-produced but never received)\n", total_skipped);
    printf("  Unique data: %d frames\n", unique_frames);
    printf("  Duplicate data: %d frames (identical to previous)\n", duplicate_frames);
    printf("  Last cam frame#: %u\n", last_frame_num);
    if (last_frame_num > 0) {
        int expected = (int)last_frame_num;
        int actual = total_received + total_dropped;
        printf("  Camera produced: ~%d frames, bridge saw: %d (%.1f%%)\n",
               expected, actual, expected > 0 ? (actual * 100.0f / expected) : 0.0f);
    }
    printf("=======================\n");

    printf("\nStopping...\n");
    if (g_debug_log) { fclose(g_debug_log); g_debug_log = nullptr; }
    preview.shutdown();
    client.stop_webcam();
    vwebcam.shutdown();
#ifdef HAS_FFMPEG
    h264dec.shutdown();
#endif
    client.disconnect();
    printf("Done.\n");

    return 0;
}
