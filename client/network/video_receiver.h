#pragma once

// ============================================================
//  VideoReceiver — receives UDP video stream from Host,
//  reassembles fragments per-frame, and forwards to decoder.
//
//  Design:
//  • Maintains a sliding window reorder buffer (REORDER_BUFFER_FRAMES slots).
//  • Chunks that arrive out-of-order are stored until all chunks
//    of a frame are received or the frame is too old (> 2 frames behind).
//  • When all chunks for a frame are received, the assembled H.264
//    data is pushed into the decode queue.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../../shared/protocol.h"
#include "../../shared/ring_buffer.h"

#include <cstdint>
#include <vector>
#include <array>
#include <atomic>
#include <thread>
#include <functional>
#include <string>

// ============================================================
//  DecodableFrame — assembled video data ready for the decoder
// ============================================================
struct DecodableFrame {
    std::vector<uint8_t> data;
    uint32_t             frame_id;
    uint64_t             capture_ts_us; // host-side timestamp
    uint64_t             recv_ts_us;    // client-side receive timestamp
};

// Ring buffer: recv thread → decode thread
using DecodeQueue = RingBuffer<DecodableFrame, 32>;

class VideoReceiver {
public:
    VideoReceiver() = default;
    ~VideoReceiver();

    bool Init(const std::string& host_ip, uint16_t port, DecodeQueue* decode_queue, int cpu_core = -1);
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void RecvLoop();
    uint64_t NowMicroseconds() const;

    SOCKET              sock_ = INVALID_SOCKET;
    DecodeQueue*        decode_queue_ = nullptr;
    std::thread         thread_;
    std::atomic<bool>   running_{ false };
    LARGE_INTEGER       qpc_freq_{};
};
