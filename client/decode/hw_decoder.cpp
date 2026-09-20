#include "hw_decoder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

static void LogAVErr(const char* ctx, int err) {
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "[Decoder] %s: %s\n", ctx, buf);
}

// ============================================================
//  HWDecoder::~HWDecoder
// ============================================================
HWDecoder::~HWDecoder() {
    Stop();
    if (av_frame_)   av_frame_free(&av_frame_);
    if (sw_frame_)   av_frame_free(&sw_frame_);
    if (sws_ctx_)    sws_freeContext(sws_ctx_);
    if (codec_ctx_)  avcodec_free_context(&codec_ctx_);
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
}

uint64_t HWDecoder::NowMicroseconds() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1'000'000ULL) / static_cast<uint64_t>(qpc_freq_.QuadPart);
}

// ============================================================
//  HWDecoder::Init
//  Try decoders in order: h264_cuvid → h264_d3d11va → h264
// ============================================================
bool HWDecoder::Init(int width, int height, DecodeQueue* in_queue,
                     RenderQueue* out_queue, int cpu_core) {
    width_     = width;
    height_    = height;
    in_queue_  = in_queue;
    out_queue_ = out_queue;
    QueryPerformanceFrequency(&qpc_freq_);

    const char* hw_decoders[] = { "h264_cuvid", "h264_d3d11va", nullptr };
    const AVCodec* codec = nullptr;

    for (int i = 0; hw_decoders[i] != nullptr; ++i) {
        codec = avcodec_find_decoder_by_name(hw_decoders[i]);
        if (codec) {
            decoder_name_ = hw_decoders[i];
            printf("[Decoder] Using HW decoder: %s\n", hw_decoders[i]);
            break;
        }
    }

    if (!codec) {
        printf("[Decoder] No HW decoder found — using software h264\n");
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        decoder_name_ = "h264 (software)";
    }

    if (!codec) {
        fprintf(stderr, "[Decoder] No H.264 decoder available\n");
        return false;
    }

    // ---- Create hardware device context for HW decoders --------
    if (decoder_name_ == "h264_cuvid") {
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_,
                                         AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (ret < 0) {
            LogAVErr("av_hwdevice_ctx_create(CUDA)", ret);
            // Fall back to software
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            decoder_name_ = "h264 (software)";
        }
    } else if (decoder_name_ == "h264_d3d11va") {
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_,
                                         AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (ret < 0) {
            LogAVErr("av_hwdevice_ctx_create(D3D11VA)", ret);
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            decoder_name_ = "h264 (software)";
        }
    }

    // ---- Allocate codec context --------------------------------
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->width          = width;
    codec_ctx_->height         = height;
    codec_ctx_->thread_count   = 1; // single-threaded for minimum latency
    codec_ctx_->flags          |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2         |= AV_CODEC_FLAG2_FAST;

    if (hw_device_ctx_) {
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    }

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        LogAVErr("avcodec_open2", ret);
        return false;
    }

    av_frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();

    running_.store(true);
    thread_ = std::thread([this, cpu_core] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        if (cpu_core >= 0) {
            SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpu_core);
        }
        DecodeLoop();
    });

    printf("[Decoder] %s initialized %dx%d\n", decoder_name_.c_str(), width, height);
    return true;
}

// ============================================================
//  HWDecoder::Stop
// ============================================================
void HWDecoder::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

// ============================================================
//  HWDecoder::DecodeLoop
// ============================================================
void HWDecoder::DecodeLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        auto item = in_queue_->pop();
        if (!item) {
            Sleep(1);
            continue;
        }
        DecodePacket(*item);
    }
}

