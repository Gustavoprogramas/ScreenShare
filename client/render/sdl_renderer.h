#pragma once

// ============================================================
//  SDLRenderer — renders decoded YUV420P frames via SDL2.
//
//  Uses SDL_RENDERER_ACCELERATED (GPU-backed) with a YUV texture.
//  The render thread displays latency stats in the window title.
// ============================================================

#include <SDL2/SDL.h>

#include "../decode/hw_decoder.h" // DecodedFrame, RenderQueue

#include <string>
#include <atomic>
#include <cstdint>

class SDLRenderer {
public:
    SDLRenderer() = default;
    ~SDLRenderer();

    // Create SDL window + renderer + YUV texture.
    // width/height: Host's video dimensions.
    bool Init(int width, int height, const std::string& title = "Remote Desktop");

    // Draw the decoded frame onto the renderer (call from main render loop).
    void DrawTexture(const DecodedFrame& frame);

    // Poll SDL events — returns false when window is closed.
    bool PollEvents(SDL_Event& out_event);

    // Show FPS and latency in the window title bar.
    void UpdateStats(double fps, double latency_ms);

    SDL_Window*   GetWindow()   const { return window_; }
    SDL_Renderer* GetRenderer() const { return renderer_; }

    bool IsValid() const { return window_ != nullptr; }

private:
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;

    int width_  = 0;
    int height_ = 0;

    // FPS measurement
    uint64_t last_fps_tick_ = 0;
    uint32_t frames_this_sec_ = 0;
    double   current_fps_     = 0.0;
};
