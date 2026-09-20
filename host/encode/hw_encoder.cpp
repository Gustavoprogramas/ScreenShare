#include "hw_encoder.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

// ============================================================
//  Utility: print FFmpeg error
// ============================================================
static void LogAVError(const char* context, int err) {
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "[Encoder] %s: %s\n", context, buf);
}

// ============================================================
//  HWEncoder::~HWEncoder
// ============================================================
HWEncoder::~HWEncoder() {
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (tmp_d3d11_frame_) av_frame_free(&tmp_d3d11_frame_);
    if (tmp_cuda_frame_)  av_frame_free(&tmp_cuda_frame_);
    if (cuda_frames_ref_) av_buffer_unref(&cuda_frames_ref_);
    if (d3d11_frames_ref_)av_buffer_unref(&d3d11_frames_ref_);
    if (cuda_device_ref_) av_buffer_unref(&cuda_device_ref_);
    if (d3d11_device_ref_)av_buffer_unref(&d3d11_device_ref_);
}

// ============================================================
//  HWEncoder::Init
// ============================================================
bool HWEncoder::Init(ID3D11Device* device, ID3D11DeviceContext* context,
                     const Config& cfg, PacketCallback cb) {
    packet_cb_ = std::move(cb);
    width_     = cfg.width;
    height_    = cfg.height;

    // FORCE SOFTWARE FALLBACK FOR DEBUGGING
    return TryFallbackSoftware(cfg);

    // Try NVENC first (NVIDIA), then AMF (AMD), then QSV (Intel)
    const char* hw_encoders[] = { "h264_nvenc", "h264_amf", "h264_qsv", nullptr };

    for (int i = 0; hw_encoders[i] != nullptr; ++i) {
        const AVCodec* codec = avcodec_find_encoder_by_name(hw_encoders[i]);
        if (!codec) continue;

        printf("[Encoder] Trying %s...\n", hw_encoders[i]);
        encoder_name_ = hw_encoders[i];

        // Clean up any state from previous failed attempt
        if (codec_ctx_) { avcodec_free_context(&codec_ctx_); codec_ctx_ = nullptr; }
        if (d3d11_frames_ref_) { av_buffer_unref(&d3d11_frames_ref_); d3d11_frames_ref_ = nullptr; }
        if (d3d11_device_ref_) { av_buffer_unref(&d3d11_device_ref_); d3d11_device_ref_ = nullptr; }

        if (!InitEncoder(cfg)) {
            fprintf(stderr, "[Encoder] avcodec_open2 failed for %s — skipping\n", hw_encoders[i]);
            continue;
        }

        // Success!
        tmp_d3d11_frame_ = av_frame_alloc();

        printf("[Encoder] %s initialized (%ux%u@%ufps %u Mbps)\n",
               encoder_name_.c_str(), cfg.width, cfg.height, cfg.fps,
               cfg.bitrate_bps / 1'000'000);
        return true;
    }

    // All HW encoders failed — try software
    fprintf(stderr, "[Encoder] All HW encoders failed. Falling back to software.\n");
    return TryFallbackSoftware(cfg);
}

// ============================================================
//  InitD3D11VAContext — wraps our existing D3D11 device in an
//  AVHWDeviceContext so FFmpeg reuses the same GPU context.
// ============================================================
bool HWEncoder::InitD3D11VAContext(ID3D11Device* device,
                                   ID3D11DeviceContext* ctx) {
    d3d11_device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!d3d11_device_ref_) return false;

    auto* hw_ctx     = reinterpret_cast<AVHWDeviceContext*>(d3d11_device_ref_->data);
    auto* d3d11va    = reinterpret_cast<AVD3D11VADeviceContext*>(hw_ctx->hwctx);

    // Hand our existing device to FFmpeg (AddRef so FFmpeg can Release later).
    d3d11va->device         = device;
    d3d11va->device_context = ctx;
    device->AddRef();
    ctx->AddRef();

    int ret = av_hwdevice_ctx_init(d3d11_device_ref_);
    if (ret < 0) { LogAVError("av_hwdevice_ctx_init(D3D11VA)", ret); return false; }

    return true;
}

// ============================================================
//  InitCUDAContext — derives a CUDA context from D3D11VA.
//  This is what enables the D3D11→CUDA zero-copy mapping.
// ============================================================
bool HWEncoder::InitCUDAContext() {
    int ret = av_hwdevice_ctx_create_derived(
        &cuda_device_ref_,
        AV_HWDEVICE_TYPE_CUDA,
        d3d11_device_ref_,
        0
    );
    if (ret < 0) { LogAVError("av_hwdevice_ctx_create_derived(CUDA)", ret); return false; }
    return true;
}

