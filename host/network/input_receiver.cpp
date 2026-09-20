#include "input_receiver.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
//  InputReceiver::~InputReceiver
// ============================================================
InputReceiver::~InputReceiver() {
    Stop();
}

// ============================================================
//  InputReceiver::Init
// ============================================================
bool InputReceiver::Init(uint16_t port, InputDispatch dispatch_fn, int cpu_core) {
    dispatch_fn_ = std::move(dispatch_fn);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[InputReceiver] WSAStartup failed: %d\n", WSAGetLastError());
        return false;
    }

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        fprintf(stderr, "[InputReceiver] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    // ---- Reuse address to allow quick restarts ----
    int reuse = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "[InputReceiver] bind() failed: %d\n", WSAGetLastError());
        return false;
    }

    if (listen(listen_sock_, 1) != 0) {
        fprintf(stderr, "[InputReceiver] listen() failed: %d\n", WSAGetLastError());
        return false;
    }

    printf("[InputReceiver] Listening on TCP port %u\n", port);

    running_.store(true);
    thread_ = std::thread([this, cpu_core] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        if (cpu_core >= 0) {
            DWORD_PTR mask = 1ULL << cpu_core;
            SetThreadAffinityMask(GetCurrentThread(), mask);
            printf("[InputReceiver] Thread pinned to CPU core %d\n", cpu_core);
        }

        AcceptLoop();
    });

    return true;
}

// ============================================================
//  InputReceiver::Stop
// ============================================================
void InputReceiver::Stop() {
    running_.store(false);

    if (client_sock_ != INVALID_SOCKET) {
        shutdown(client_sock_, SD_BOTH);
        closesocket(client_sock_);
        client_sock_ = INVALID_SOCKET;
    }
    if (listen_sock_ != INVALID_SOCKET) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }

    if (thread_.joinable()) thread_.join();
}

// ============================================================
//  InputReceiver::AcceptLoop — waits for the Client to connect
// ============================================================
void InputReceiver::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        int         addrlen = sizeof(client_addr);

        SOCKET client = accept(listen_sock_,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &addrlen);
        if (client == INVALID_SOCKET) {
            if (running_.load())
                fprintf(stderr, "[InputReceiver] accept() failed: %d\n", WSAGetLastError());
            break;
        }

        // ---- Critical: disable Nagle's algorithm -----------------
        // Without TCP_NODELAY, Windows batches 28-byte InputPackets
        // causing ~200ms of input stutter ("elastic mouse" sensation).
        int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        char ip_str[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("[InputReceiver] Client connected from %s\n", ip_str);

        client_sock_ = client;
        RecvLoop(client);

        closesocket(client_sock_);
        client_sock_ = INVALID_SOCKET;
        printf("[InputReceiver] Client disconnected — waiting for reconnect\n");
    }
}

// ============================================================
//  InputReceiver::RecvLoop — receives InputPackets over TCP.
//  Uses a small persistent buffer to handle partial reads.
// ============================================================
void InputReceiver::RecvLoop(SOCKET client_sock) {
    static constexpr size_t PKT_SIZE = sizeof(InputPacket);

    uint8_t  buf[PKT_SIZE * 4]; // handle up to 4 packets per read
    size_t   buf_used = 0;

    while (running_.load()) {
        int received = recv(client_sock,
                            reinterpret_cast<char*>(buf + buf_used),
                            static_cast<int>(sizeof(buf) - buf_used),
                            0);

        if (received <= 0) {
            // 0 = graceful disconnect, <0 = error
            break;
        }

        buf_used += static_cast<size_t>(received);

        // Process all complete packets in the buffer
        size_t offset = 0;
        while (buf_used - offset >= PKT_SIZE) {
            InputPacket pkt;
            memcpy(&pkt, buf + offset, PKT_SIZE);
            dispatch_fn_(pkt);
            offset += PKT_SIZE;
        }

        // Shift remaining partial packet to front
        if (offset > 0 && offset < buf_used) {
            memmove(buf, buf + offset, buf_used - offset);
        }
        buf_used -= offset;
    }
}
