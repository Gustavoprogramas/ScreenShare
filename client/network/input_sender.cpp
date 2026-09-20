#include "input_sender.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
//  SDL Keycode → Windows Virtual Key Code conversion
// ============================================================
static uint32_t SDLKeycodeToWindowsVK(SDL_Keycode key) {
    // Letters: SDL uses lowercase ASCII, Windows VK uses uppercase
    if (key >= 'a' && key <= 'z') return key - 'a' + 0x41; // VK_A..VK_Z
    // Digits: same mapping
    if (key >= '0' && key <= '9') return key; // VK_0..VK_9

    switch (key) {
    case SDLK_BACKSPACE:    return 0x08; // VK_BACK
    case SDLK_TAB:          return 0x09; // VK_TAB
    case SDLK_RETURN:       return 0x0D; // VK_RETURN
    case SDLK_ESCAPE:       return 0x1B; // VK_ESCAPE
    case SDLK_SPACE:        return 0x20; // VK_SPACE
    case SDLK_DELETE:       return 0x2E; // VK_DELETE
    case SDLK_INSERT:       return 0x2D; // VK_INSERT
    case SDLK_HOME:         return 0x24; // VK_HOME
    case SDLK_END:          return 0x23; // VK_END
    case SDLK_PAGEUP:       return 0x21; // VK_PRIOR
    case SDLK_PAGEDOWN:     return 0x22; // VK_NEXT
    case SDLK_UP:           return 0x26; // VK_UP
    case SDLK_DOWN:         return 0x28; // VK_DOWN
    case SDLK_LEFT:         return 0x25; // VK_LEFT
    case SDLK_RIGHT:        return 0x27; // VK_RIGHT
    case SDLK_LSHIFT:       return 0xA0; // VK_LSHIFT
    case SDLK_RSHIFT:       return 0xA1; // VK_RSHIFT
    case SDLK_LCTRL:        return 0xA2; // VK_LCONTROL
    case SDLK_RCTRL:        return 0xA3; // VK_RCONTROL
    case SDLK_LALT:         return 0xA4; // VK_LMENU
    case SDLK_RALT:         return 0xA5; // VK_RMENU
    case SDLK_CAPSLOCK:     return 0x14; // VK_CAPITAL
    case SDLK_F1:           return 0x70;
    case SDLK_F2:           return 0x71;
    case SDLK_F3:           return 0x72;
    case SDLK_F4:           return 0x73;
    case SDLK_F5:           return 0x74;
    case SDLK_F6:           return 0x75;
    case SDLK_F7:           return 0x76;
    case SDLK_F8:           return 0x77;
    case SDLK_F9:           return 0x78;
    case SDLK_F10:          return 0x79;
    case SDLK_F11:          return 0x7A;
    case SDLK_F12:          return 0x7B;
    case SDLK_PRINTSCREEN:  return 0x2C;
    case SDLK_SCROLLLOCK:   return 0x91;
    case SDLK_PAUSE:        return 0x13;
    case SDLK_NUMLOCKCLEAR: return 0x90;
    case SDLK_SEMICOLON:    return 0xBA;
    case SDLK_EQUALS:       return 0xBB;
    case SDLK_COMMA:        return 0xBC;
    case SDLK_MINUS:        return 0xBD;
    case SDLK_PERIOD:       return 0xBE;
    case SDLK_SLASH:        return 0xBF;
    case SDLK_BACKQUOTE:    return 0xC0;
    case SDLK_LEFTBRACKET:  return 0xDB;
    case SDLK_BACKSLASH:    return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD;
    case SDLK_QUOTE:        return 0xDE;
    case SDLK_KP_0:         return 0x60;
    case SDLK_KP_1:         return 0x61;
    case SDLK_KP_2:         return 0x62;
    case SDLK_KP_3:         return 0x63;
    case SDLK_KP_4:         return 0x64;
    case SDLK_KP_5:         return 0x65;
    case SDLK_KP_6:         return 0x66;
    case SDLK_KP_7:         return 0x67;
    case SDLK_KP_8:         return 0x68;
    case SDLK_KP_9:         return 0x69;
    case SDLK_KP_MULTIPLY:  return 0x6A;
    case SDLK_KP_PLUS:      return 0x6B;
    case SDLK_KP_MINUS:     return 0x6D;
    case SDLK_KP_PERIOD:    return 0x6E;
    case SDLK_KP_DIVIDE:    return 0x6F;
    case SDLK_KP_ENTER:     return 0x0D;
    default:                return 0;
    }
}

static bool IsExtendedKey(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT:
    case SDLK_INSERT: case SDLK_DELETE: case SDLK_HOME: case SDLK_END:
    case SDLK_PAGEUP: case SDLK_PAGEDOWN:
    case SDLK_RCTRL: case SDLK_RALT:
    case SDLK_KP_ENTER: case SDLK_KP_DIVIDE:
    case SDLK_PRINTSCREEN: case SDLK_PAUSE:
        return true;
    default:
        return false;
    }
}

// ============================================================
//  InputSender::~InputSender
// ============================================================
InputSender::~InputSender() {
    Disconnect();
}

uint64_t InputSender::NowMicroseconds() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1'000'000ULL) / static_cast<uint64_t>(qpc_freq_.QuadPart);
}

