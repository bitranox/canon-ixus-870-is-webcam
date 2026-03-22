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
#include <map>
#include <climits>

#ifdef _WIN32
#include <windows.h>
#endif

// Global flag for graceful shutdown
static volatile bool g_running = true;

// Audio WAV writer — accumulates PCM samples during session, writes WAV on exit
static FILE* g_audio_wav = nullptr;
static uint32_t g_audio_bytes = 0;

static void audio_wav_open(const char* path) {
    g_audio_wav = fopen(path, "wb");
    if (!g_audio_wav) return;
    // Write WAV header placeholder (44 bytes) — updated on close
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    memcpy(hdr + 16, &fmt_size, 4);
    uint16_t pcm = 1;           memcpy(hdr + 20, &pcm, 2);        // PCM format
    uint16_t channels = 1;      memcpy(hdr + 22, &channels, 2);   // mono
    uint32_t sample_rate = 44100; memcpy(hdr + 24, &sample_rate, 4);
    uint32_t byte_rate = 88200; memcpy(hdr + 28, &byte_rate, 4);  // 44100*2
    uint16_t block_align = 2;   memcpy(hdr + 32, &block_align, 2);
    uint16_t bits = 16;         memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, g_audio_wav);
    g_audio_bytes = 0;
}

static void audio_wav_write(const uint8_t* data, size_t size) {
    if (!g_audio_wav || !data || size == 0) return;
    fwrite(data, 1, size, g_audio_wav);
    g_audio_bytes += (uint32_t)size;
}

static void audio_wav_close() {
    if (!g_audio_wav) return;
    // Update WAV header with final sizes
    uint32_t riff_size = 36 + g_audio_bytes;
    fseek(g_audio_wav, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, g_audio_wav);
    fseek(g_audio_wav, 40, SEEK_SET);
    fwrite(&g_audio_bytes, 4, 1, g_audio_wav);
    fclose(g_audio_wav);
    g_audio_wav = nullptr;
    fprintf(stderr, "Audio saved: %u bytes (%.1f seconds)\n",
            g_audio_bytes, g_audio_bytes / 88200.0);
}

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

struct UploadEntry {
    std::string local_path;
    std::string remote_path;
};

struct Options {
    int jpeg_quality = 50;
    int output_width = 1280;
    int output_height = 720;
    int target_fps = 30;
    bool flip_h = false;
    bool flip_v = false;
    bool no_webcam = false;
    bool no_preview = false;
    bool no_decode = false;
    bool verbose = false;
    bool debug = false;
    int timeout_sec = 0;
    std::string dump_dir;  // if non-empty, save raw H.264 frames to this directory
    std::vector<UploadEntry> uploads;
    std::vector<std::string> delete_files;   // remote files to delete
    std::vector<std::string> download_files; // pairs: remote, local
    std::string ls_path;     // directory to list
    bool reboot = false;
    std::string exec_script;  // Lua script to execute on camera
};

// AVCC frame validation
struct AvccInfo {
    bool valid;
    uint8_t nal_type;
    const char* nal_name;
    uint32_t avcc_len;
    const char* problem;  // null if OK
};

