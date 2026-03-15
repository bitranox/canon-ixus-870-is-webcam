#include "frame_processor.h"
#include <cstring>
#include <string>
#include <algorithm>

#ifdef HAS_TURBOJPEG
#include <turbojpeg.h>
#else
// Minimal JPEG decoder using standard jpeglib
extern "C" {
#include <jpeglib.h>
}
#endif

namespace webcam {

struct FrameProcessor::Impl {
    ProcessorConfig config;
    std::string last_error;

#ifdef HAS_TURBOJPEG
    tjhandle tj_handle = nullptr;
#endif

    // Intermediate buffer for decoded image (before scaling)
    std::vector<uint8_t> decoded_buf;
    int decoded_width = 0;
    int decoded_height = 0;

    Impl() {
#ifdef HAS_TURBOJPEG
        tj_handle = tjInitDecompress();
#endif
    }

    ~Impl() {
#ifdef HAS_TURBOJPEG
        if (tj_handle) {
            tjDestroy(tj_handle);
        }
#endif
    }

    bool decode_jpeg(const uint8_t* jpeg_data, int jpeg_size) {
#ifdef HAS_TURBOJPEG
        int width, height, subsamp, colorspace;
        if (tjDecompressHeader3(tj_handle, jpeg_data, jpeg_size,
                                &width, &height, &subsamp, &colorspace) < 0) {
            last_error = "JPEG header decode failed: " + std::string(tjGetErrorStr2(tj_handle));
            return false;
        }

        decoded_width = width;
        decoded_height = height;
        int stride = width * 3;
        decoded_buf.resize(stride * height);

        if (tjDecompress2(tj_handle, jpeg_data, jpeg_size,
                          decoded_buf.data(), width, stride, height,
                          TJPF_RGB, TJFLAG_FASTDCT) < 0) {
            const char* err = tjGetErrorStr2(tj_handle);
            // "extraneous bytes" is a non-fatal warning — image decoded OK.
            // This happens when the JPEG encoder writes slightly more MCU
            // data than the SOF0 header declares.
            if (err && strstr(err, "extraneous bytes")) {
                return true;
            }
            last_error = "JPEG decode failed: " + std::string(err ? err : "unknown");
            return false;
        }
        return true;
#else
        // Use standard libjpeg
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            last_error = "JPEG header read failed";
            jpeg_destroy_decompress(&cinfo);
            return false;
        }

        cinfo.out_color_space = JCS_RGB;
        cinfo.dct_method = JDCT_IFAST;

        jpeg_start_decompress(&cinfo);

        decoded_width = cinfo.output_width;
        decoded_height = cinfo.output_height;
        int stride = decoded_width * 3;
        decoded_buf.resize(stride * decoded_height);

        while (cinfo.output_scanline < cinfo.output_height) {
            uint8_t* row = decoded_buf.data() + cinfo.output_scanline * stride;
            jpeg_read_scanlines(&cinfo, &row, 1);
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return true;
#endif
    }

