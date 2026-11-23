#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::host {

/// Stereo SPSC bridge between independent input and output device clocks.
class LiveInputRing final {
 public:
  LiveInputRing() = default;

  LiveInputRing(const LiveInputRing&) = delete;
  LiveInputRing& operator=(const LiveInputRing&) = delete;

  /// CONTROL thread, with both device callbacks stopped.
  void prepare(int max_input_frames, int max_output_frames, bool clock_locked) {
    frames_.clear();
    capacity_ = 0;
    mask_ = 0;
    target_frames_ = 0;
    low_water_frames_ = 0;
    high_water_frames_ = 0;
    clock_locked_ = clock_locked;
    write_index_.store(0, std::memory_order_relaxed);
    read_index_.store(0, std::memory_order_relaxed);
    primed_ = false;
    primed_once_ = false;
    overrun_frames_.store(0, std::memory_order_relaxed);
    starvation_frames_.store(0, std::memory_order_relaxed);
    drift_repeat_frames_.store(0, std::memory_order_relaxed);
    last_frame_ = {};

    if (max_input_frames <= 0 || max_output_frames <= 0) return;

    const auto input = static_cast<std::size_t>(max_input_frames);
    const auto output = static_cast<std::size_t>(max_output_frames);
    const auto max_size = frames_.max_size();
    if (input > max_size / kCapacityMultiplier || output > max_size / kCapacityMultiplier - input) {
      return;
    }

    const std::size_t minimum_capacity = kCapacityMultiplier * (input + output);
    std::size_t capacity = 1;
    while (capacity < minimum_capacity) {
      if (capacity > max_size / 2) return;
      capacity *= 2;
    }

    const std::size_t margin = std::max(input, output);
    const std::size_t target = input + output + margin;
    if (target + input > capacity) return;

    std::vector<Frame> storage(capacity);
    frames_.swap(storage);
    capacity_ = capacity;
    mask_ = capacity - 1;
    target_frames_ = target;
    low_water_frames_ = target - output;
    high_water_frames_ = target + input;
  }

  /// Input IO thread: accepts a whole block or drops and counts it whole.
  void push(const float* const* planes, int num_channels, int frames) noexcept {
    if (capacity_ == 0 || frames <= 0) return;
    std::uint64_t write = write_index_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_index_.load(std::memory_order_acquire);
    const std::uint64_t queued = write - read;
    const std::uint64_t capacity = static_cast<std::uint64_t>(capacity_);
    const std::uint64_t requested = static_cast<std::uint64_t>(frames);
    if (queued > capacity || requested > capacity - queued) {
      overrun_frames_.fetch_add(requested, std::memory_order_relaxed);
      return;
    }

    for (int i = 0; i < frames; ++i) {
      Frame frame{};
      if (planes != nullptr && num_channels > 0 && planes[0] != nullptr) {
        frame.left = planes[0][i];
        if (num_channels == 1) {
          frame.right = frame.left;
        } else if (num_channels > 1 && planes[1] != nullptr) {
          frame.right = planes[1][i];
        }
      }
      frames_[static_cast<std::size_t>(write) & mask_].left = frame.left;
      frames_[static_cast<std::size_t>(write) & mask_].right = frame.right;
      ++write;
    }
    write_index_.store(write, std::memory_order_release);
  }

