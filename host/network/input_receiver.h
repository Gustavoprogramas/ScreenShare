#pragma once

// ============================================================
//  InputReceiver — TCP server that receives InputPackets from
//  the Client and dispatches them to the InputInjector.
//
//  TCP is used here (not UDP) because:
//  • Inputs must be delivered reliably — a dropped key-up event
//    would leave the Host with a stuck key.
//  • TCP_NODELAY disables Nagle's algorithm, so each tiny
//    InputPacket (28 bytes) is sent immediately without buffering.
//  • Input volume is low (~120 packets/sec max) so TCP overhead
//    is negligible.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../../shared/protocol.h"

#include <atomic>
#include <thread>
#include <functional>
#include <string>

using InputDispatch = std::function<void(const InputPacket&)>;

class InputReceiver {
public:
    InputReceiver() = default;
    ~InputReceiver();

    // Bind, listen, and start waiting for Client connection.
    // dispatch_fn: called (on recv thread) for each InputPacket received.
    // cpu_core   : thread affinity (-1 = no affinity).
    bool Init(uint16_t port, InputDispatch dispatch_fn, int cpu_core = -1);

    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void AcceptLoop();
    void RecvLoop(SOCKET client_sock);

    SOCKET            listen_sock_ = INVALID_SOCKET;
    SOCKET            client_sock_ = INVALID_SOCKET;
    std::thread       thread_;
    std::atomic<bool> running_{ false };
    InputDispatch     dispatch_fn_;
};