    void flip_frame(uint8_t* data, int width, int height, int stride, bool h, bool v) {
        if (h) {
            for (int y = 0; y < height; y++) {
                uint8_t* row = data + y * stride;
                for (int x = 0; x < width / 2; x++) {
                    int x2 = width - 1 - x;
                    std::swap(row[x * 3 + 0], row[x2 * 3 + 0]);
                    std::swap(row[x * 3 + 1], row[x2 * 3 + 1]);
                    std::swap(row[x * 3 + 2], row[x2 * 3 + 2]);
                }
            }
        }
        if (v) {
            std::vector<uint8_t> tmp(stride);
            for (int y = 0; y < height / 2; y++) {
                uint8_t* row1 = data + y * stride;
                uint8_t* row2 = data + (height - 1 - y) * stride;
                memcpy(tmp.data(), row1, stride);
                memcpy(row1, row2, stride);
                memcpy(row2, tmp.data(), stride);
            }
        }
    }
};

FrameProcessor::FrameProcessor() : impl_(std::make_unique<Impl>()) {}
FrameProcessor::~FrameProcessor() = default;

void FrameProcessor::configure(const ProcessorConfig& config) {
    impl_->config = config;
}

bool FrameProcessor::process_uyvy(const uint8_t* uyvy_data, int data_size, int width, int height, RGBFrame& rgb_out) {
    int expected = width * height * 2;
    if (!uyvy_data || data_size < expected || width <= 0 || height <= 0) {
        impl_->last_error = "Invalid UYVY data";
        return false;
    }

    int out_stride = width * 3;
    rgb_out.width = width;
    rgb_out.height = height;
    rgb_out.stride = out_stride;
    rgb_out.data.resize(out_stride * height);

    // Convert UYVY (U Y0 V Y1) to RGB using fixed-point BT.601.
    // Each 4-byte UYVY macro-pixel produces 2 RGB pixels.
    //
    // IMPORTANT: Digic IV stores chroma as SIGNED bytes centered at 0,
    // not unsigned centered at 128. The U and V bytes must be interpreted
    // as signed char (-128..+127), NOT unsigned with 128 subtracted.
    for (int y = 0; y < height; y++) {
        const uint8_t* src = uyvy_data + y * width * 2;
        uint8_t* dst = rgb_out.data.data() + y * out_stride;

        for (int x = 0; x < width; x += 2) {
            int u  = static_cast<int>(static_cast<int8_t>(src[0]));  // signed, centered at 0
            int y0 = static_cast<int>(src[1]);
            int v  = static_cast<int>(static_cast<int8_t>(src[2]));  // signed, centered at 0
            int y1 = static_cast<int>(src[3]);
            src += 4;

            // Fixed-point BT.601: R = Y + 1.402*V, G = Y - 0.344*U - 0.714*V, B = Y + 1.772*U
            int r0 = y0 + ((359 * v) >> 8);
            int g0 = y0 - ((88 * u + 183 * v) >> 8);
            int b0 = y0 + ((454 * u) >> 8);

            int r1 = y1 + ((359 * v) >> 8);
            int g1 = y1 - ((88 * u + 183 * v) >> 8);
            int b1 = y1 + ((454 * u) >> 8);

            dst[0] = static_cast<uint8_t>(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
            dst[1] = static_cast<uint8_t>(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
            dst[2] = static_cast<uint8_t>(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));
            dst[3] = static_cast<uint8_t>(r1 < 0 ? 0 : (r1 > 255 ? 255 : r1));
            dst[4] = static_cast<uint8_t>(g1 < 0 ? 0 : (g1 > 255 ? 255 : g1));
            dst[5] = static_cast<uint8_t>(b1 < 0 ? 0 : (b1 > 255 ? 255 : b1));
            dst += 6;
        }
    }

    // Apply flips if configured
    if (impl_->config.flip_horizontal || impl_->config.flip_vertical) {
        impl_->flip_frame(rgb_out.data.data(), rgb_out.width, rgb_out.height,
                          rgb_out.stride, impl_->config.flip_horizontal,
                          impl_->config.flip_vertical);
    }

    return true;
}

bool FrameProcessor::process(const uint8_t* jpeg_data, int jpeg_size, RGBFrame& rgb_out) {
    if (!jpeg_data || jpeg_size <= 0) {
        impl_->last_error = "Invalid JPEG data";
        return false;
    }

    // Decode JPEG
    if (!impl_->decode_jpeg(jpeg_data, jpeg_size)) {
        return false;
    }

    // Pass through at native decoded resolution (upscaling handled by H.264 sws_scale path)
    int out_w = impl_->decoded_width;
    int out_h = impl_->decoded_height;
    int out_stride = out_w * 3;

    rgb_out.data = impl_->decoded_buf;
    rgb_out.width = out_w;
    rgb_out.height = out_h;
    rgb_out.stride = out_stride;

    // Apply flips if configured
    if (impl_->config.flip_horizontal || impl_->config.flip_vertical) {
        impl_->flip_frame(rgb_out.data.data(), rgb_out.width, rgb_out.height,
                          rgb_out.stride, impl_->config.flip_horizontal,
                          impl_->config.flip_vertical);
    }

    return true;
}

std::string FrameProcessor::get_last_error() const {
    return impl_->last_error;
}

} // namespace webcam