static AvccInfo check_avcc(const uint8_t* data, size_t size) {
    AvccInfo info = { false, 0, "?", 0, nullptr };
    if (size < 5) {
        info.problem = "too_short(<5)";
        return info;
    }
    info.avcc_len = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                    ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    bool forbidden = (data[4] & 0x80) != 0;
    info.nal_type = data[4] & 0x1F;
    switch (info.nal_type) {
        case 1:  info.nal_name = "P"; break;
        case 5:  info.nal_name = "IDR"; break;
        case 6:  info.nal_name = "SEI"; break;
        case 7:  info.nal_name = "SPS"; break;
        case 8:  info.nal_name = "PPS"; break;
        default: info.nal_name = "?"; break;
    }
    if (forbidden) {
        info.problem = "forbidden_bit";
    } else if (info.avcc_len == 0) {
        info.problem = "zero_len";
    } else if (info.avcc_len > size) {
        info.problem = "len_overflow";
    } else if (info.nal_type == 0 || info.nal_type > 12) {
        info.problem = "bad_nal_type";
    } else {
        info.valid = true;
    }
    return info;
}

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
        "  --no-decode         Skip H.264 decode (measure raw PTP throughput)\n"
        "  --verbose           Show per-frame statistics\n"
        "  --debug             Per-frame CSV debug logging to stderr\n"
        "  --dump-frames DIR   Save raw H.264 frames to DIR (for analysis)\n"
        "  --timeout N         Exit after N seconds (graceful shutdown)\n"
        "  --upload LOCAL REMOTE  Upload file to camera SD card, then exit\n"
        "                         (can be repeated; REMOTE uses A/ prefix for SD root)\n"
        "  --reboot              Reboot camera (use after --upload to reload firmware)\n"
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
        } else if (arg == "--no-decode") {
            opts.no_decode = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--timeout" && i + 1 < argc) {
            opts.timeout_sec = atoi(argv[++i]);
        } else if (arg == "--dump-frames" && i + 1 < argc) {
            opts.dump_dir = argv[++i];
        } else if (arg == "--reboot") {
            opts.reboot = true;
        } else if (arg == "--exec" && i + 1 < argc) {
            opts.exec_script = argv[++i];
        } else if (arg == "--upload" && i + 2 < argc) {
            UploadEntry e;
            e.local_path = argv[++i];
            e.remote_path = argv[++i];
            opts.uploads.push_back(e);
        } else if (arg == "--delete" && i + 1 < argc) {
            opts.delete_files.push_back(argv[++i]);
        } else if (arg == "--download" && i + 2 < argc) {
            opts.download_files.push_back(argv[++i]); // remote
            opts.download_files.push_back(argv[++i]); // local
        } else if (arg == "--ls" && i + 1 < argc) {
            opts.ls_path = argv[++i];
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
    printf("Output: %dx%d @ %d FPS\n\n",
           opts.output_width, opts.output_height, opts.target_fps);

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

    // --- Upload mode: send files to camera and exit ---
    if (!opts.uploads.empty()) {
        int ok = 0, fail = 0;
        for (const auto& u : opts.uploads) {
            // Read local file
            FILE* f = fopen(u.local_path.c_str(), "rb");
            if (!f) {
                fprintf(stderr, "ERROR: Cannot open local file: %s\n", u.local_path.c_str());
                fail++;
                continue;
            }
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> data(fsize);
            if (fsize > 0)
                fread(data.data(), 1, fsize, f);
            fclose(f);

            printf("Uploading %s (%ld bytes) -> %s ... ",
                   u.local_path.c_str(), fsize, u.remote_path.c_str());
            fflush(stdout);

            if (client.upload_file(u.remote_path, data)) {
                printf("OK\n");
                ok++;
            } else {
                printf("FAILED: %s\n", client.get_last_error().c_str());
                fail++;
            }
        }
        printf("\nUpload complete: %d OK, %d failed\n", ok, fail);
        if (!opts.reboot) {
            client.disconnect();
            return fail > 0 ? 1 : 0;
        }
        // Fall through to reboot
    }

    // --- Delete files on camera via Lua ---
    if (!opts.delete_files.empty()) {
        for (const auto& path : opts.delete_files) {
            std::string lua = "os.remove('" + path + "')";
            printf("Deleting %s ... ", path.c_str());
            if (client.execute_script(lua)) {
                Sleep(500);
                printf("OK\n");
            } else {
                printf("FAILED: %s\n", client.get_last_error().c_str());
            }
        }
        if (opts.uploads.empty() && !opts.reboot && opts.exec_script.empty()
            && opts.ls_path.empty() && opts.download_files.empty()) {
            client.disconnect();
            return 0;
        }
    }

    // --- Download files from camera via Lua ---
    for (size_t di = 0; di + 1 < opts.download_files.size(); di += 2) {
        const auto& remote = opts.download_files[di];
        const auto& local = opts.download_files[di + 1];
        // Use Lua to read file and send via write_usb_msg
        std::string lua =
            "f=io.open('" + remote + "','rb') "
            "if f then d=f:read('*a') f:close() write_usb_msg(d) "
            "else write_usb_msg('ERROR:not found') end";
        printf("Downloading %s -> %s ... ", remote.c_str(), local.c_str());
        if (client.execute_script(lua)) {
            Sleep(1000);
            std::string msg;
            if (client.read_script_msg(msg) && !msg.empty()) {
                if (msg.substr(0, 6) == "ERROR:") {
                    printf("FAILED: %s\n", msg.c_str());
                } else {
                    FILE* f = fopen(local.c_str(), "wb");
                    if (f) {
                        fwrite(msg.data(), 1, msg.size(), f);
                        fclose(f);
                        printf("OK (%zu bytes)\n", msg.size());
                    } else {
                        printf("FAILED: can't write local file\n");
                    }
                }
            } else {
                printf("FAILED: no response\n");
            }
        } else {
            printf("FAILED: %s\n", client.get_last_error().c_str());
        }
    }

    // --- List directory on camera via Lua ---
    if (!opts.ls_path.empty()) {
        std::string lua =
            "local s='' "
            "for f in os.idir('" + opts.ls_path + "') do "
            "  s=s..f..'\\n' "
            "end "
            "write_usb_msg(s)";
        if (client.execute_script(lua)) {
            Sleep(1000);
            std::string msg;
            if (client.read_script_msg(msg) && !msg.empty()) {
                printf("%s", msg.c_str());
            } else {
                printf("(empty or no response)\n");
            }
        } else {
            fprintf(stderr, "ERROR: ls failed: %s\n", client.get_last_error().c_str());
        }
        if (opts.uploads.empty() && !opts.reboot && opts.exec_script.empty()) {
            client.disconnect();
            return 0;
        }
    }

    // --- Exec mode: run Lua script on camera and print output ---
    if (!opts.exec_script.empty()) {
        if (!client.execute_script(opts.exec_script)) {
            fprintf(stderr, "ERROR: execute_script failed: %s\n", client.get_last_error().c_str());
            client.disconnect();
            return 1;
        }
        // Wait for script to finish, then read result from 0xFF800
        Sleep(2000);
        // Poll for any script messages
        for (int i = 0; i < 10; i++) {
            std::string msg;
            if (client.read_script_msg(msg)) {
                if (!msg.empty()) {
                    printf("%s\n", msg.c_str());
                    if (msg == "DONE") break;
                }
            }
            Sleep(100);
        }
        // Also read 4 bytes from 0xFF800 (script can poke results here)
        std::vector<uint8_t> mem;
        if (client.read_memory(0xFF800, 4, mem) && mem.size() >= 4) {
            uint32_t val = mem[0] | (mem[1]<<8) | (mem[2]<<16) | (mem[3]<<24);
            printf("[mem@0xFF800] = %u (0x%08X)\n", val, val);
        }
        if (!opts.reboot) {
            client.disconnect();
            return 0;
        }
    }

    if (opts.reboot) {
        printf("Rebooting camera...\n");
        client.execute_script("reboot()");
        // Camera will disconnect immediately — don't wait for response
        client.disconnect();
        printf("Reboot command sent. Wait ~10 seconds for camera to restart.\n");
        return 0;
    }

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
        pvc.width = opts.output_width;
        pvc.height = opts.output_height;
        pvc.title = "CHDK Webcam Preview";
        if (!preview.init(pvc)) {
            fprintf(stderr, "WARNING: Preview window failed to open.\n");
        }
    }

    // --- Clean up leftover 0-byte MOV files ---
    // Three-step approach: delete in playback mode → reboot → reconnect.
    // Reboot fully resets ISP state so no color shift on streaming.
    {
        client.execute_script(
            "local d='A/DCIM/100CANON' "
            "local t={} "
            "for f in os.idir(d) do "
            "  if string.match(f,'%.MOV$') then "
            "    local p=d..'/'..f "
            "    local s=os.stat(p) "
            "    if s and s.size<1024 then t[#t+1]=p end "
            "  end "
            "end "
            "for _,p in ipairs(t) do os.remove(p) end "
            "write_usb_msg(tostring(#t))");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string msg;
        client.read_script_msg(msg);
        int deleted = atoi(msg.c_str());

        // Always delete + reboot to ensure clean ISP state.
        // The reboot fixes both color shift AND recording failures
        // from previous session's finalization.
        printf("Cleaned MOV files, rebooting for clean state...\n");
        client.execute_script("reboot()");
        client.disconnect();
        printf("Waiting for camera...\n");
        for (int retry = 0; retry < 30; retry++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (client.connect()) break;
        }
        if (!client.is_connected()) {
            fprintf(stderr, "ERROR: Camera did not come back after reboot\n");
            return 1;
        }
        printf("Reconnected.\n");
    }

    // --- Start webcam streaming on camera ---
    printf("Starting webcam on camera (quality=%d)...\n", opts.jpeg_quality);
    bool start_ok = client.start_webcam(opts.jpeg_quality);
    if (!start_ok) {
        fprintf(stderr, "WARNING: start_webcam returned false: %s\n", client.get_last_error().c_str());
        fprintf(stderr, "Will try to get frames anyway (module may auto-load).\n");
    }
    if (opts.no_decode)
        printf("\n*** NO-DECODE MODE: measuring raw PTP throughput (no H.264 decode, no IDR re-injection) ***\n");
    if (opts.timeout_sec > 0)
        printf("\nStreaming for %d seconds. Press Ctrl+C to stop early.\n\n", opts.timeout_sec);
    else
        printf("\nStreaming. Press Ctrl+C to stop.\n\n");

    // Open audio WAV file for recording
    audio_wav_open("audio_capture.wav");

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

    // Timing: measure FPS from first to last frame (excludes startup)
    std::chrono::steady_clock::time_point first_frame_time;
    std::chrono::steady_clock::time_point last_frame_time;
    bool has_first_frame = false;

    // Debug mode tracking
    int dbg_ptp_calls = 0;           // total get_frame() calls
    int dbg_ptp_success = 0;         // calls that returned a frame
    int dbg_ptp_noframe = 0;         // calls that returned no frame
    int dbg_decode_attempts = 0;
    int dbg_decode_ok = 0;
    int dbg_decode_fail = 0;
    int dbg_nal_idr = 0;
    int dbg_nal_p = 0;
    int dbg_nal_sei = 0;
    int dbg_nal_other = 0;
    int dbg_avcc_valid = 0;
    int dbg_avcc_invalid = 0;
    int decode_streak = 0;
    int max_decode_streak = 0;
    int streak_start_frame = 0;
    int max_streak_start = 0;
    int max_streak_end = 0;
    int64_t dbg_total_frame_bytes = 0;
    int dbg_min_frame_size = INT_MAX;
    int dbg_max_frame_size = 0;
    double dbg_rtt_min_ms = 1e9;
    double dbg_rtt_max_ms = 0;
    double dbg_rtt_total_ms = 0;
    int dbg_rtt_count = 0;
    std::map<std::string, int> dbg_decode_errors;  // error string -> count

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

        // Handle zoom from preview window (+/- keys or mouse wheel)
        {
            int zoom_delta = preview.get_zoom_delta();
            if (zoom_delta != 0) {
                if (!client.zoom(zoom_delta)) {
                    fprintf(stderr, "Zoom failed: %s\n", client.get_last_error().c_str());
                }
            }
        }

        auto frame_start = std::chrono::steady_clock::now();

        // Get frame from camera
        ptp::MJPEGFrame mjpeg;
        if (!client.get_frame(mjpeg)) {
            frames_dropped++;
            if (opts.debug) {
                dbg_ptp_calls++;
                dbg_ptp_noframe++;
                auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                fprintf(stderr, "[DBG] t=%.3f DROP err=\"%s\"\n", t, client.get_last_error().c_str());
            }
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

        // Measure USB PTP round-trip time (frame_start → get_frame return)
        double rtt_ms = 0;
        {
            auto frame_end = std::chrono::steady_clock::now();
            rtt_ms = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
            if (opts.debug) {
                dbg_rtt_total_ms += rtt_ms;
                dbg_rtt_count++;
                if (rtt_ms < dbg_rtt_min_ms) dbg_rtt_min_ms = rtt_ms;
                if (rtt_ms > dbg_rtt_max_ms) dbg_rtt_max_ms = rtt_ms;
            }
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
            if (opts.debug) {
                auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                fprintf(stderr, "[DBG] t=%.3f GAP lost=%d cam#%u->cam#%u\n",
                        t, gap, last_frame_num, mjpeg.frame_num);
            } else {
                fprintf(stderr, "FRAME GAP: %d frames lost (cam #%u -> #%u)\n",
                        gap, last_frame_num, mjpeg.frame_num);
            }
        }
        last_frame_num = mjpeg.frame_num;

        // Dump raw frame to disk if --dump-frames is set
        if (!opts.dump_dir.empty() && mjpeg.format != ptp::FRAME_FMT_DEBUG) {
            char fname[512];
            snprintf(fname, sizeof(fname), "%s/frame_%06u_%zu.h264",
                     opts.dump_dir.c_str(), mjpeg.frame_num, mjpeg.data.size());
            FILE* f = fopen(fname, "wb");
            if (f) {
                fwrite(mjpeg.data.data(), 1, mjpeg.data.size(), f);
                fclose(f);
            }
        }

        // Write piggybacked audio to WAV file
        if (!mjpeg.audio_data.empty()) {
            audio_wav_write(mjpeg.audio_data.data(), mjpeg.audio_data.size());
        }

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

        // Handle multi-frame H.264 batch: unpack into individual frames.
        // Format: [u16 count][u32 sz][data]... — camera batches 2-3 frames per PTP call.
        // We expand them into separate mjpeg entries and process each through the
        // normal H.264 path below by re-entering the loop body via a queue.
        if (mjpeg.format == ptp::FRAME_FMT_H264_MULTI) {
            if (mjpeg.data.size() < 2) continue;
            uint16_t mf_count = mjpeg.data[0] | (mjpeg.data[1] << 8);
            size_t mf_pos = 2;
            if (opts.debug) {
                auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                fprintf(stderr, "[DBG] t=%.3f MULTI batch=%d total=%zu bytes\n",
                        t, mf_count, mjpeg.data.size());
            }
            for (int mf_i = 0; mf_i < mf_count && mf_pos + 4 <= mjpeg.data.size(); mf_i++) {
                uint32_t mf_sz = mjpeg.data[mf_pos] | (mjpeg.data[mf_pos+1] << 8)
                               | (mjpeg.data[mf_pos+2] << 16) | (mjpeg.data[mf_pos+3] << 24);
                mf_pos += 4;
                if (mf_sz == 0 || mf_pos + mf_sz > mjpeg.data.size()) break;

                // Create individual H.264 frame and decode it
                ptp::MJPEGFrame sub_frame;
                sub_frame.data.assign(mjpeg.data.begin() + mf_pos, mjpeg.data.begin() + mf_pos + mf_sz);
                sub_frame.width = mjpeg.width;
                sub_frame.height = mjpeg.height;
                sub_frame.frame_num = mjpeg.frame_num;
                sub_frame.format = ptp::FRAME_FMT_H264;
                mf_pos += mf_sz;

                // Debug stats
                if (opts.debug) {
                    dbg_ptp_calls++;
                    dbg_ptp_success++;
                    dbg_decode_attempts++;
                    int ssz = static_cast<int>(sub_frame.data.size());
                    dbg_total_frame_bytes += ssz;
                    if (ssz < dbg_min_frame_size) dbg_min_frame_size = ssz;
                    if (ssz > dbg_max_frame_size) dbg_max_frame_size = ssz;

                    AvccInfo avcc = check_avcc(sub_frame.data.data(), sub_frame.data.size());
                    if (avcc.valid) dbg_avcc_valid++; else dbg_avcc_invalid++;
                    switch (avcc.nal_type) {
                        case 5: dbg_nal_idr++; break;
                        case 1: dbg_nal_p++; break;
                        case 6: dbg_nal_sei++; break;
                        default: dbg_nal_other++; break;
                    }
                }

                // Track uniqueness
                unique_frames++;
                if (unique_frames <= 10) {
                    fprintf(stderr, "UNIQUE #%d (cam#%u[%d/%d], %zu bytes): ",
                            unique_frames, sub_frame.frame_num, mf_i+1, mf_count, sub_frame.data.size());
                    for (size_t bi = 0; bi < 16 && bi < sub_frame.data.size(); bi++)
                        fprintf(stderr, "%02X ", sub_frame.data[bi]);
                    if (sub_frame.data.size() >= 5)
                        fprintf(stderr, " NAL=0x%02X (type %d)", sub_frame.data[4], sub_frame.data[4] & 0x1F);
                    fprintf(stderr, "\n");
                }
                if (sub_frame.data.size() >= 5 && (sub_frame.data[4] & 0x1F) == 5) {
                    static int idr_count = 0;
                    idr_count++;
                    fprintf(stderr, "IDR #%d (cam#%u[%d/%d], %zu bytes)\n",
                            idr_count, sub_frame.frame_num, mf_i+1, mf_count, sub_frame.data.size());
                }

                // Decode
#ifdef HAS_FFMPEG
                webcam::RGBFrame sub_rgb;
                bool sub_ok = h264dec.decode(sub_frame.data.data(), sub_frame.data.size(), sub_rgb);
                if (!sub_ok) {
                    if (opts.debug) {
                        dbg_decode_fail++;
                        dbg_decode_errors[h264dec.get_last_error()]++;
                        decode_streak = 0;
                        auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                        AvccInfo avcc = check_avcc(sub_frame.data.data(), sub_frame.data.size());
                        fprintf(stderr, "[DBG] t=%.3f RECV cam#%u[%d/%d] sz=%zu nal=0x%02X(%s) avcc=%s dec=FAIL err=\"%s\"\n",
                                t, sub_frame.frame_num, mf_i+1, mf_count, sub_frame.data.size(),
                                sub_frame.data.size() >= 5 ? sub_frame.data[4] : 0,
                                avcc.nal_name, avcc.valid ? "OK" : avcc.problem,
                                h264dec.get_last_error().c_str());
                    }
                    frames_dropped++;
                    continue;
                }
                if (opts.debug) {
                    dbg_decode_ok++;
                    decode_streak++;
                    if (decode_streak == 1) streak_start_frame = sub_frame.frame_num;
                    if (decode_streak > max_decode_streak) {
                        max_decode_streak = decode_streak;
                        max_streak_start = streak_start_frame;
                        max_streak_end = sub_frame.frame_num;
                    }
                    auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                    AvccInfo avcc = check_avcc(sub_frame.data.data(), sub_frame.data.size());
                    fprintf(stderr, "[DBG] t=%.3f RECV cam#%u[%d/%d] sz=%zu nal=0x%02X(%s) avcc=%s dec=OK streak=%d\n",
                            t, sub_frame.frame_num, mf_i+1, mf_count, sub_frame.data.size(),
                            sub_frame.data.size() >= 5 ? sub_frame.data[4] : 0,
                            avcc.nal_name, avcc.valid ? "OK" : avcc.problem,
                            decode_streak);
                }

                // Send decoded multi-frame to preview/webcam
                if (vwebcam.is_active()) {
                    vwebcam.send_frame(sub_rgb.data.data(), sub_rgb.width, sub_rgb.height, sub_rgb.stride);
                }
                if (preview.is_open()) {
                    preview.show_frame(sub_rgb.data.data(), sub_rgb.width, sub_rgb.height, sub_rgb.stride);
                }

                frames_received++;
                total_bytes += static_cast<int>(sub_frame.data.size());
                if (!has_first_frame) { first_frame_time = std::chrono::steady_clock::now(); has_first_frame = true; }
                last_frame_time = std::chrono::steady_clock::now();
#endif
            }
            // Stats for multi-frame (count the PTP call itself)
            auto now = std::chrono::steady_clock::now();
            if (now - last_stats >= stats_interval) {
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats).count();
                float fps = (elapsed_ms > 0) ? (frames_received * 1000.0f / elapsed_ms) : 0;
                float kbps = (elapsed_ms > 0) ? (total_bytes * 8.0f / elapsed_ms) : 0;
                float avg_sz = (frames_received > 0) ? (total_bytes / (float)frames_received / 1024.0f) : 0;
                printf("\r[Stats] FPS: %.1f | Recv: %d | Drop: %d | Skip: %d | %.0f kbps | %.1f KB/f    \n",
                       fps, frames_received, frames_dropped, frames_skipped, kbps, avg_sz);
                total_received += frames_received;
                total_dropped += frames_dropped;
                total_skipped += frames_skipped;
                frames_received = 0;
                frames_dropped = 0;
                frames_skipped = 0;
                total_bytes = 0;
                last_stats = now;
            }
            continue;
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
                // Log every IDR frame (NAL type 5) to track GOP sync points
                if (mjpeg.data.size() >= 5 && (mjpeg.data[4] & 0x1F) == 5) {
                    static int idr_count = 0;
                    idr_count++;
                    fprintf(stderr, "IDR #%d (cam#%u, %zu bytes)\n",
                            idr_count, mjpeg.frame_num, mjpeg.data.size());
                }
            }
            prev_data_hash = h;
        }

        // Debug mode: track per-frame stats (common to both decode and no-decode paths)
        if (opts.debug) {
            dbg_ptp_calls++;
            dbg_ptp_success++;
            int sz = static_cast<int>(mjpeg.data.size());
            dbg_total_frame_bytes += sz;
            if (sz < dbg_min_frame_size) dbg_min_frame_size = sz;
            if (sz > dbg_max_frame_size) dbg_max_frame_size = sz;

            if (mjpeg.format == ptp::FRAME_FMT_H264) {
                AvccInfo avcc = check_avcc(mjpeg.data.data(), mjpeg.data.size());
                if (avcc.valid) dbg_avcc_valid++; else dbg_avcc_invalid++;
                switch (avcc.nal_type) {
                    case 5: dbg_nal_idr++; break;
                    case 1: dbg_nal_p++; break;
                    case 6: dbg_nal_sei++; break;
                    default: dbg_nal_other++; break;
                }
            }
        }

        // --no-decode: skip all decode/display, just count raw PTP throughput
        if (opts.no_decode) {
            frames_received++;
            total_bytes += static_cast<int>(mjpeg.data.size());
            {
                auto frame_time = std::chrono::steady_clock::now();
                if (!has_first_frame) { first_frame_time = frame_time; has_first_frame = true; }
                last_frame_time = frame_time;
            }
            if (frames_received == 1) {
                const char* fmt_name = "JPEG";
                if (mjpeg.format == ptp::FRAME_FMT_UYVY) fmt_name = "UYVY";
                else if (mjpeg.format == ptp::FRAME_FMT_H264) fmt_name = "H.264";
                printf("First frame: %ux%u, %zu bytes, format=%s (NO DECODE)\n",
                       mjpeg.width, mjpeg.height, mjpeg.data.size(), fmt_name);
            }
            // AVCC header validity check (no decode, just inspect)
            if (mjpeg.format == ptp::FRAME_FMT_H264 && mjpeg.data.size() >= 5) {
                uint8_t nal_type = mjpeg.data[4] & 0x1F;
                bool forbidden = (mjpeg.data[4] & 0x80) != 0;
                uint32_t avcc_len = ((uint32_t)mjpeg.data[0] << 24) |
                                    ((uint32_t)mjpeg.data[1] << 16) |
                                    ((uint32_t)mjpeg.data[2] << 8) |
                                    (uint32_t)mjpeg.data[3];
                bool valid_avcc = !forbidden && (nal_type == 1 || nal_type == 5 || nal_type == 6)
                                  && avcc_len > 0 && avcc_len <= mjpeg.data.size();
                static int valid_count = 0, invalid_count = 0;
                if (valid_avcc) valid_count++; else invalid_count++;
                if (!opts.debug && (frames_received <= 10 || frames_received % 50 == 0)) {
                    fprintf(stderr, "  [no-decode #%d] %zu bytes, NAL=%d, avcc_len=%u, %s  (valid:%d invalid:%d)\n",
                            frames_received, mjpeg.data.size(), nal_type, avcc_len,
                            valid_avcc ? "OK" : "BAD", valid_count, invalid_count);
                }
            }

            // Debug per-frame line (no-decode path)
            if (opts.debug && mjpeg.format == ptp::FRAME_FMT_H264) {
                auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                AvccInfo avcc = check_avcc(mjpeg.data.data(), mjpeg.data.size());
                fprintf(stderr, "[DBG] t=%.3f RECV cam#%u sz=%zu nal=0x%02X(%s) avcc=%s rtt=%.1fms\n",
                        t, mjpeg.frame_num, mjpeg.data.size(),
                        mjpeg.data.size() >= 5 ? mjpeg.data[4] : 0,
                        avcc.nal_name,
                        avcc.valid ? "OK" : avcc.problem,
                        rtt_ms);
            }

            // Print stats periodically
            auto now = std::chrono::steady_clock::now();
            if (now - last_stats >= stats_interval) {
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats).count();
                float fps = (elapsed_ms > 0) ? (frames_received * 1000.0f / elapsed_ms) : 0;
                float kbps = (elapsed_ms > 0) ? (total_bytes * 8.0f / elapsed_ms) : 0;
                printf("\r[NO-DECODE] FPS: %.1f | Recv: %d | Drop: %d | Skip: %d | %.0f kbps | Uniq: %d Dup: %d    \n",
                       fps, frames_received, frames_dropped, frames_skipped, kbps,
                       unique_frames, duplicate_frames);
                total_received += frames_received;
                total_dropped += frames_dropped;
                total_skipped += frames_skipped;
                frames_received = 0;
                frames_dropped = 0;
                frames_skipped = 0;
                total_bytes = 0;
                last_stats = now;
            }
            continue;
        }

        // Decode frame (JPEG, raw UYVY, or H.264)
        webcam::RGBFrame rgb;
        bool decode_ok;
        std::string decode_err;
        if (mjpeg.format == ptp::FRAME_FMT_H264) {
#ifdef HAS_FFMPEG
            if (opts.debug) dbg_decode_attempts++;
            decode_ok = h264dec.decode(mjpeg.data.data(), mjpeg.data.size(), rgb);
            if (!decode_ok) {
                decode_err = h264dec.get_last_error();
                if (opts.debug) {
                    dbg_decode_fail++;
                    dbg_decode_errors[decode_err]++;
                    decode_streak = 0;
                    auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                    AvccInfo avcc = check_avcc(mjpeg.data.data(), mjpeg.data.size());
                    fprintf(stderr, "[DBG] t=%.3f RECV cam#%u sz=%zu nal=0x%02X(%s) avcc=%s dec=FAIL err=\"%s\" streak=0 rtt=%.1fms\n",
                            t, mjpeg.frame_num, mjpeg.data.size(),
                            mjpeg.data.size() >= 5 ? mjpeg.data[4] : 0,
                            avcc.nal_name, avcc.valid ? "OK" : avcc.problem,
                            decode_err.c_str(), rtt_ms);
                }
                frames_dropped++;
                continue;
            }
            if (opts.debug) {
                dbg_decode_ok++;
                decode_streak++;
                if (decode_streak == 1) streak_start_frame = mjpeg.frame_num;
                if (decode_streak > max_decode_streak) {
                    max_decode_streak = decode_streak;
                    max_streak_start = streak_start_frame;
                    max_streak_end = mjpeg.frame_num;
                }
                auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                AvccInfo avcc = check_avcc(mjpeg.data.data(), mjpeg.data.size());
                fprintf(stderr, "[DBG] t=%.3f RECV cam#%u sz=%zu nal=0x%02X(%s) avcc=%s dec=OK streak=%d rtt=%.1fms\n",
                        t, mjpeg.frame_num, mjpeg.data.size(),
                        mjpeg.data.size() >= 5 ? mjpeg.data[4] : 0,
                        avcc.nal_name, avcc.valid ? "OK" : avcc.problem,
                        decode_streak, rtt_ms);
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
            if (opts.debug) dbg_decode_attempts++;
            decode_ok = processor.process_uyvy(mjpeg.data.data(), static_cast<int>(mjpeg.data.size()),
                                               mjpeg.width, mjpeg.height, rgb);
        } else {
            if (opts.debug) dbg_decode_attempts++;
            decode_ok = processor.process(mjpeg.data.data(), static_cast<int>(mjpeg.data.size()), rgb);
        }
        if (!decode_ok) {
            decode_err = processor.get_last_error();
            if (opts.verbose) {
                fprintf(stderr, "Decode error: %s\n", decode_err.c_str());
            }
            if (opts.debug) {
                dbg_decode_fail++;
                dbg_decode_errors[decode_err]++;
                decode_streak = 0;
                auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                fprintf(stderr, "[DBG] t=%.3f RECV cam#%u sz=%zu dec=FAIL err=\"%s\" streak=0 rtt=%.1fms\n",
                        t, mjpeg.frame_num, mjpeg.data.size(), decode_err.c_str(), rtt_ms);
            }
            frames_dropped++;
            continue;
        }
        if (opts.debug && mjpeg.format != ptp::FRAME_FMT_H264) {
            dbg_decode_ok++;
            decode_streak++;
            if (decode_streak == 1) streak_start_frame = mjpeg.frame_num;
            if (decode_streak > max_decode_streak) {
                max_decode_streak = decode_streak;
                max_streak_start = streak_start_frame;
                max_streak_end = mjpeg.frame_num;
            }
            auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
            fprintf(stderr, "[DBG] t=%.3f RECV cam#%u sz=%zu dec=OK streak=%d rtt=%.1fms\n",
                    t, mjpeg.frame_num, mjpeg.data.size(), decode_streak, rtt_ms);
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
        {
            auto frame_time = std::chrono::steady_clock::now();
            if (!has_first_frame) { first_frame_time = frame_time; has_first_frame = true; }
            last_frame_time = frame_time;
        }

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

        // No frame rate limiter — bridge polls as fast as PTP round-trip allows.
        // Camera-side msleep(10) provides the only pacing.
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
    if (has_first_frame) {
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            last_frame_time - first_frame_time).count();
        if (duration_ms > 0) {
            float recv_fps = (total_received - 1) * 1000.0f / duration_ms;
            float total_fps = (total_received + total_dropped - 1) * 1000.0f / duration_ms;
            printf("  Duration: %.1f seconds (first frame to last)\n", duration_ms / 1000.0f);
            printf("  Decoded FPS: %.1f\n", recv_fps);
            printf("  Total FPS (incl. drops): %.1f\n", total_fps);
        }
    }
    if (last_frame_num > 0) {
        printf("  Camera produced: ~%u frames\n", last_frame_num);
    }
    printf("=======================\n");

    if (opts.debug) {
        auto usb = client.get_usb_stats();
        fprintf(stderr, "\n=== DEBUG SUMMARY ===\n");
        fprintf(stderr, "  PTP calls:    %d (%d success, %d no-frame)\n",
                dbg_ptp_calls, dbg_ptp_success, dbg_ptp_noframe);
        if (dbg_decode_attempts > 0) {
            fprintf(stderr, "  Decode:       %d attempts, %d OK (%.1f%%), %d FAIL\n",
                    dbg_decode_attempts, dbg_decode_ok,
                    dbg_decode_attempts > 0 ? (100.0 * dbg_decode_ok / dbg_decode_attempts) : 0.0,
                    dbg_decode_fail);
            if (!dbg_decode_errors.empty()) {
                fprintf(stderr, "  Decode errors:");
                for (auto& kv : dbg_decode_errors) {
                    fprintf(stderr, " \"%s\": %d", kv.first.c_str(), kv.second);
                    if (&kv != &*dbg_decode_errors.rbegin())
                        fprintf(stderr, ",");
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "  NAL types:    IDR: %d, P-frame: %d, SEI: %d, other: %d\n",
                dbg_nal_idr, dbg_nal_p, dbg_nal_sei, dbg_nal_other);
        int total_avcc = dbg_avcc_valid + dbg_avcc_invalid;
        if (total_avcc > 0) {
            fprintf(stderr, "  AVCC valid:   %d/%d (%.1f%%)\n",
                    dbg_avcc_valid, total_avcc,
                    100.0 * dbg_avcc_valid / total_avcc);
        }
        if (max_decode_streak > 0) {
            fprintf(stderr, "  Max streak:   %d (cam#%d-cam#%d)\n",
                    max_decode_streak, max_streak_start, max_streak_end);
        }
        if (dbg_rtt_count > 0) {
            fprintf(stderr, "  PTP RTT:      min=%.1fms avg=%.1fms max=%.1fms (n=%d)\n",
                    dbg_rtt_min_ms, dbg_rtt_total_ms / dbg_rtt_count, dbg_rtt_max_ms, dbg_rtt_count);
        }
        fprintf(stderr, "  USB errors:   send=%d recv=%d timeout=%d io=%d\n",
                usb.send_errors, usb.recv_errors, usb.timeout_errors, usb.io_errors);
        if (dbg_ptp_success > 0) {
            fprintf(stderr, "  Frame sizes:  min=%d max=%d avg=%lld\n",
                    dbg_min_frame_size == INT_MAX ? 0 : dbg_min_frame_size,
                    dbg_max_frame_size,
                    dbg_ptp_success > 0 ? dbg_total_frame_bytes / dbg_ptp_success : 0LL);
        }
        fprintf(stderr, "=====================\n");
    }

    printf("\nStopping...\n");
    audio_wav_close();
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
