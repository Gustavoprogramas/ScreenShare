#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>
#include <cassert>

// ============================================================
//  Lock-free Single-Producer / Single-Consumer ring buffer
//
//  Designed for the critical hot paths:
//    • capture_thread  → encode_thread   (encoded AVFrame/texture handle)
//    • encode_thread   → send_thread     (AVPacket bytes)
//    • recv_thread     → decode_thread   (encoded bytes from network)
//    • decode_thread   → render_thread   (decoded AVFrame)
//
//  Template parameters:
//    T    — element type (must be trivially copyable or moveable)
//    N    — capacity (must be a power of 2)
// ============================================================
template<typename T, size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static constexpr size_t MASK = N - 1;

public:
    RingBuffer() : head_(0), tail_(0) {}

    // Producer side — returns false if buffer is full
    bool push(T&& item) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next_h = (h + 1) & MASK;
        if (next_h == tail_.load(std::memory_order_acquire))
            return false; // full
        buf_[h] = std::move(item);
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    bool push(const T& item) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next_h = (h + 1) & MASK;
        if (next_h == tail_.load(std::memory_order_acquire))
            return false;
        buf_[h] = item;
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    // Consumer side — returns nullopt if empty
    std::optional<T> pop() noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return std::nullopt; // empty
        T item = std::move(buf_[t]);
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t + N) & MASK;
    }

private:
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::array<T, N> buf_;
};
