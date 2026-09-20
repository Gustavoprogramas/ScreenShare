#pragma once

// ============================================================
//  DXGICapture — Desktop Duplication API wrapper
//
//  Owns the D3D11 device/context that will be SHARED with
//  HWEncoder so that captured textures never leave the VRAM.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>

using Microsoft::WRL::ComPtr;

// Frame data handed from capture thread to encoder thread.
// The texture lives in VRAM on the shared D3D11 device.
struct CapturedFrame {
    ComPtr<ID3D11Texture2D> texture;    // BGRA, still in VRAM
    uint32_t                subresource; // typically 0
    uint64_t                timestamp_us;// QPC-based capture timestamp
    uint32_t                frame_id;   // monotonically increasing
    uint32_t                width;
    uint32_t                height;
};

class DXGICapture {
public:
    DXGICapture() = default;
    ~DXGICapture();

    // Initialize D3D11 device + DXGI output duplication.
    // adapter_index: 0 = primary GPU (RTX 5060).
    // output_index : 0 = primary monitor.
    bool Init(uint32_t adapter_index = 0, uint32_t output_index = 0);

    // Acquire the next changed frame.  Blocks up to timeout_ms.
    // Returns false on timeout (no change) or error; sets *error on error.
    bool AcquireNextFrame(CapturedFrame& out_frame, uint32_t timeout_ms = 16);

    // Must be called after processing each AcquireNextFrame.
    void ReleaseFrame();

    // Accessors for the shared D3D11 device — used by HWEncoder.
    ID3D11Device*        GetDevice()  const { return d3d_device_.Get(); }
    ID3D11DeviceContext* GetContext() const { return d3d_context_.Get(); }

    uint32_t GetOutputWidth()  const { return output_width_; }
    uint32_t GetOutputHeight() const { return output_height_; }

    bool IsValid() const { return duplication_ != nullptr; }

    // Reinitialize after an access-lost error (e.g. screen resolution change).
    bool Reinitialize();

private:
    bool InitDuplication(uint32_t output_index);
    bool CreateStagingPoolTexture();

    ComPtr<ID3D11Device>           d3d_device_;
    ComPtr<ID3D11DeviceContext>    d3d_context_;
    ComPtr<IDXGIOutputDuplication> duplication_;

    // Pool of pre-allocated encoder-compatible textures.
    // We copy the DXGI frame into one of these so it has the right bind flags
    // for CUDA interop (D3D11_BIND_SHADER_RESOURCE + D3D11_BIND_RENDER_TARGET).
    static constexpr uint32_t POOL_SIZE = 4;
    ComPtr<ID3D11Texture2D>  texture_pool_[POOL_SIZE];
    uint32_t                 pool_index_ = 0;

    uint32_t output_width_  = 0;
    uint32_t output_height_ = 0;
    uint32_t adapter_index_ = 0;
    uint32_t output_index_  = 0;
    uint32_t frame_id_      = 0;
    bool     frame_acquired_= false;

    LARGE_INTEGER qpc_freq_{};
};
