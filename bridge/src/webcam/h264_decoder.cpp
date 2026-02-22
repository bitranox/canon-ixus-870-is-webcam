#include "h264_decoder.h"
#include "frame_processor.h"  // for RGBFrame

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstring>

namespace webcam {

struct H264Decoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;

    int output_width = 0;
    int output_height = 0;
    int last_decoded_width = 0;
    int last_decoded_height = 0;

    std::string last_error;
    std::vector<uint8_t> annex_b_buf;  // reusable buffer for AVCC->Annex B conversion
    std::vector<uint8_t> stored_idr;   // first IDR frame data for re-injection on decode failure
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}

H264Decoder::~H264Decoder() {
    shutdown();
}

bool H264Decoder::init(int output_width, int output_height) {
    impl_->output_width = output_width;
    impl_->output_height = output_height;

    impl_->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!impl_->codec) {
        impl_->last_error = "H.264 decoder not found in FFmpeg";
        return false;
    }

    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    if (!impl_->ctx) {
        impl_->last_error = "Failed to allocate codec context";
        return false;
    }

    // Allow partial/corrupt frames, skip loop filter for speed
    impl_->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    impl_->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    impl_->ctx->skip_loop_filter = AVDISCARD_ALL;
    impl_->ctx->thread_count = 1;  // single-threaded for low latency

    // Canon IXUS 870 IS H.264 SPS+PPS (640x480 Baseline Level 3.1)
    // Extracted from avcC atom of camera's MOV output.
    // SPS: 67 42 E0 1F DA 02 80 F6 9B 80 80 83 01
    // PPS: 68 CE 3C 80
    // Build avcC configuration record for FFmpeg extradata.
    static const uint8_t sps[] = {0x67,0x42,0xE0,0x1F,0xDA,0x02,0x80,0xF6,0x9B,0x80,0x80,0x83,0x01};
    static const uint8_t pps[] = {0x68,0xCE,0x3C,0x80};
    // avcC record: version(1) + profile(1) + compat(1) + level(1) + nal_size_len(1)
    //            + num_sps(1) + sps_len(2) + sps_data + num_pps(1) + pps_len(2) + pps_data
    const int avcc_size = 6 + 2 + (int)sizeof(sps) + 1 + 2 + (int)sizeof(pps);
    impl_->ctx->extradata = (uint8_t*)av_mallocz(avcc_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (impl_->ctx->extradata) {
        uint8_t* p = impl_->ctx->extradata;
        p[0] = 1;            // version
        p[1] = sps[0+1];     // profile (0x42 = Baseline)
        p[2] = sps[0+2];     // compat flags
        p[3] = sps[0+3];     // level (0x1F = 31)
        p[4] = 0xFF;         // 6 bits reserved (111111) + 2 bits nal_size_len-1 (11 = 4 bytes)
        p[5] = 0xE1;         // 3 bits reserved (111) + 5 bits num_sps (00001 = 1)
        p[6] = 0; p[7] = (uint8_t)sizeof(sps);  // SPS length (big-endian)
        memcpy(p + 8, sps, sizeof(sps));
        p[8 + sizeof(sps)] = 1;  // num_pps
        p[9 + sizeof(sps)] = 0; p[10 + sizeof(sps)] = (uint8_t)sizeof(pps);
        memcpy(p + 11 + sizeof(sps), pps, sizeof(pps));
        impl_->ctx->extradata_size = avcc_size;
        fprintf(stderr, "H.264 decoder: loaded Canon IXUS 870 SPS+PPS (avcC %d bytes)\n", avcc_size);
    }

    if (avcodec_open2(impl_->ctx, impl_->codec, nullptr) < 0) {
        impl_->last_error = "Failed to open H.264 decoder";
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) {
        impl_->last_error = "Failed to allocate frame/packet";
        return false;
    }

    return true;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, RGBFrame& rgb_out) {
    if (!impl_->ctx) {
        impl_->last_error = "Decoder not initialized";
        return false;
    }

    // Store first IDR frame for later re-injection on decode failure.
    // AVCC format: first NAL type is at byte 4 (after 4-byte length prefix).
    if (impl_->stored_idr.empty() && size >= 5 && (data[4] & 0x1F) == 5) {
        impl_->stored_idr.assign(data, data + size);
        fprintf(stderr, "H.264 decoder: stored IDR frame (%zu bytes) for recovery\n", size);
    }

    // Sanitize AVCC data before feeding to FFmpeg.
    // The camera's spy buffer sometimes produces frames where AVCC NAL length
    // fields don't exactly match the packet size (ring buffer race conditions).
    // Fix: clamp each NAL length to not exceed remaining packet data.
    // Also handles Annex B input by converting start codes to AVCC lengths.
    const uint8_t* feed_data = data;
    size_t feed_size = size;

    if (size >= 5) {
        impl_->annex_b_buf.clear();
        impl_->annex_b_buf.reserve(size + 16);

        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            // Annex B format — convert to AVCC (replace start codes with lengths)
            size_t pos = 4;  // skip first start code
            size_t nal_start = pos;
            while (pos <= size) {
                bool at_end = (pos == size);
                bool at_sc = false;
                if (!at_end && pos + 3 <= size &&
                    data[pos] == 0 && data[pos+1] == 0 &&
                    (data[pos+2] == 1 || (data[pos+2] == 0 && pos+3 < size && data[pos+3] == 1))) {
                    at_sc = true;
                }
                if (at_end || at_sc) {
                    uint32_t nal_len = (uint32_t)(pos - nal_start);
                    impl_->annex_b_buf.push_back((uint8_t)(nal_len >> 24));
                    impl_->annex_b_buf.push_back((uint8_t)(nal_len >> 16));
                    impl_->annex_b_buf.push_back((uint8_t)(nal_len >> 8));
                    impl_->annex_b_buf.push_back((uint8_t)nal_len);
                    impl_->annex_b_buf.insert(impl_->annex_b_buf.end(),
                                              data + nal_start, data + pos);
                    if (at_sc) {
                        pos += (data[pos+2] == 1) ? 3 : 4;
                        nal_start = pos;
                    }
                }
                if (!at_end && !at_sc) pos++;
            }
        } else {
            // AVCC format — sanitize NAL lengths to prevent FFmpeg errors.
            // Walk through [4-byte length][NAL data] units and clamp any
            // length that exceeds the remaining packet data.
            size_t pos = 0;
            while (pos + 4 < size) {
                uint32_t nal_len = ((uint32_t)data[pos] << 24) |
                                   ((uint32_t)data[pos+1] << 16) |
                                   ((uint32_t)data[pos+2] << 8) |
                                   (uint32_t)data[pos+3];

                uint32_t remaining = (uint32_t)(size - pos - 4);

                // Clamp NAL length to available data
                if (nal_len > remaining)
                    nal_len = remaining;

                // Skip obviously invalid NALs (empty or no valid header)
                if (nal_len < 1) break;

                // Write sanitized length
                impl_->annex_b_buf.push_back((uint8_t)(nal_len >> 24));
                impl_->annex_b_buf.push_back((uint8_t)(nal_len >> 16));
                impl_->annex_b_buf.push_back((uint8_t)(nal_len >> 8));
                impl_->annex_b_buf.push_back((uint8_t)nal_len);

                // Copy NAL data
                impl_->annex_b_buf.insert(impl_->annex_b_buf.end(),
                                          data + pos + 4, data + pos + 4 + nal_len);
                pos += 4 + nal_len;
            }
        }

        if (!impl_->annex_b_buf.empty()) {
            feed_data = impl_->annex_b_buf.data();
            feed_size = impl_->annex_b_buf.size();
        }
    }

    impl_->pkt->data = const_cast<uint8_t*>(feed_data);
    impl_->pkt->size = static_cast<int>(feed_size);

    // FFmpeg H.264 decoder has a 1-frame pipeline delay: it accepts a packet
    // via send_packet but doesn't produce output until the NEXT packet arrives.
    // Fix: try to drain pending output BEFORE sending the new packet.
    av_frame_unref(impl_->frame);
    int ret = avcodec_receive_frame(impl_->ctx, impl_->frame);
    if (ret == 0) {
        // Got a pending frame from previous packet — send current packet
        // for next time, then return this frame.
        avcodec_send_packet(impl_->ctx, impl_->pkt);
        goto have_frame;
    }

    // No pending output — send packet and try to receive
    ret = avcodec_send_packet(impl_->ctx, impl_->pkt);

    static int dbg_count = 0;
    dbg_count++;
    if (dbg_count <= 5) {
        fprintf(stderr, "  [h264 dbg #%d] send_packet(%d bytes) = %d, nal=0x%02x\n",
                dbg_count, impl_->pkt->size,  ret,
                (impl_->pkt->size >= 5) ? impl_->pkt->data[4] : 0);
    }

    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // Input buffer full — drain output first, then resend
            av_frame_unref(impl_->frame);
            avcodec_receive_frame(impl_->ctx, impl_->frame);
            ret = avcodec_send_packet(impl_->ctx, impl_->pkt);
        }
        if (ret < 0 && ret != AVERROR_INVALIDDATA) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            impl_->last_error = std::string("send_packet failed: ") + errbuf;
            return false;
        }
    }

    av_frame_unref(impl_->frame);
    ret = avcodec_receive_frame(impl_->ctx, impl_->frame);

    if (dbg_count <= 5) {
        fprintf(stderr, "  [h264 dbg #%d] receive_frame = %d%s\n",
                dbg_count, ret,
                ret == AVERROR(EAGAIN) ? " (EAGAIN)" :
                ret == 0 ? " (OK!)" : " (error)");
    }

    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            impl_->last_error = "Decoder needs more data";
        } else {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            impl_->last_error = std::string("receive_frame failed: ") + errbuf;
        }
        return false;
    }

