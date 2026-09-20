#include "video_receiver.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
//  VideoReceiver::~VideoReceiver
// ============================================================
VideoReceiver::~VideoReceiver() {
    Stop();
}

uint64_t VideoReceiver::NowMicroseconds() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1'000'000ULL) / static_cast<uint64_t>(qpc_freq_.QuadPart);
}

// ============================================================
//  VideoReceiver::Init
// ============================================================
bool VideoReceiver::Init(const std::string& host_ip, uint16_t port, DecodeQueue* decode_queue, int cpu_core) {
    decode_queue_ = decode_queue;
    QueryPerformanceFrequency(&qpc_freq_);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host_ip.c_str(), &addr.sin_addr) != 1) return false;

    if (connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "[VideoReceiver] connect() failed: %d\n", WSAGetLastError());
        return false;
    }

    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    printf("[VideoReceiver] Connected to TCP port %u\n", port);

    running_.store(true);
    thread_ = std::thread([this, cpu_core] {
        if (cpu_core >= 0) {
            SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpu_core);
            printf("[VideoReceiver] Thread pinned to CPU core %d\n", cpu_core);
        }
        RecvLoop();
    });

    return true;
}

// ============================================================
//  VideoReceiver::Stop
// ============================================================
void VideoReceiver::Stop() {
    running_.store(false);
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) thread_.join();
    WSACleanup();
}

static bool ReadAllTCP(SOCKET sock, uint8_t* buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        int r = recv(sock, reinterpret_cast<char*>(buf + total), static_cast<int>(size - total), 0);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

// ============================================================
//  VideoReceiver::RecvLoop
// ============================================================
void VideoReceiver::RecvLoop() {
    uint32_t dbg_recv = 0;
    while (running_.load(std::memory_order_relaxed)) {
        struct {
            uint32_t payload_size;
            uint32_t frame_id;
        } hdr;

        if (!ReadAllTCP(sock_, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr))) {
            printf("[VideoReceiver] ReadAllTCP(hdr) failed\n");
            break;
        }

        if (hdr.payload_size == 0 || hdr.payload_size > 10 * 1024 * 1024) {
            fprintf(stderr, "[VideoReceiver] Invalid payload size: %u\n", hdr.payload_size);
            break;
        }

        DecodableFrame df;
        df.frame_id = hdr.frame_id;
        df.capture_ts_us = NowMicroseconds();
        df.data.resize(hdr.payload_size);

        if (!ReadAllTCP(sock_, df.data.data(), hdr.payload_size)) {
            printf("[VideoReceiver] ReadAllTCP(payload) failed\n");
            break;
        }

        dbg_recv++;
        if (dbg_recv % 60 == 1) printf("[DEBUG-Receiver] Got frame #%u id=%u size=%u\n", dbg_recv, hdr.frame_id, hdr.payload_size);

        if (!decode_queue_->push(std::move(df))) {
            // Queue full, drop
        }
    }
    printf("[VideoReceiver] Disconnected\n");
}
