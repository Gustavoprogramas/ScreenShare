#include "video_sender.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
//  VideoSender::~VideoSender
// ============================================================
VideoSender::~VideoSender() {
    Stop();
}

// ============================================================
//  VideoSender::Init
// ============================================================
bool VideoSender::Init(uint16_t bind_port, VideoSendQueue* queue, int cpu_core) {
    queue_ = queue;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(bind_port);

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    if (listen(listen_sock_, 1) != 0) return false;

    printf("[VideoSender] Listening on TCP port %u\n", bind_port);

    running_.store(true);
    thread_ = std::thread([this, bind_port, cpu_core] {
        if (cpu_core >= 0) {
            SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpu_core);
            printf("[VideoSender] Thread pinned to CPU core %d\n", cpu_core);
        }
        AcceptLoop(bind_port);
    });

    return true;
}

// ============================================================
//  VideoSender::Stop
// ============================================================
void VideoSender::Stop() {
    running_.store(false);
    if (listen_sock_ != INVALID_SOCKET) closesocket(listen_sock_);
    if (client_sock_ != INVALID_SOCKET) closesocket(client_sock_);
    if (thread_.joinable()) thread_.join();
    WSACleanup();
}

// ============================================================
//  VideoSender::AcceptLoop
// ============================================================
void VideoSender::AcceptLoop(uint16_t port) {
    sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);

    client_sock_ = accept(listen_sock_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    if (client_sock_ == INVALID_SOCKET) return;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    printf("[VideoSender] Client connected from %s\n", ip_str);

    int sndbuf = 4 * 1024 * 1024;
    setsockopt(client_sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

    // Disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(client_sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    SendLoop();
}

// ============================================================
//  VideoSender::SendLoop
// ============================================================
void VideoSender::SendLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        auto item = queue_->pop();
        if (!item) {
            Sleep(1);
            continue;
        }
        SendEncodedPacket(*item);
    }
}

// ============================================================
//  VideoSender::SendEncodedPacket
// ============================================================
void VideoSender::SendEncodedPacket(const OutgoingPacket& pkt) {
    if (client_sock_ == INVALID_SOCKET) return;

    static uint32_t dbg_sent = 0;
    dbg_sent++;
    if (dbg_sent % 60 == 1) printf("[DEBUG-Sender] Sending packet #%u size=%zu frame_id=%u\n", dbg_sent, pkt.data.size(), pkt.frame_id);

    uint32_t size = static_cast<uint32_t>(pkt.data.size());
    
    // Header (8 bytes): 4 bytes size, 4 bytes padding (or timestamp if we want)
    struct {
        uint32_t payload_size;
        uint32_t frame_id;
    } hdr;
    
    hdr.payload_size = size;
    hdr.frame_id = pkt.frame_id;

    // We can send header and payload sequentially
    int sent_hdr = send(client_sock_, reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);
    if (sent_hdr <= 0) {
        printf("[VideoSender] Client disconnected\n");
        closesocket(client_sock_);
        client_sock_ = INVALID_SOCKET;
        return;
    }

    int total_sent = 0;
    const char* ptr = reinterpret_cast<const char*>(pkt.data.data());
    while (total_sent < (int)size) {
        int sent = send(client_sock_, ptr + total_sent, size - total_sent, 0);
        if (sent <= 0) {
            closesocket(client_sock_);
            client_sock_ = INVALID_SOCKET;
            return;
        }
        total_sent += sent;
    }
}