// ============================================================
//  InputSender::Connect
// ============================================================
bool InputSender::Connect(const std::string& host_ip, uint16_t port) {
    QueryPerformanceFrequency(&qpc_freq_);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[InputSender] WSAStartup failed: %d\n", WSAGetLastError());
        return false;
    }

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) {
        fprintf(stderr, "[InputSender] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    // ---- Critical: disable Nagle's algorithm BEFORE connect ----------
    // If set after connect, there's a race; setting before is safe.
    int nodelay = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host_ip.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "[InputSender] Invalid host IP: %s\n", host_ip.c_str());
        return false;
    }

    if (connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "[InputSender] connect() failed: %d\n", WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }

    printf("[InputSender] Connected to %s:%u (TCP_NODELAY=ON)\n", host_ip.c_str(), port);
    return true;
}

// ============================================================
//  InputSender::Disconnect
// ============================================================
void InputSender::Disconnect() {
    if (sock_ != INVALID_SOCKET) {
        shutdown(sock_, SD_BOTH);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    WSACleanup();
}

// ============================================================
//  InputSender::Send
// ============================================================
void InputSender::Send(const InputPacket& pkt) {
    if (sock_ == INVALID_SOCKET) return;

    int sent = send(sock_,
                    reinterpret_cast<const char*>(&pkt),
                    sizeof(InputPacket),
                    0);
    if (sent == SOCKET_ERROR) {
        fprintf(stderr, "[InputSender] send() failed: %d\n", WSAGetLastError());
        // Connection may be lost; the render loop will detect this
    }
}

// ============================================================
//  InputSender::ProcessEvent
//  Translates SDL events into InputPackets.
//
//  Coordinate mapping:
//  SDL gives mouse positions in render-window pixel space.
//  We need to map to [0..65535] absolute space (Host's resolution).
//  Formula: abs = (sdl_pos * 65535) / render_dim
// ============================================================
bool InputSender::ProcessEvent(const SDL_Event& ev,
                               int render_width, int render_height,
                               int host_width,   int host_height) {
    InputPacket pkt{};
    pkt.timestamp_us = NowMicroseconds();

    switch (ev.type) {

    // ---- Mouse movement ----------------------------------------
    case SDL_MOUSEMOTION: {
        pkt.type = InputType::MOUSE_MOVE;
        // Map render-space → [0..65535] absolute (Host coordinate space)
        pkt.x = static_cast<int32_t>(
            (static_cast<int64_t>(ev.motion.x) * 65535) / render_width);
        pkt.y = static_cast<int32_t>(
            (static_cast<int64_t>(ev.motion.y) * 65535) / render_height);
        Send(pkt);
        return true;
    }

    // ---- Mouse button down -------------------------------------
    case SDL_MOUSEBUTTONDOWN: {
        pkt.type = InputType::MOUSE_BTN;
        pkt.x    = 1; // pressed
        if      (ev.button.button == SDL_BUTTON_LEFT)   pkt.button = static_cast<uint8_t>(MouseButton::LEFT);
        else if (ev.button.button == SDL_BUTTON_RIGHT)  pkt.button = static_cast<uint8_t>(MouseButton::RIGHT);
        else if (ev.button.button == SDL_BUTTON_MIDDLE) pkt.button = static_cast<uint8_t>(MouseButton::MIDDLE);
        else return false;
        Send(pkt);
        return true;
    }

    // ---- Mouse button up ---------------------------------------
    case SDL_MOUSEBUTTONUP: {
        pkt.type = InputType::MOUSE_BTN;
        pkt.x    = 0; // released
        if      (ev.button.button == SDL_BUTTON_LEFT)   pkt.button = static_cast<uint8_t>(MouseButton::LEFT);
        else if (ev.button.button == SDL_BUTTON_RIGHT)  pkt.button = static_cast<uint8_t>(MouseButton::RIGHT);
        else if (ev.button.button == SDL_BUTTON_MIDDLE) pkt.button = static_cast<uint8_t>(MouseButton::MIDDLE);
        else return false;
        Send(pkt);
        return true;
    }

    // ---- Mouse wheel -------------------------------------------
    case SDL_MOUSEWHEEL: {
        pkt.type = InputType::MOUSE_WHEEL;
        // SDL wheel y: positive = scroll up; Windows WHEEL_DELTA = 120 per notch
        pkt.x = ev.wheel.y * WHEEL_DELTA;
        Send(pkt);
        return true;
    }

    // ---- Key down ----------------------------------------------
    case SDL_KEYDOWN: {
        if (ev.key.repeat) return false; // ignore key-repeat events
        pkt.type      = InputType::KEY_DOWN;
        pkt.vk_code   = SDLKeycodeToWindowsVK(ev.key.keysym.sym);
        pkt.scan_code = static_cast<uint32_t>(ev.key.keysym.scancode); // SDL scancode as backup
        // Extended key detection for arrow keys, numpad, insert/delete/home/end/pgup/pgdown, right ctrl/alt
        pkt.key_flags = IsExtendedKey(ev.key.keysym.sym) ? 0x01 : 0x00;
        if (pkt.vk_code != 0) {
            Send(pkt);
            return true;
        }
        return false;
    }

    // ---- Key up ------------------------------------------------
    case SDL_KEYUP: {
        pkt.type      = InputType::KEY_UP;
        pkt.vk_code   = SDLKeycodeToWindowsVK(ev.key.keysym.sym);
        pkt.scan_code = static_cast<uint32_t>(ev.key.keysym.scancode);
        pkt.key_flags = IsExtendedKey(ev.key.keysym.sym) ? 0x01 : 0x00;
        if (pkt.vk_code != 0) {
            Send(pkt);
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}
