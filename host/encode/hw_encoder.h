#pragma once

// ============================================================
//  HWEncoder — FFmpeg NVENC H.264 encoder with D3D11 zero-copy
//
//  Pipeline:
//    ID3D11Texture2D (VRAM, BGRA)
//      → [GPU copy into encoder pool texture]
//      → [D3D11VA→CUDA hwframe_map]
//      → [h264_nvenc preset=p1 tune=ull]
//      → AVPacket (compressed bytes ready for network)
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <vector>
#include <functional>
#include <string>

using Microsoft::WRL::ComPtr;

struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t              pts_us;   // presentation timestamp (microseconds)
    bool                 is_keyframe;
};

// Callback invoked (on encode thread) for each completed packet.
using PacketCallback = std::function<void(EncodedPacket&&)>;

class HWEncoder {
public:
    struct Config {
        uint32_t width        = 1920;
        uint32_t height       = 1080;
        uint32_t fps          = 60;
        uint32_t bitrate_bps  = 12'000'000; // 12 Mbps CBR
        // NVENC presets: p1 (fastest/lowest quality) ... p7 (slowest/best)
        // For remote desktop ull: p1 or p2 gives minimum encode latency
        const char* nvenc_preset = "p1";
        const char* nvenc_tune   = "ull";  // Ultra Low Latency
        const char* nvenc_rc     = "cbr";
    };

    HWEncoder() = default;
    ~HWEncoder();

    // Must pass the same D3D11 device used by DXGICapture.
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context,
              const Config& cfg, PacketCallback cb);

    // Feed a captured D3D11 texture (BGRA, from pool).
    // Returns false on unrecoverable error.
    bool EncodeFrame(ID3D11Texture2D* texture, uint32_t subresource,
                     uint64_t timestamp_us);

    // Flush remaining packets (call on shutdown).
    void Flush();

    bool IsValid() const { return codec_ctx_ != nullptr; }

    const std::string& GetLastError() const { return last_error_; }

private:
    bool InitD3D11VAContext(ID3D11Device* device, ID3D11DeviceContext* ctx);
    bool InitCUDAContext();
    bool InitEncoder(const Config& cfg);
    bool InitFramePools(const Config& cfg);
    bool TryFallbackSoftware(const Config& cfg);

    // Wrap an external D3D11 texture into an AVFrame without copying to CPU.
    AVFrame* WrapD3D11Texture(ID3D11Texture2D* tex, uint32_t subresource);

    AVCodecContext* codec_ctx_         = nullptr;
    AVBufferRef*    d3d11_device_ref_  = nullptr; // AV_HWDEVICE_TYPE_D3D11VA
    AVBufferRef*    cuda_device_ref_   = nullptr; // AV_HWDEVICE_TYPE_CUDA (derived)
    AVBufferRef*    d3d11_frames_ref_  = nullptr; // Input frames (BGRA on D3D11)
    AVBufferRef*    cuda_frames_ref_   = nullptr; // Encoder frames (NV12 on CUDA)

    AVFrame*        tmp_d3d11_frame_   = nullptr; // Reused wrapper frame
    AVFrame*        tmp_sw_frame_      = nullptr; // Reused software frame
    AVFrame*        tmp_cuda_frame_    = nullptr; // Reused CUDA frame

    PacketCallback  packet_cb_;
    std::string     last_error_;
    int64_t         frame_pts_         = 0;
    bool            using_software_    = false;
    std::string     encoder_name_;

    uint32_t        width_  = 0;
    uint32_t        height_ = 0;
};
