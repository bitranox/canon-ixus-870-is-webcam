// AV Recorder — saves H.264 + PCM audio to MKV using FFmpeg libavformat

#include "av_recorder.h"

#ifdef HAS_AVFORMAT
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#include <cstdio>
#include <cstring>
#include <vector>

namespace webcam {

struct AVRecorder::Impl {
    AVFormatContext* fmt_ctx = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    int64_t video_pts = 0;
    int64_t audio_pts = 0;
    uint32_t fps = 30;
    uint32_t audio_rate = 44100;
    uint16_t audio_channels = 1;
    // SPS/PPS for codec extradata
    uint8_t sps[32];
    int sps_len = 0;
    uint8_t pps[16];
    int pps_len = 0;
    bool header_written = false;
};

AVRecorder::AVRecorder() : impl_(new Impl), opened_(false) {}
AVRecorder::~AVRecorder() { close(); delete impl_; }

bool AVRecorder::open(const std::string& filename,
                       uint32_t width, uint32_t height, uint32_t fps,
                       uint32_t audio_rate, uint16_t audio_channels) {
    if (opened_) close();

    impl_->fps = fps;
    impl_->audio_rate = audio_rate;
    impl_->audio_channels = audio_channels;

    int ret = avformat_alloc_output_context2(&impl_->fmt_ctx, nullptr, "matroska", filename.c_str());
    if (ret < 0 || !impl_->fmt_ctx) {
        fprintf(stderr, "AVRecorder: Failed to create output context\n");
        return false;
    }

    // Video stream (H.264)
    const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    impl_->video_stream = avformat_new_stream(impl_->fmt_ctx, vcodec);
    if (!impl_->video_stream) {
        fprintf(stderr, "AVRecorder: Failed to create video stream\n");
        return false;
    }
    impl_->video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    impl_->video_stream->codecpar->codec_id = AV_CODEC_ID_H264;
    impl_->video_stream->codecpar->width = width;
    impl_->video_stream->codecpar->height = height;
    impl_->video_stream->time_base = {1, (int)fps};

    // Audio stream (PCM s16le)
    const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    impl_->audio_stream = avformat_new_stream(impl_->fmt_ctx, acodec);
    if (!impl_->audio_stream) {
        fprintf(stderr, "AVRecorder: Failed to create audio stream\n");
        return false;
    }
    impl_->audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    impl_->audio_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    impl_->audio_stream->codecpar->sample_rate = audio_rate;
    impl_->audio_stream->codecpar->ch_layout.nb_channels = audio_channels;
    impl_->audio_stream->codecpar->format = AV_SAMPLE_FMT_S16;
    impl_->audio_stream->time_base = {1, (int)audio_rate};

    // Open file
    if (!(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&impl_->fmt_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "AVRecorder: Failed to open output file: %s\n", filename.c_str());
            return false;
        }
    }

    // Set known Canon IXUS 870 IS SPS/PPS as codec extradata
    // SPS: 67 42 E0 1F DA 02 80 F6 9B 80 80 83 01 (13 bytes)
    // PPS: 68 CE 3C 80 (4 bytes)
    {
        static const uint8_t sps[] = {0x67,0x42,0xE0,0x1F,0xDA,0x02,0x80,0xF6,0x9B,0x80,0x80,0x83,0x01};
        static const uint8_t pps[] = {0x68,0xCE,0x3C,0x80};
        int extra_sz = 4 + sizeof(sps) + 4 + sizeof(pps);
        uint8_t* extra = (uint8_t*)av_malloc(extra_sz);
        int off = 0;
        extra[off++]=0; extra[off++]=0; extra[off++]=0; extra[off++]=1;
        memcpy(extra+off, sps, sizeof(sps)); off += sizeof(sps);
        extra[off++]=0; extra[off++]=0; extra[off++]=0; extra[off++]=1;
        memcpy(extra+off, pps, sizeof(pps));
        impl_->video_stream->codecpar->extradata = extra;
        impl_->video_stream->codecpar->extradata_size = extra_sz;
    }

    ret = avformat_write_header(impl_->fmt_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "AVRecorder: Failed to write header: %d\n", ret);
        return false;
    }
    impl_->header_written = true;

    opened_ = true;
    fprintf(stderr, "AVRecorder: opened %s (%ux%u @%ufps + %uHz audio)\n",
            filename.c_str(), width, height, fps, audio_rate);
    return true;
}

void AVRecorder::write_video(const uint8_t* data, size_t size, bool is_idr) {
    if (!opened_ || !data || size < 5) return;

    if (!impl_->header_written) return;

    // Convert AVCC → Annex B (replace 4-byte length with 00 00 00 01)
    std::vector<uint8_t> annexb;
    annexb.reserve(size + 16);
    size_t pos = 0;
    while (pos + 4 < size) {
        uint32_t nal_len = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
                           ((uint32_t)data[pos+2] << 8) | (uint32_t)data[pos+3];
        if (nal_len < 1 || nal_len > size - pos - 4) break;
        annexb.push_back(0); annexb.push_back(0); annexb.push_back(0); annexb.push_back(1);
        annexb.insert(annexb.end(), data + pos + 4, data + pos + 4 + nal_len);
        pos += 4 + nal_len;
    }

    AVPacket* pkt = av_packet_alloc();
    pkt->data = annexb.data();
    pkt->size = (int)annexb.size();
    pkt->stream_index = impl_->video_stream->index;
    pkt->pts = impl_->video_pts;
    pkt->dts = impl_->video_pts;
    pkt->duration = 1;
    if (is_idr) pkt->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(pkt, {1, (int)impl_->fps}, impl_->video_stream->time_base);
    av_interleaved_write_frame(impl_->fmt_ctx, pkt);
    av_packet_free(&pkt);
    impl_->video_pts++;
}

void AVRecorder::write_audio(const uint8_t* data, size_t size) {
    if (!opened_ || !impl_->header_written || !data || size == 0) return;

    uint32_t samples = (uint32_t)(size / (2 * impl_->audio_channels));

    AVPacket* pkt = av_packet_alloc();
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = (int)size;
    pkt->stream_index = impl_->audio_stream->index;
    pkt->pts = impl_->audio_pts;
    pkt->dts = impl_->audio_pts;
    pkt->duration = samples;
    av_packet_rescale_ts(pkt, {1, (int)impl_->audio_rate}, impl_->audio_stream->time_base);
    av_interleaved_write_frame(impl_->fmt_ctx, pkt);
    av_packet_free(&pkt);
    impl_->audio_pts += samples;
}

void AVRecorder::close() {
    if (!opened_) return;
    if (impl_->header_written) {
        av_write_trailer(impl_->fmt_ctx);
    }
    if (impl_->fmt_ctx) {
        if (!(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&impl_->fmt_ctx->pb);
        avformat_free_context(impl_->fmt_ctx);
        impl_->fmt_ctx = nullptr;
    }
    impl_->video_pts = 0;
    impl_->audio_pts = 0;
    impl_->header_written = false;
    opened_ = false;
    fprintf(stderr, "AVRecorder: closed\n");
}

} // namespace webcam

#else
// Stub when libavformat not available
namespace webcam {
AVRecorder::AVRecorder() : impl_(nullptr), opened_(false) {}
AVRecorder::~AVRecorder() {}
bool AVRecorder::open(const std::string&, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t) {
    fprintf(stderr, "AVRecorder: not available (FFmpeg libavformat not linked)\n");
    return false;
}
void AVRecorder::write_video(const uint8_t*, size_t, bool) {}
void AVRecorder::write_audio(const uint8_t*, size_t) {}
void AVRecorder::close() {}
}
#endif