have_frame:

    // We have a decoded frame — convert YUV to RGB
    int src_w = impl_->frame->width;
    int src_h = impl_->frame->height;
    int dst_w = (impl_->output_width > 0) ? impl_->output_width : src_w;
    int dst_h = (impl_->output_height > 0) ? impl_->output_height : src_h;

    // Recreate sws context if dimensions changed
    if (src_w != impl_->last_decoded_width || src_h != impl_->last_decoded_height) {
        if (impl_->sws) sws_freeContext(impl_->sws);
        impl_->sws = sws_getContext(
            src_w, src_h, (AVPixelFormat)impl_->frame->format,
            dst_w, dst_h, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!impl_->sws) {
            impl_->last_error = "Failed to create swscale context";
            return false;
        }
        impl_->last_decoded_width = src_w;
        impl_->last_decoded_height = src_h;
        fprintf(stderr, "H.264 decoder: %dx%d -> %dx%d RGB\n", src_w, src_h, dst_w, dst_h);
    }

    // Allocate output
    rgb_out.width = dst_w;
    rgb_out.height = dst_h;
    rgb_out.stride = dst_w * 3;
    rgb_out.data.resize(rgb_out.stride * dst_h);

    uint8_t* dst_data[1] = { rgb_out.data.data() };
    int dst_linesize[1] = { rgb_out.stride };

    sws_scale(impl_->sws,
              impl_->frame->data, impl_->frame->linesize,
              0, src_h,
              dst_data, dst_linesize);

    av_frame_unref(impl_->frame);
    return true;
}

