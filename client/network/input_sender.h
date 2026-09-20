#pragma once

// ============================================================
//  InputSender — captures SDL2 events and sends InputPackets
//  to the Host over TCP.
//
//  Critical: TCP_NODELAY is set on connect so each InputPacket
//  is transmitted immediately without Nagle buffering.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <SDL2/SDL.h>

#include "../../shared/protocol.h"

#include <string>
#include <atomic>
#include <thread>
#include <cstdint>

class InputSender {
public:
    InputSender() = default;
    ~InputSender();

    // Connect to Host TCP port. Must be called before ProcessEvent.
    bool Connect(const std::string& host_ip, uint16_t port);

    // Feed this an SDL_Event from the render loop.
    // Returns true if the event was an input event (consumed).
    bool ProcessEvent(const SDL_Event& ev,
                      int render_width, int render_height,
                      int host_width,   int host_height);

    void Disconnect();

    bool IsConnected() const { return sock_ != INVALID_SOCKET; }

private:
    void Send(const InputPacket& pkt);
    uint64_t NowMicroseconds() const;

    SOCKET        sock_ = INVALID_SOCKET;
    LARGE_INTEGER qpc_freq_{};
};