// ============================================================
//  HWDecoder::DecodePacket
// ============================================================
bool HWDecoder::DecodePacket(const DecodableFrame& frame) {
    static uint32_t dbg_decode = 0;
    dbg_decode++;
    if (dbg_decode % 60 == 1) printf("[DEBUG-Decoder] DecodePacket #%u frame_id=%u size=%zu\n", dbg_decode, frame.frame_id, frame.data.size());
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    // Wrap the data without copying (data lives until after receive_frame)
    pkt->data = const_cast<uint8_t*>(frame.data.data());
    pkt->size = static_cast<int>(frame.data.size());
    pkt->pts  = static_cast<int64_t>(frame.frame_id);

    int ret = avcodec_send_packet(codec_ctx_, pkt);
    av_packet_free(&pkt); // free wrapper, not data (no buf_ref)

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        LogAVErr("avcodec_send_packet", ret);
        return false;
    }

    while (true) {
        av_frame_unref(av_frame_);
        ret = avcodec_receive_frame(codec_ctx_, av_frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { LogAVErr("avcodec_receive_frame", ret); break; }

        DecodedFrame out;
        out.frame_id      = frame.frame_id;
        out.capture_ts_us = frame.capture_ts_us;
        out.decode_ts_us  = NowMicroseconds();

        if (!DownloadFrame(av_frame_, out)) { printf("[DEBUG-Decoder] DownloadFrame FAILED format=%d\n", av_frame_->format); continue; }

        static uint32_t dbg_decoded = 0;
        dbg_decoded++;
        if (dbg_decoded % 60 == 1) printf("[DEBUG-Decoder] Decoded frame #%u %dx%d pushed to render\n", dbg_decoded, out.width, out.height);
        if (!out_queue_->push(std::move(out))) {
            // Render thread is slower than decode; drop oldest frame silently
        }
    }

    return true;
}

// ============================================================
//  HWDecoder::DownloadFrame
//  Downloads hardware frame to CPU YUV420P for SDL rendering.
// ============================================================
bool HWDecoder::DownloadFrame(AVFrame* hw_frame, DecodedFrame& out) {
    AVFrame* src = hw_frame;

    // If the frame is on hardware, transfer to CPU
    if (hw_frame->format == AV_PIX_FMT_CUDA    ||
        hw_frame->format == AV_PIX_FMT_D3D11   ||
        hw_frame->format == AV_PIX_FMT_D3D11VA_VLD) {

        av_frame_unref(sw_frame_);
        sw_frame_->format = AV_PIX_FMT_YUV420P;

        int ret = av_hwframe_transfer_data(sw_frame_, hw_frame, 0);
        if (ret < 0) { LogAVErr("av_hwframe_transfer_data", ret); return false; }

        src = sw_frame_;
    }

    // Convert to YUV420P if the decoder outputs a different SW format
    const int w = src->width;
    const int h = src->height;

    if (src->format != AV_PIX_FMT_YUV420P) {
        sws_ctx_ = sws_getCachedContext(sws_ctx_,
            w, h, static_cast<AVPixelFormat>(src->format),
            w, h, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!sws_ctx_) return false;

        // Allocate destination buffers
        out.y_stride = w;
        out.u_stride = w / 2;
        out.v_stride = w / 2;
        out.y_plane.resize(w * h);
        out.u_plane.resize(w * h / 4);
        out.v_plane.resize(w * h / 4);

        uint8_t* dst_data[4]  = { out.y_plane.data(), out.u_plane.data(), out.v_plane.data(), nullptr };
        int      dst_lines[4] = { out.y_stride, out.u_stride, out.v_stride, 0 };
        sws_scale(sws_ctx_, src->data, src->linesize, 0, h, dst_data, dst_lines);
    } else {
        // Direct YUV420P — just copy the planes
        out.y_stride = src->linesize[0];
        out.u_stride = src->linesize[1];
        out.v_stride = src->linesize[2];
        out.y_plane.assign(src->data[0], src->data[0] + src->linesize[0] * h);
        out.u_plane.assign(src->data[1], src->data[1] + src->linesize[1] * (h / 2));
        out.v_plane.assign(src->data[2], src->data[2] + src->linesize[2] * (h / 2));
    }

    out.width  = w;
    out.height = h;
    return true;
}
