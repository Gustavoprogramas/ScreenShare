#pragma once

// ============================================================
//  HWDecoder — FFmpeg H.264 decoder with hardware acceleration.
//
//  Decoder preference order (client-side):
//    1. h264_cuvid   (NVIDIA CUDA, zero-copy to GPU)
//    2. h264_d3d11va (Direct3D 11 hardware decode)
//    3. h264         (software fallback)
//
//  Output: decoded AVFrame in YUV420P or NV12 format,
//  ready to be uploaded into SDL2's YUV texture.
// ============================================================

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "../network/video_receiver.h" // DecodeQueue, DecodableFrame
#include "../../shared/ring_buffer.h"

#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <string>

// ============================================================
//  DecodedFrame — CPU-side YUV420P data for SDL rendering
// ============================================================
struct DecodedFrame {
    std::vector<uint8_t> y_plane;
    std::vector<uint8_t> u_plane;
    std::vector<uint8_t> v_plane;
    int                  y_stride;
    int                  u_stride;
    int                  v_stride;
    int                  width;
    int                  height;
    uint32_t             frame_id;
    uint64_t             capture_ts_us;
    uint64_t             decode_ts_us;
};

// Ring buffer: decode thread → render thread
using RenderQueue = RingBuffer<DecodedFrame, 8>;

class HWDecoder {
public:
    HWDecoder() = default;
    ~HWDecoder();

    bool Init(int width, int height, DecodeQueue* in_queue,
              RenderQueue* out_queue, int cpu_core = -1);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    const std::string& GetDecoderName() const { return decoder_name_; }

private:
    void DecodeLoop();
    bool DecodePacket(const DecodableFrame& frame);
    bool DownloadFrame(AVFrame* hw_frame, DecodedFrame& out);
    uint64_t NowMicroseconds() const;

    AVCodecContext* codec_ctx_    = nullptr;
    AVBufferRef*    hw_device_ctx_= nullptr;
    SwsContext*     sws_ctx_      = nullptr;

    // Temporary frames reused each iteration
    AVFrame*        av_frame_     = nullptr;
    AVFrame*        sw_frame_     = nullptr;

    DecodeQueue*    in_queue_     = nullptr;
    RenderQueue*    out_queue_    = nullptr;

    std::thread       thread_;
    std::atomic<bool> running_{ false };
    std::string       decoder_name_;

    int width_  = 0;
    int height_ = 0;

    LARGE_INTEGER qpc_freq_{};
};
