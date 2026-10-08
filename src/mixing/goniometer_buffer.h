#pragma once

/// @file goniometer_buffer.h
/// @brief Small best-effort ring for stereo scope points.
///
/// push() writes one point per call and decimates nothing; ChannelStrip calls it
/// once per sample, so the ring turns over at the sample rate and a reader sees
/// only the most recent Capacity points. Thinning for display is the consumer's
/// choice, made from what read_latest() returns.
///
/// Each point is one 64-bit atomic word, so a reader never sees half of a point
/// and reader and writer never race. A reader that falls a whole ring behind
/// drops the points it saw overwritten while it copied; the one point being
/// written as the copy finishes can still arrive out of order. Intended for
/// visual metering, never audio or state decisions.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sonare::mixing {

struct GoniometerPoint {
  float left = 0.0f;
  float right = 0.0f;
};

template <size_t Capacity>
class GoniometerBuffer {
 public:
  static_assert(Capacity > 0, "Capacity must be positive");

  void push(float left, float right) noexcept {
    const size_t written = write_index_.load(std::memory_order_relaxed);
    points_[written % Capacity].store(pack({left, right}), std::memory_order_relaxed);
    write_index_.store(written + 1, std::memory_order_release);
  }

  void reset() noexcept {
    for (auto& point : points_) point.store(0, std::memory_order_relaxed);
    write_index_.store(0, std::memory_order_release);
  }

  size_t read_latest(GoniometerPoint* dest, size_t max_points) const noexcept {
    if (dest == nullptr || max_points == 0) {
      return 0;
    }
    const size_t written = write_index_.load(std::memory_order_acquire);
    const size_t count = written < Capacity ? written : Capacity;
    const size_t out_count = count < max_points ? count : max_points;
    const size_t start = written - out_count;
    for (size_t i = 0; i < out_count; ++i) {
      dest[i] = unpack(points_[(start + i) % Capacity].load(std::memory_order_relaxed));
    }
    // Slots the writer lapped during the copy hold newer points; drop them from the front.
    std::atomic_thread_fence(std::memory_order_acquire);
    const size_t after = write_index_.load(std::memory_order_relaxed);
    const size_t first_valid = after > Capacity ? after - Capacity : 0;
    const size_t lapped = first_valid > start ? first_valid - start : 0;
    if (lapped == 0) return out_count;
    if (lapped >= out_count) return 0;
    for (size_t i = lapped; i < out_count; ++i) dest[i - lapped] = dest[i];
    return out_count - lapped;
  }

 private:
  static uint64_t pack(GoniometerPoint point) noexcept {
    uint64_t word = 0;
    std::memcpy(&word, &point, sizeof(point));
    return word;
  }
  static GoniometerPoint unpack(uint64_t word) noexcept {
    GoniometerPoint point;
    std::memcpy(static_cast<void*>(&point), &word, sizeof(point));
    return point;
  }

  static_assert(sizeof(GoniometerPoint) == sizeof(uint64_t), "a point packs into one word");
  static_assert(std::is_trivially_copyable_v<GoniometerPoint>, "a point is copied as raw bytes");
  std::array<std::atomic<uint64_t>, Capacity> points_{};
  std::atomic<size_t> write_index_{0};
};

}  // namespace sonare::mixing