  /// Output IO thread: primes with silence, then repeats the previous frame at low water.
  /// At most frames - 1 are repeated, so every corrected block contains a fresh frame.
  void pop(float* const* stereo, int frames) noexcept {
    if (stereo == nullptr || stereo[0] == nullptr || stereo[1] == nullptr || frames <= 0) return;
    if (capacity_ == 0) {
      fill_silence(stereo, 0, frames);
      return;
    }

    std::uint64_t read = read_index_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_index_.load(std::memory_order_acquire);
    std::uint64_t available = write - read;
    if (!primed_) {
      if (available < target_frames_) {
        fill_silence(stereo, 0, frames);
        if (primed_once_) add_starvation(static_cast<std::uint64_t>(frames));
        return;
      }
      primed_ = true;
      primed_once_ = true;
    }

    if (!clock_locked_ && available > high_water_frames_) {
      const std::uint64_t discarded = available - high_water_frames_;
      read += discarded;
      available -= discarded;
      overrun_frames_.fetch_add(discarded, std::memory_order_relaxed);
    }

    int output_index = 0;
    if (available == 0) {
      repeat_last(stereo, output_index, frames);
      add_starvation(static_cast<std::uint64_t>(frames));
      reset_priming(read);
      return;
    }

    if (!clock_locked_ && available < low_water_frames_) {
      const std::uint64_t correction = std::min<std::uint64_t>(
          low_water_frames_ - available, static_cast<std::uint64_t>(frames - 1));
      repeat_last(stereo, output_index, static_cast<int>(correction));
      add_drift_repeats(correction);
    }

    while (output_index < frames && available > 0) {
      const Frame frame = frames_[static_cast<std::size_t>(read) & mask_];
      stereo[0][output_index] = frame.left;
      stereo[1][output_index] = frame.right;
      last_frame_ = frame;
      ++read;
      --available;
      ++output_index;
    }

    if (output_index < frames) {
      const int deficit = frames - output_index;
      repeat_last(stereo, output_index, deficit);
      add_starvation(static_cast<std::uint64_t>(deficit));
      reset_priming(read);
      return;
    }

    read_index_.store(read, std::memory_order_release);
    if (available == 0) primed_ = false;
  }

  /// Input frames dropped at the producer or discarded above high water.
  std::uint64_t overrun_frames() const noexcept {
    return overrun_frames_.load(std::memory_order_relaxed);
  }

  /// Low-water repeats plus actual starvation after the first successful prime.
  std::uint64_t underrun_frames() const noexcept {
    return starvation_frames() + drift_repeat_frames();
  }

  /// Frames actually missing after the first successful prime, including silence while reprime
  /// waits.
  std::uint64_t starvation_frames() const noexcept {
    return starvation_frames_.load(std::memory_order_relaxed);
  }

  /// Frames repeated intentionally to compensate for low-water clock drift.
  std::uint64_t drift_repeat_frames() const noexcept {
    return drift_repeat_frames_.load(std::memory_order_relaxed);
  }

 private:
  struct Frame {
    float left = 0.0f;
    float right = 0.0f;
  };

  static constexpr std::size_t kCapacityMultiplier = 4;
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  void add_starvation(std::uint64_t frames) noexcept {
    starvation_frames_.fetch_add(frames, std::memory_order_relaxed);
  }

  void add_drift_repeats(std::uint64_t frames) noexcept {
    drift_repeat_frames_.fetch_add(frames, std::memory_order_relaxed);
  }

  void reset_priming(std::uint64_t read) noexcept {
    read_index_.store(read, std::memory_order_release);
    primed_ = false;
  }

  static void fill_silence(float* const* stereo, int offset, int frames) noexcept {
    std::fill(stereo[0] + offset, stereo[0] + frames, 0.0f);
    std::fill(stereo[1] + offset, stereo[1] + frames, 0.0f);
  }

  void repeat_last(float* const* stereo, int& output_index, int frames) noexcept {
    for (int i = 0; i < frames; ++i, ++output_index) {
      stereo[0][output_index] = last_frame_.left;
      stereo[1][output_index] = last_frame_.right;
    }
  }

  std::vector<Frame> frames_;
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  std::size_t target_frames_ = 0;
  std::size_t low_water_frames_ = 0;
  std::size_t high_water_frames_ = 0;
  bool clock_locked_ = false;
  bool primed_once_ = false;
  std::atomic<std::uint64_t> write_index_{0};
  std::atomic<std::uint64_t> read_index_{0};
  bool primed_ = false;
  std::atomic<std::uint64_t> overrun_frames_{0};
  std::atomic<std::uint64_t> starvation_frames_{0};
  std::atomic<std::uint64_t> drift_repeat_frames_{0};
  Frame last_frame_{};
};

}  // namespace sonare::host