// ============================================================
//  InitFramePools — allocates D3D11 frame pool.
//  All HW encoders use D3D11 directly (no CUDA needed).
// ============================================================
bool HWEncoder::InitFramePools(const Config& cfg) {
    d3d11_frames_ref_ = av_hwframe_ctx_alloc(d3d11_device_ref_);
    if (!d3d11_frames_ref_) return false;

    {
        auto* fctx   = reinterpret_cast<AVHWFramesContext*>(d3d11_frames_ref_->data);
        fctx->format    = AV_PIX_FMT_D3D11;
        fctx->sw_format = AV_PIX_FMT_BGRA;
        fctx->width     = cfg.width;
        fctx->height    = cfg.height;
        fctx->initial_pool_size = 0; // we manage the pool externally
    }

    int ret = av_hwframe_ctx_init(d3d11_frames_ref_);
    if (ret < 0) { LogAVError("av_hwframe_ctx_init(D3D11)", ret); return false; }

    return true;
}

// ============================================================
//  InitEncoder
// ============================================================
bool HWEncoder::InitEncoder(const Config& cfg) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name_.c_str());
    if (!codec) {
        fprintf(stderr, "[Encoder] %s not found in FFmpeg build\n", encoder_name_.c_str());
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->width          = static_cast<int>(cfg.width);
    codec_ctx_->height         = static_cast<int>(cfg.height);
    codec_ctx_->time_base      = { 1, static_cast<int>(cfg.fps) };
    codec_ctx_->framerate      = { static_cast<int>(cfg.fps), 1 };
    codec_ctx_->bit_rate       = cfg.bitrate_bps;
    codec_ctx_->rc_max_rate    = cfg.bitrate_bps;
    codec_ctx_->rc_buffer_size = cfg.bitrate_bps / cfg.fps; // ~1 frame buffer
    codec_ctx_->max_b_frames   = 0; // No B-frames — critical for low latency
    codec_ctx_->gop_size       = cfg.fps * 2; // Keyframe every 2 seconds

    // All HW encoders use D3D11 directly
    codec_ctx_->pix_fmt        = AV_PIX_FMT_D3D11;
    codec_ctx_->hw_device_ctx  = av_buffer_ref(d3d11_device_ref_);
    codec_ctx_->hw_frames_ctx  = av_buffer_ref(d3d11_frames_ref_);

    if (encoder_name_ == "h264_nvenc") {
        av_opt_set(codec_ctx_->priv_data, "preset",      cfg.nvenc_preset, 0);
        av_opt_set(codec_ctx_->priv_data, "tune",        cfg.nvenc_tune,   0);
        av_opt_set(codec_ctx_->priv_data, "rc",          cfg.nvenc_rc,     0);
        av_opt_set_int(codec_ctx_->priv_data, "delay",   0, 0);
        av_opt_set_int(codec_ctx_->priv_data, "forced-idr", 1, 0);
    } else if (encoder_name_ == "h264_amf") {
        av_opt_set(codec_ctx_->priv_data, "quality", "speed", 0);
        av_opt_set(codec_ctx_->priv_data, "rc", "cbr", 0);
        av_opt_set(codec_ctx_->priv_data, "usage", "lowlatency", 0);
    }

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        LogAVError("avcodec_open2", ret);
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    return true;
}

// ============================================================
//  TryFallbackSoftware — libx264 with zerolatency tune
// ============================================================
bool HWEncoder::TryFallbackSoftware(const Config& cfg) {
    fprintf(stderr, "[Encoder] Falling back to libx264 (software)\n");
    using_software_ = true;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        fprintf(stderr, "[Encoder] libx264 not found — no encoder available\n");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    codec_ctx_->width        = static_cast<int>(cfg.width);
    codec_ctx_->height       = static_cast<int>(cfg.height);
    codec_ctx_->time_base    = { 1, static_cast<int>(cfg.fps) };
    codec_ctx_->framerate    = { static_cast<int>(cfg.fps), 1 };
    codec_ctx_->bit_rate     = cfg.bitrate_bps;
    codec_ctx_->pix_fmt      = AV_PIX_FMT_YUV420P;
    codec_ctx_->max_b_frames = 0;
    codec_ctx_->gop_size     = cfg.fps * 2;

    av_opt_set(codec_ctx_->priv_data, "preset",  "ultrafast",    0);
    av_opt_set(codec_ctx_->priv_data, "tune",    "zerolatency",  0);
    av_opt_set(codec_ctx_->priv_data, "crf",     "23",           0);

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) { LogAVError("avcodec_open2(libx264)", ret); return false; }

    printf("[Encoder] libx264 fallback initialized\n");
    return true;
}

// ============================================================
//  WrapD3D11Texture — create an AVFrame that references an
//  existing D3D11 texture without copying it.
// ============================================================
AVFrame* HWEncoder::WrapD3D11Texture(ID3D11Texture2D* tex,
                                     uint32_t subresource) {
    AVFrame* frame = tmp_d3d11_frame_;
    av_frame_unref(frame);

    frame->format = AV_PIX_FMT_D3D11;
    frame->width  = static_cast<int>(width_);
    frame->height = static_cast<int>(height_);

    // data[0] = ID3D11Texture2D*, data[1] = subresource index
    frame->data[0] = reinterpret_cast<uint8_t*>(tex);
    frame->data[1] = reinterpret_cast<uint8_t*>(static_cast<intptr_t>(subresource));

    // Attach the D3D11 frames context so FFmpeg knows the format details
    frame->hw_frames_ctx = av_buffer_ref(d3d11_frames_ref_);

    return frame;
}

