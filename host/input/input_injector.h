#pragma once

// ============================================================
//  InputInjector — wraps Win32 SendInput for mouse + keyboard
//
//  Key design decisions:
//  • MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE with coordinates
//    normalized to [0..65535] — works in exclusive fullscreen games.
//  • KEYEVENTF_SCANCODE instead of virtual keys — hardware-level
//    injection bypasses per-app key filtering (osu!, games, etc).
//  • Batches up to MAX_BATCH INPUT structs per SendInput call to
//    minimize syscall overhead without adding latency.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../../shared/protocol.h"

#include <cstdint>
#include <array>

class InputInjector {
public:
    InputInjector() = default;

    // Must be called before any injection.
    // screen_width/height — the Host's resolution, used for
    // absolute mouse coordinate normalization.
    bool Init(uint32_t screen_width, uint32_t screen_height);

    // Process a deserialized InputPacket from the network.
    void Inject(const InputPacket& pkt);

    // Getters so main_host can update on resolution change.
    void SetScreenSize(uint32_t w, uint32_t h) {
        screen_width_  = w;
        screen_height_ = h;
    }

private:
    void InjectMouseMove (const InputPacket& pkt);
    void InjectMouseBtn  (const InputPacket& pkt);
    void InjectMouseWheel(const InputPacket& pkt);
    void InjectKeyDown   (const InputPacket& pkt);
    void InjectKeyUp     (const InputPacket& pkt);

    // Normalize client coordinates [0..65535] → absolute MOUSEINPUT space.
    // Client sends pre-normalized coordinates, so this is mostly a pass-through.
    int32_t NormalizeX(int32_t x) const;
    int32_t NormalizeY(int32_t y) const;

    static constexpr size_t MAX_BATCH = 8;
    std::array<INPUT, MAX_BATCH> batch_{};

    uint32_t screen_width_  = 1920;
    uint32_t screen_height_ = 1080;
};
