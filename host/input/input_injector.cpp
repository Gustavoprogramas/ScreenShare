#include "input_injector.h"

#include <cstdio>
#include <cstring>

// ============================================================
//  InputInjector::Init
// ============================================================
bool InputInjector::Init(uint32_t screen_width, uint32_t screen_height) {
    screen_width_  = screen_width;
    screen_height_ = screen_height;
    printf("[Input] Injector ready. Screen: %ux%u\n", screen_width_, screen_height_);
    return true;
}

// ============================================================
//  InputInjector::Inject — dispatcher
// ============================================================
void InputInjector::Inject(const InputPacket& pkt) {
    switch (pkt.type) {
    case InputType::MOUSE_MOVE:    InjectMouseMove(pkt);  break;
    case InputType::MOUSE_BTN:     InjectMouseBtn(pkt);   break;
    case InputType::MOUSE_WHEEL:   InjectMouseWheel(pkt); break;
    case InputType::KEY_DOWN:      InjectKeyDown(pkt);    break;
    case InputType::KEY_UP:        InjectKeyUp(pkt);      break;
    default:
        fprintf(stderr, "[Input] Unknown InputType: %u\n",
                static_cast<uint8_t>(pkt.type));
        break;
    }
}

// ============================================================
//  Coordinate normalization
//  Client sends coordinates in [0..65535] absolute space.
//  MOUSEEVENTF_ABSOLUTE also uses [0..65535] — no transform needed.
// ============================================================
int32_t InputInjector::NormalizeX(int32_t x) const { return x; }
int32_t InputInjector::NormalizeY(int32_t y) const { return y; }

// ============================================================
//  InjectMouseMove
// ============================================================
void InputInjector::InjectMouseMove(const InputPacket& pkt) {
    INPUT inp{};
    inp.type           = INPUT_MOUSE;
    inp.mi.dx          = NormalizeX(pkt.x);
    inp.mi.dy          = NormalizeY(pkt.y);
    inp.mi.dwFlags     = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE |
                         MOUSEEVENTF_VIRTUALDESK; // spans all monitors
    inp.mi.time        = 0;  // let Windows timestamp
    inp.mi.dwExtraInfo = 0;

    SendInput(1, &inp, sizeof(INPUT));
}

// ============================================================
//  InjectMouseBtn — handles press and release in one packet.
//  pkt.x: 1 = button pressed, 0 = button released
//  pkt.button: MouseButton mask
// ============================================================
void InputInjector::InjectMouseBtn(const InputPacket& pkt) {
    const bool pressed = (pkt.x != 0);

    INPUT inp{};
    inp.type = INPUT_MOUSE;

    uint8_t btn = pkt.button;

    if (btn & static_cast<uint8_t>(MouseButton::LEFT)) {
        inp.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (btn & static_cast<uint8_t>(MouseButton::RIGHT)) {
        inp.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (btn & static_cast<uint8_t>(MouseButton::MIDDLE)) {
        inp.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    } else {
        return;
    }

    SendInput(1, &inp, sizeof(INPUT));
}

// ============================================================
//  InjectMouseWheel — pkt.x holds the wheel delta
// ============================================================
void InputInjector::InjectMouseWheel(const InputPacket& pkt) {
    INPUT inp{};
    inp.type          = INPUT_MOUSE;
    inp.mi.dwFlags    = MOUSEEVENTF_WHEEL;
    inp.mi.mouseData  = static_cast<DWORD>(pkt.x); // positive = forward
    SendInput(1, &inp, sizeof(INPUT));
}

// ============================================================
//  InjectKeyDown
//  Uses Windows VK codes received from the client, then derives
//  the proper scan code via MapVirtualKey for maximum compatibility.
// ============================================================
void InputInjector::InjectKeyDown(const InputPacket& pkt) {
    INPUT inp{};
    inp.type        = INPUT_KEYBOARD;
    inp.ki.wVk      = static_cast<WORD>(pkt.vk_code);
    inp.ki.wScan    = static_cast<WORD>(MapVirtualKeyW(pkt.vk_code, MAPVK_VK_TO_VSC));
    inp.ki.dwFlags  = 0;
    // Extended key (e.g. right Ctrl, numpad Enter, arrow keys)
    if (pkt.key_flags & 0x01) {
        inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    inp.ki.time        = 0;
    inp.ki.dwExtraInfo = 0;
    SendInput(1, &inp, sizeof(INPUT));
}

// ============================================================
//  InjectKeyUp
// ============================================================
void InputInjector::InjectKeyUp(const InputPacket& pkt) {
    INPUT inp{};
    inp.type        = INPUT_KEYBOARD;
    inp.ki.wVk      = static_cast<WORD>(pkt.vk_code);
    inp.ki.wScan    = static_cast<WORD>(MapVirtualKeyW(pkt.vk_code, MAPVK_VK_TO_VSC));
    inp.ki.dwFlags  = KEYEVENTF_KEYUP;
    if (pkt.key_flags & 0x01) {
        inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    inp.ki.time        = 0;
    inp.ki.dwExtraInfo = 0;
    SendInput(1, &inp, sizeof(INPUT));
}