// ============================================================
//  HWEncoder::EncodeFrame — hot path
// ============================================================
bool HWEncoder::EncodeFrame(ID3D11Texture2D* texture, uint32_t subresource,
                            uint64_t timestamp_us) {
    static uint32_t dbg_encode_calls = 0;
    dbg_encode_calls++;
    if (dbg_encode_calls % 60 == 1) printf("[DEBUG-Encoder] EncodeFrame called #%u\n", dbg_encode_calls);
    AVFrame* send_frame = nullptr;

    if (!using_software_) {
        // ---- Zero-copy path: D3D11 → Encode (all HW encoders) ----
        AVFrame* d3d11_frame = WrapD3D11Texture(texture, subresource);
        d3d11_frame->pts     = frame_pts_++;
        send_frame = d3d11_frame;

    } else {
        // ---- Software fallback: download texture to CPU and convert ----
        // Download D3D11 texture to a staging texture, then read pixels
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.BindFlags = 0;
        staging_desc.MiscFlags = 0;

        // Get device and context from the texture
        ComPtr<ID3D11Device> dev;
        texture->GetDevice(&dev);
        ComPtr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(&ctx);

        ComPtr<ID3D11Texture2D> staging;
        HRESULT hr = dev->CreateTexture2D(&staging_desc, nullptr, &staging);
        if (FAILED(hr)) return false;

        ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, subresource, nullptr);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        // Map mapped texture to YUV420P AVFrame
        AVFrame* sw_frame = av_frame_alloc();
        sw_frame->format = AV_PIX_FMT_YUV420P;
        sw_frame->width = width_;
        sw_frame->height = height_;
        av_frame_get_buffer(sw_frame, 32);

        const uint8_t* src_data = reinterpret_cast<const uint8_t*>(mapped.pData);
        uint32_t src_stride = mapped.RowPitch;

        int w = static_cast<int>(width_);
        int h = static_cast<int>(height_);

        // Simple BGRA → YUV420P conversion
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int si = y * src_stride + x * 4;
                uint8_t b = src_data[si + 0];
                uint8_t g = src_data[si + 1];
                uint8_t r = src_data[si + 2];

                sw_frame->data[0][y * sw_frame->linesize[0] + x] = 
                    static_cast<uint8_t>((0.257 * r) + (0.504 * g) + (0.098 * b) + 16);

                if (y % 2 == 0 && x % 2 == 0) {
                    sw_frame->data[1][(y/2) * sw_frame->linesize[1] + (x/2)] = 
                        static_cast<uint8_t>(-(0.148 * r) - (0.291 * g) + (0.439 * b) + 128);
                    sw_frame->data[2][(y/2) * sw_frame->linesize[2] + (x/2)] = 
                        static_cast<uint8_t>((0.439 * r) - (0.368 * g) - (0.071 * b) + 128);
                }
            }
        }
        ctx->Unmap(staging.Get(), 0);

        sw_frame->pts = frame_pts_++;
        send_frame = sw_frame;
    }

    // ---- Send frame to encoder ------------------------------------
    int ret = avcodec_send_frame(codec_ctx_, send_frame);
    
    // If we used a software frame, free it (avcodec_send_frame takes a reference)
    if (using_software_ && send_frame) {
        av_frame_free(&send_frame);
    }
    
    if (ret < 0) { LogAVError("avcodec_send_frame", ret); return false; }

    // ---- Drain packets ------------------------------------------
    AVPacket* pkt = av_packet_alloc();
    while (true) {
        ret = avcodec_receive_packet(codec_ctx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { LogAVError("avcodec_receive_packet", ret); break; }

        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts_us      = timestamp_us;
        ep.is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

        static uint32_t dbg_packets = 0;
        dbg_packets++;
        if (dbg_packets % 60 == 1) printf("[DEBUG-Encoder] Produced packet #%u size=%d key=%d\n", dbg_packets, pkt->size, ep.is_keyframe);
        packet_cb_(std::move(ep));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    return true;
}

// ============================================================
//  HWEncoder::Flush — drain the encoder pipeline on shutdown
// ============================================================
void HWEncoder::Flush() {
    if (!codec_ctx_) return;
    avcodec_send_frame(codec_ctx_, nullptr);

    AVPacket* pkt = av_packet_alloc();
    int ret;
    while ((ret = avcodec_receive_packet(codec_ctx_, pkt)) == 0) {
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts_us      = 0;
        ep.is_keyframe = false;
        packet_cb_(std::move(ep));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}
