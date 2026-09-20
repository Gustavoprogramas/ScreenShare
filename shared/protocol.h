#pragma once

#include <cstdint>

// ============================================================
//  Port configuration
// ============================================================
static constexpr uint16_t VIDEO_PORT   = 9000;  // UDP — video stream
static constexpr uint16_t INPUT_PORT   = 9001;  // TCP — input commands

// ============================================================
//  UDP fragmentation constants
// ============================================================
// MTU-safe payload per UDP datagram (1500 - 28 bytes IP/UDP header = 1472,
// subtract our 20-byte header = 1452; we use 1400 for extra headroom)
static constexpr uint16_t MAX_UDP_PAYLOAD = 1400;

// Maximum chunks we reassemble per frame before giving up
static constexpr uint16_t MAX_CHUNKS_PER_FRAME = 512;

// How many frame slots we keep in the reorder buffer on the client
static constexpr uint32_t REORDER_BUFFER_FRAMES = 4;

// ============================================================
//  VideoPacketHeader  (20 bytes, sent before each UDP chunk)
// ============================================================
#pragma pack(push, 1)
struct VideoPacketHeader {
    uint32_t seq_num;       // Monotonically increasing packet counter
    uint32_t frame_id;      // Unique ID for this video frame
    uint16_t chunk_idx;     // 0-based index of this chunk within the frame
    uint16_t total_chunks;  // Total chunks that make up this frame
    uint32_t payload_size;  // Bytes of video data following this header
    uint64_t capture_ts_us; // Host capture timestamp (microseconds, QPC-based)
};
static_assert(sizeof(VideoPacketHeader) == 24, "VideoPacketHeader size mismatch");

// ============================================================
//  InputType  — discriminator for InputPacket
// ============================================================
enum class InputType : uint8_t {
    MOUSE_MOVE    = 0x01,
    MOUSE_BTN     = 0x02,
    MOUSE_WHEEL   = 0x03,
    KEY_DOWN      = 0x10,
    KEY_UP        = 0x11,
};

// ============================================================
//  MouseButton flags
// ============================================================
enum class MouseButton : uint8_t {
    LEFT   = 0x01,
    RIGHT  = 0x02,
    MIDDLE = 0x04,
};

// ============================================================
//  InputPacket  (20 bytes fixed, sent over TCP)
// ============================================================
struct InputPacket {
    InputType type;         // Discriminator
    uint8_t   button;       // MouseButton mask or 0
    uint8_t   key_flags;    // Bit 0 = extended key; reserved otherwise
    uint8_t   _pad0;
    int32_t   x;            // Mouse absolute X [0..65535] or wheel delta
    int32_t   y;            // Mouse absolute Y [0..65535]
    uint32_t  scan_code;    // Hardware scan code (keyboard events)
    uint32_t  vk_code;      // Virtual-key code (keyboard events)
    uint64_t  timestamp_us; // Client-side timestamp for jitter measurement
};
static_assert(sizeof(InputPacket) == 28, "InputPacket size mismatch");

#pragma pack(pop)
