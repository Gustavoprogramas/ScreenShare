#pragma once

// ============================================================
//  VideoSender — fragments and sends encoded H.264 packets
//  over UDP to the Client.
//
//  Each AVPacket is split into chunks of MAX_UDP_PAYLOAD bytes.
//  Each chunk gets a VideoPacketHeader prepended.
//  UDP datagrams are sent from a dedicated thread with
//  THREAD_PRIORITY_HIGHEST and pinned CPU affinity.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../../shared/protocol.h"
#include "../../shared/ring_buffer.h"

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

struct OutgoingPacket {
    std::vector<uint8_t> data;
    uint64_t             timestamp_us;
    uint32_t             frame_id;
    bool                 is_keyframe;
};

// Ring buffer: encode thread → send thread (64-entry SPSC)
using VideoSendQueue = RingBuffer<OutgoingPacket, 64>;

class VideoSender {
public:
    VideoSender() = default;
    ~VideoSender();

    // Bind UDP socket and start the send thread.
    // client_ip: dotted-decimal string of the client's IP.
    // port     : VIDEO_PORT (9000).
    // cpu_core : affinity core for the send thread (-1 = no affinity).
    bool Init(uint16_t bind_port, VideoSendQueue* queue, int cpu_core = -1);

    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void AcceptLoop(uint16_t port);
    void SendLoop();
    void SendEncodedPacket(const OutgoingPacket& pkt);

    SOCKET              listen_sock_ = INVALID_SOCKET;
    SOCKET              client_sock_ = INVALID_SOCKET;
    VideoSendQueue*     queue_       = nullptr;
    std::thread         thread_;
    std::atomic<bool>   running_{ false };
    uint32_t            seq_num_ = 0;
};