bool H264Decoder::reinject_idr(RGBFrame& rgb_out) {
    if (!impl_->ctx || impl_->stored_idr.empty()) {
        impl_->last_error = "No stored IDR available";
        return false;
    }

    // Flush the decoder to clear stale state
    avcodec_flush_buffers(impl_->ctx);

    // Re-decode the stored IDR frame
    fprintf(stderr, "H.264 decoder: re-injecting stored IDR (%zu bytes)\n",
            impl_->stored_idr.size());
    return decode(impl_->stored_idr.data(), impl_->stored_idr.size(), rgb_out);
}

bool H264Decoder::flush(RGBFrame& rgb_out) {
    if (!impl_->ctx) return false;

    avcodec_send_packet(impl_->ctx, nullptr);  // flush signal
    int ret = avcodec_receive_frame(impl_->ctx, impl_->frame);
    if (ret < 0) return false;

    // Same conversion as decode()
    int src_w = impl_->frame->width;
    int src_h = impl_->frame->height;
    int dst_w = (impl_->output_width > 0) ? impl_->output_width : src_w;
    int dst_h = (impl_->output_height > 0) ? impl_->output_height : src_h;

    if (!impl_->sws) return false;

    rgb_out.width = dst_w;
    rgb_out.height = dst_h;
    rgb_out.stride = dst_w * 3;
    rgb_out.data.resize(rgb_out.stride * dst_h);

    uint8_t* dst_data[1] = { rgb_out.data.data() };
    int dst_linesize[1] = { rgb_out.stride };

    sws_scale(impl_->sws,
              impl_->frame->data, impl_->frame->linesize,
              0, src_h,
              dst_data, dst_linesize);

    av_frame_unref(impl_->frame);
    return true;
}

void H264Decoder::shutdown() {
    if (impl_->sws) { sws_freeContext(impl_->sws); impl_->sws = nullptr; }
    if (impl_->frame) { av_frame_free(&impl_->frame); }
    if (impl_->pkt) { av_packet_free(&impl_->pkt); }
    if (impl_->ctx) { avcodec_free_context(&impl_->ctx); }
}

std::string H264Decoder::get_last_error() const {
    return impl_->last_error;
}

} // namespace webcam
