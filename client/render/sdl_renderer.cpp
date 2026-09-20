#include "sdl_renderer.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// ============================================================
//  SDLRenderer::~SDLRenderer
// ============================================================
SDLRenderer::~SDLRenderer() {
    if (texture_)  SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_)   SDL_DestroyWindow(window_);
    SDL_Quit();
}

// ============================================================
//  SDLRenderer::Init
// ============================================================
bool SDLRenderer::Init(int width, int height, const std::string& title) {
    width_  = width;
    height_ = height;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[Renderer] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // ---- Create window -------------------------------------------
    window_ = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window_) {
        fprintf(stderr, "[Renderer] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // ---- Create hardware-accelerated renderer -------------------
    // SDL_RENDERER_PRESENTVSYNC is intentionally OMITTED here:
    // We want to present frames as soon as they arrive from the network,
    // not locked to the client's monitor refresh rate.
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
        fprintf(stderr, "[Renderer] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    // Render at native resolution regardless of window resize
    SDL_RenderSetLogicalSize(renderer_, width, height);

    // ---- YUV texture --------------------------------------------
    // SDL_PIXELFORMAT_IYUV = planar YUV 4:2:0 (I420)
    // SDL_UpdateYUVTexture allows separate Y, U, V plane pointers.
    texture_ = SDL_CreateTexture(
        renderer_,
        SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING,
        width, height
    );
    if (!texture_) {
        fprintf(stderr, "[Renderer] SDL_CreateTexture (YUV) failed: %s\n", SDL_GetError());
        return false;
    }

    last_fps_tick_ = SDL_GetTicks64();
    printf("[Renderer] SDL2 initialized: %dx%d\n", width, height);
    return true;
}

// ============================================================
//  SDLRenderer::DrawTexture — hot path called per decoded frame
// ============================================================
void SDLRenderer::DrawTexture(const DecodedFrame& frame) {
    // Upload YUV planes to GPU texture
    SDL_UpdateYUVTexture(
        texture_,
        nullptr,                       // update entire texture
        frame.y_plane.data(),          // Y plane
        frame.y_stride,
        frame.u_plane.data(),          // U plane (Cb)
        frame.u_stride,
        frame.v_plane.data(),          // V plane (Cr)
        frame.v_stride
    );

    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);

    // FPS counter
    frames_this_sec_++;
    uint64_t now = SDL_GetTicks64();
    if (now - last_fps_tick_ >= 1000) {
        current_fps_     = static_cast<double>(frames_this_sec_);
        frames_this_sec_ = 0;
        last_fps_tick_   = now;
    }
}

// ============================================================
//  SDLRenderer::PollEvents
// ============================================================
bool SDLRenderer::PollEvents(SDL_Event& out_event) {
    // SDL_PollEvent is non-blocking
    return SDL_PollEvent(&out_event) != 0;
}

// ============================================================
//  SDLRenderer::UpdateStats — display FPS + latency in title
// ============================================================
void SDLRenderer::UpdateStats(double fps, double latency_ms) {
    char title[128];
    snprintf(title, sizeof(title),
             "Remote Desktop | %.0f FPS | Latency: %.1f ms",
             fps, latency_ms);
    SDL_SetWindowTitle(window_, title);
}
