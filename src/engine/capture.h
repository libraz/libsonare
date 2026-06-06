#pragma once

/// @file capture.h
/// @brief Realtime-safe punch in/out capture sink.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rt/overflow_counter.h"
#include "rt/seqlock_cell.h"

namespace sonare::engine {

struct CaptureSegment {
  float* const* channels = nullptr;
  int num_channels = 0;
  int64_t capacity_frames = 0;
};

/// @brief Single-writer / single-reader punch in/out capture sink.
///
/// Threading contract:
///  - @ref prepare, @ref arm, @ref set_punch and @ref reset are CONTROL-thread
///    setters. They mutate the published control state (capture segment, armed
///    flag, punch window) and are NOT real-time safe — call them from the
///    control thread only.
///  - @ref process and @ref captured_frames run on the AUDIO thread.
///  - The published control state is handed across the thread boundary through a
///    seqlock (@ref rt::SeqlockCell), so any reader observes a consistent
///    whole-snapshot (no torn reads, no data race) even while the control thread
///    is mid-update: a setter republishes the whole snapshot guarded by an
///    even/odd counter, and a reader that observes an in-progress write retries.
///    The setter is the single seqlock writer (control thread).
///  - @ref process uses the seqlock's non-spinning @c try_load() path so the
///    audio callback never waits for a preempted control-thread store. Accessor
///    getters use the spinning @c load() path and are control-thread helpers.
///
/// Note: @ref reset zeroes @c captured_frames_, which is otherwise owned by the
/// audio thread. @ref reset is therefore expected to be issued while the audio
/// thread is not capturing (i.e. before arming / between renders), matching the
/// engine's command-driven reset path.
class CaptureSink {
 public:
  void prepare(CaptureSegment segment) noexcept;
  void arm(bool armed) noexcept;
  void set_punch(int64_t start_sample, int64_t end_sample, bool enabled) noexcept;
  void reset() noexcept;

  void process(const float* const* input, int num_channels, int num_frames,
               int64_t timeline_sample) noexcept;

  int64_t captured_frames() const noexcept {
    return captured_frames_.load(std::memory_order_relaxed);
  }
  uint32_t overflow_count() const noexcept { return overflow_count_.load(); }
  bool armed() const noexcept { return snapshot().armed; }
  bool punch_enabled() const noexcept { return snapshot().punch_enabled; }
  int64_t punch_start_sample() const noexcept { return snapshot().punch_start_sample; }
  int64_t punch_end_sample() const noexcept { return snapshot().punch_end_sample; }

 private:
  // Trivially-copyable snapshot of the control-thread state, published across
  // the thread boundary via a seqlock. Kept POD so SeqlockCell can hand it to a
  // reader without a lock or allocation. Setters republish the whole snapshot so
  // a partial (torn) update is never visible.
  struct Control {
    CaptureSegment segment{};
    bool armed = false;
    bool punch_enabled = false;
    int64_t punch_start_sample = 0;
    int64_t punch_end_sample = 0;
  };

  Control snapshot() const noexcept { return control_.load(); }
  Control snapshot_rt() const noexcept { return control_.try_load(); }

  // Control-thread shadow of the published state. The setters mutate this then
  // republish the whole snapshot so partial updates are never visible.
  Control control_state_{};
  rt::SeqlockCell<Control> control_{};

  std::atomic<int64_t> captured_frames_{0};
  // Atomic: process() (audio thread) bumps it while overflow_count() is polled
  // cross-thread (control thread, via RealtimeEngine::capture_overflow_count()).
  rt::OverflowCounter overflow_count_{};
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
