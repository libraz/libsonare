#pragma once

/// @file capture.h
/// @brief Realtime-safe punch in/out capture sink.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::engine {

struct CaptureSegment {
  float* const* channels = nullptr;
  int num_channels = 0;
  int64_t capacity_frames = 0;
};

class CaptureSink {
 public:
  void prepare(CaptureSegment segment) noexcept;
  void arm(bool armed) noexcept;
  void set_punch(int64_t start_sample, int64_t end_sample, bool enabled) noexcept;
  void reset() noexcept;

  void process(const float* const* input, int num_channels, int num_frames,
               int64_t timeline_sample) noexcept;

  int64_t captured_frames() const noexcept { return captured_frames_; }
  uint32_t overflow_count() const noexcept { return overflow_count_; }
  bool armed() const noexcept { return armed_; }
  bool punch_enabled() const noexcept { return punch_enabled_; }
  int64_t punch_start_sample() const noexcept { return punch_start_sample_; }
  int64_t punch_end_sample() const noexcept { return punch_end_sample_; }

 private:
  CaptureSegment segment_{};
  int64_t captured_frames_ = 0;
  uint32_t overflow_count_ = 0;
  bool armed_ = false;
  bool punch_enabled_ = false;
  int64_t punch_start_sample_ = 0;
  int64_t punch_end_sample_ = 0;
};

struct CaptureBoundaryList {
  static constexpr size_t kCapacity = 4;
  std::array<int, kCapacity> offsets{};
  size_t size = 0;
  bool overflowed = false;

  void clear() noexcept;
  bool add(int offset) noexcept;
  void sort_unique() noexcept;
};

void collect_capture_boundaries(int64_t block_start_sample, int num_frames, int64_t punch_start,
                                int64_t punch_end, CaptureBoundaryList* out) noexcept;

}  // namespace sonare::engine
