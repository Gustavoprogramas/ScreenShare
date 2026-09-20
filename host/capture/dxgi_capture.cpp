#include "dxgi_capture.h"

#include <cstdio>
#include <stdexcept>
#include <cassert>

// ============================================================
//  Helpers
// ============================================================
static uint64_t QPCToMicroseconds(LARGE_INTEGER t, LARGE_INTEGER freq) {
    // Avoid overflow: multiply by 1e6 with intermediate division
    return (t.QuadPart * 1'000'000ULL) / static_cast<uint64_t>(freq.QuadPart);
}

// ============================================================
//  DXGICapture::~DXGICapture
// ============================================================
DXGICapture::~DXGICapture() {
    if (frame_acquired_ && duplication_)
        duplication_->ReleaseFrame();
}

// ============================================================
//  DXGICapture::Init
// ============================================================
bool DXGICapture::Init(uint32_t adapter_index, uint32_t output_index) {
    adapter_index_ = adapter_index;
    output_index_  = output_index;

    QueryPerformanceFrequency(&qpc_freq_);

    // --------------------------------------------------------
    //  Create D3D11 device on the primary GPU (adapter 0 = RTX 5060).
    //  D3D11_CREATE_DEVICE_BGRA_SUPPORT is required by DXGI Duplication.
    //  D3D11_CREATE_DEVICE_VIDEO_SUPPORT enables hardware video processing.
    // --------------------------------------------------------
    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Enumerate adapters to pick the right GPU
    ComPtr<IDXGIFactory1> dxgi_factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(dxgi_factory.GetAddressOf()));
    if (FAILED(hr)) {
        fprintf(stderr, "[DXGI] CreateDXGIFactory1 failed: 0x%08X\n", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = dxgi_factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[DXGI] EnumAdapters1(%u) failed: 0x%08X\n", adapter_index, hr);
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    wprintf(L"[DXGI] Using adapter: %s\n", desc.Description);

    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL selected_level   = {};

    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,   // must be UNKNOWN when adapter != nullptr
        nullptr,
        create_flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        d3d_device_.GetAddressOf(),
        &selected_level,
        d3d_context_.GetAddressOf()
    );
    if (FAILED(hr)) {
        fprintf(stderr, "[DXGI] D3D11CreateDevice failed: 0x%08X\n", hr);
        return false;
    }

    if (!InitDuplication(output_index)) return false;
    if (!CreateStagingPoolTexture())    return false;

    printf("[DXGI] Initialized: %ux%u\n", output_width_, output_height_);
    return true;
}

// ============================================================
//  DXGICapture::InitDuplication
// ============================================================
bool DXGICapture::InitDuplication(uint32_t output_index) {
    // Get IDXGIDevice → IDXGIAdapter → IDXGIOutput → IDXGIOutput1
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = d3d_device_.As(&dxgi_device);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(output_index, output.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[DXGI] EnumOutputs(%u) failed: 0x%08X\n", output_index, hr);
        return false;
    }

    DXGI_OUTPUT_DESC out_desc{};
    output->GetDesc(&out_desc);
    output_width_  = static_cast<uint32_t>(out_desc.DesktopCoordinates.right  - out_desc.DesktopCoordinates.left);
    output_height_ = static_cast<uint32_t>(out_desc.DesktopCoordinates.bottom - out_desc.DesktopCoordinates.top);

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return false;

    // DuplicateOutput requires the device to support BGRA.
    hr = output1->DuplicateOutput(d3d_device_.Get(), duplication_.GetAddressOf());
    if (FAILED(hr)) {
        fprintf(stderr, "[DXGI] DuplicateOutput failed: 0x%08X\n", hr);
        return false;
    }

    return true;
}

// ============================================================
//  DXGICapture::CreateStagingPoolTexture
//  Creates a pool of BGRA textures with the flags required for
//  CUDA D3D11 interop (SHADER_RESOURCE + RENDER_TARGET).
// ============================================================
bool DXGICapture::CreateStagingPoolTexture() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width              = output_width_;
    desc.Height             = output_height_;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    // These bind flags are required so NVENC/CUDA can access the texture
    desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = D3D11_RESOURCE_MISC_SHARED; // shared with CUDA

    for (uint32_t i = 0; i < POOL_SIZE; ++i) {
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr,
                                                   texture_pool_[i].GetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "[DXGI] CreateTexture2D (pool %u) failed: 0x%08X\n", i, hr);
            return false;
        }
    }
    return true;
}

// ============================================================
//  DXGICapture::AcquireNextFrame
// ============================================================
bool DXGICapture::AcquireNextFrame(CapturedFrame& out_frame, uint32_t timeout_ms) {
    assert(!frame_acquired_ && "ReleaseFrame() must be called before next acquire");

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource>   dxgi_resource;

    HRESULT hr = duplication_->AcquireNextFrame(
        timeout_ms,
        &frame_info,
        dxgi_resource.GetAddressOf()
    );

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)  return false; // no change, normal
    if (hr == DXGI_ERROR_ACCESS_LOST)   { Reinitialize(); return false; }
    if (FAILED(hr)) {
        fprintf(stderr, "[DXGI] AcquireNextFrame failed: 0x%08X\n", hr);
        return false;
    }

    frame_acquired_ = true;

    // --------------------------------------------------------
    //  QI the resource to ID3D11Texture2D
    // --------------------------------------------------------
    ComPtr<ID3D11Texture2D> raw_texture;
    hr = dxgi_resource.As(&raw_texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        return false;
    }

    // --------------------------------------------------------
    //  GPU-side copy: DXGI texture → our pool texture.
    //  This is a VRAM-to-VRAM copy (few microseconds) and
    //  is necessary because:
    //   1. DXGI textures have no BIND flags for CUDA interop.
    //   2. We release the DXGI frame immediately after copy,
    //      unblocking the desktop compositor.
    // --------------------------------------------------------
    ID3D11Texture2D* dest = texture_pool_[pool_index_].Get();
    d3d_context_->CopyResource(dest, raw_texture.Get());

    // Release the DXGI frame right after the GPU copy command is issued.
    // The GPU copy is queued asynchronously — dest texture is safe to use.
    duplication_->ReleaseFrame();
    frame_acquired_ = false;

    // --------------------------------------------------------
    //  Record capture timestamp
    // --------------------------------------------------------
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t ts_us = QPCToMicroseconds(now, qpc_freq_);

    out_frame.texture      = texture_pool_[pool_index_];
    out_frame.subresource  = 0;
    out_frame.timestamp_us = ts_us;
    out_frame.frame_id     = ++frame_id_;
    out_frame.width        = output_width_;
    out_frame.height       = output_height_;

    pool_index_ = (pool_index_ + 1) % POOL_SIZE;
    return true;
}

// ============================================================
//  DXGICapture::ReleaseFrame — kept for API symmetry.
//  Actual release is done immediately inside AcquireNextFrame.
// ============================================================
void DXGICapture::ReleaseFrame() {
    // Already released in AcquireNextFrame for minimum DXGI hold time.
}

// ============================================================
//  DXGICapture::Reinitialize
// ============================================================
bool DXGICapture::Reinitialize() {
    printf("[DXGI] Access lost — reinitializing...\n");
    duplication_.Reset();
    frame_acquired_ = false;

    // Brief sleep to allow resolution change to settle
    Sleep(200);

    if (!InitDuplication(output_index_))    return false;
    if (!CreateStagingPoolTexture())        return false;

    printf("[DXGI] Reinitialized: %ux%u\n", output_width_, output_height_);
    return true;
}
