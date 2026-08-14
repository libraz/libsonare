#pragma once

/// @file channel_delay.h
/// @brief Fixed-capacity multi-channel delay for plugin-delay compensation
///        (PDC), in Q8.8 fixed-point samples. Integer delays take an exact
///        rt::DelayLine fast path; fractional delays use a 3rd-order Lagrange
///        interpolator so sub-sample instrument latency is compensated too.
///
/// Threading / RT contract
/// -----------------------
///  - CONTROL thread: configure() builds a complete replacement bank and
///    commits it only after every requested lane has been prepared. Call it
///    only between audio blocks.
///  - AUDIO thread: process() and reset() are allocation-free and lock-free.
///    A delay of 0 is a pass-through (process() leaves the samples untouched).

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "rt/delay_line.h"
#include "rt/fractional_delay.h"

namespace sonare::engine {

/// Q8.8-sample delay applied independently to up to @p MaxChannels planar
/// channels (256 == one sample). Used by the engine to phase-align the clip bus
/// and each hosted instrument so their audio reaches the source-merge point
/// time-coherent (PDC). The delay is uniform across channels.
template <std::size_t MaxChannels>
class ChannelDelay {
 public:
  /// CONTROL thread: replace the prepared channel/delay bank. The replacement
  /// is built in a temporary object, so an allocation failure leaves the
  /// current bank fully intact. A zero channel count is accepted to release
  /// every lane's storage.
  bool configure(int prepared_channels, int delay_q8) noexcept {
    const int channels = std::clamp(prepared_channels, 0, static_cast<int>(MaxChannels));
    const int delay = std::max(0, delay_q8);

    try {
      ChannelDelay next;
      next.prepared_channels_ = channels;
      next.delay_q8_ = delay;
      next.fractional_ = (delay & 0xFF) != 0;

      if (delay != 0 && channels != 0) {
        const int integer_delay = delay >> 8;
        if (next.fractional_) {
          // Convert before adding interpolation headroom so a large Q8 request
          // cannot overflow a signed int.
          const std::size_t frac_size = std::max<std::size_t>(
              8, static_cast<std::size_t>(integer_delay) + static_cast<std::size_t>(8));
          for (int channel = 0; channel < channels; ++channel) {
            FractionalLane& lane = next.fractional_lanes_[static_cast<std::size_t>(channel)];
            lane.buffer.assign(frac_size, 0.0f);
          }
        } else {
          for (int channel = 0; channel < channels; ++channel) {
            next.lanes_[static_cast<std::size_t>(channel)].prepare(
                static_cast<std::size_t>(integer_delay));
          }
        }
      }

      swap(next);
      return true;
    } catch (...) {
      return false;
    }
  }

  /// Compatibility spelling for callers that configure the channel count and
  /// delay in separate control-thread steps. New code should prefer configure()
  /// so both values commit as one replacement.
  bool set_prepared_channels(int channels) noexcept { return configure(channels, delay_q8_); }

  /// Compatibility spelling for integer/fractional delay updates. The update
  /// still uses configure(), so inactive and superseded lane storage is
  /// reclaimed on every successful change.
  bool set_delay_q8(int delay_q8) noexcept { return configure(prepared_channels_, delay_q8); }

  /// Convenience: set an integer-sample delay (delay_q8 = samples << 8).
  bool set_delay(int delay_samples) noexcept {
    if (delay_samples <= 0) return set_delay_q8(0);
    constexpr int kMaxIntegerDelay = std::numeric_limits<int>::max() >> 8;
    const int bounded = std::min(delay_samples, kMaxIntegerDelay);
    return set_delay_q8(bounded << 8);
  }

  /// AUDIO thread: zero every lane's history (flush stale audio on a transport
  /// discontinuity such as stop/seek/loop). Allocation-free.
  void reset() noexcept {
    if (delay_q8_ == 0) return;
    for (int channel = 0; channel < prepared_channels_; ++channel) {
      if (fractional_) {
        FractionalLane& lane = fractional_lanes_[static_cast<std::size_t>(channel)];
        std::fill(lane.buffer.begin(), lane.buffer.end(), 0.0f);
        lane.write_index = 0;
      } else {
        lanes_[static_cast<std::size_t>(channel)].reset();
      }
    }
  }

  int delay_q8() const noexcept { return delay_q8_; }
  int delay_samples() const noexcept { return delay_q8_ >> 8; }
  int prepared_channels() const noexcept { return prepared_channels_; }

  /// Total float capacity physically held by all active lanes. Inactive lanes,
  /// zero-delay banks, and the superseded interpolation mode contribute zero.
  size_t capacity() const noexcept {
    size_t total = 0;
    if (fractional_) {
      for (int channel = 0; channel < prepared_channels_; ++channel) {
        total += fractional_lanes_[static_cast<std::size_t>(channel)].buffer.capacity();
      }
    } else {
      for (int channel = 0; channel < prepared_channels_; ++channel) {
        total += lanes_[static_cast<std::size_t>(channel)].capacity();
      }
    }
    return total;
  }
  size_t reserved_samples() const noexcept { return capacity(); }
  size_t storage_capacity() const noexcept { return capacity(); }

  /// CONTROL thread: exchange two fully prepared banks without allocating.
  /// Used by the engine to commit a complete PDC configuration only after all
  /// of its replacement banks have been built successfully.
  void swap(ChannelDelay& other) noexcept {
    for (std::size_t i = 0; i < MaxChannels; ++i) {
      using std::swap;
      swap(lanes_[i], other.lanes_[i]);
      swap(fractional_lanes_[i], other.fractional_lanes_[i]);
    }
    using std::swap;
    swap(delay_q8_, other.delay_q8_);
    swap(prepared_channels_, other.prepared_channels_);
    swap(fractional_, other.fractional_);
  }

  /// AUDIO thread: delay @p num_channels planar buffers in place. A delay of 0
  /// returns immediately, leaving the buffers byte-identical. Channels beyond
  /// MaxChannels are left unprocessed (the engine never exceeds its scratch).
  void process(float* const* channels, int num_channels, int num_frames) noexcept {
    if (delay_q8_ == 0 || channels == nullptr || num_frames <= 0) {
      return;
    }
    const int n = std::min(num_channels, prepared_channels_);
    for (int ch = 0; ch < n; ++ch) {
      float* buffer = channels[ch];
      if (buffer == nullptr) {
        continue;
      }
      if (fractional_) {
        FractionalLane& lane = fractional_lanes_[static_cast<std::size_t>(ch)];
        for (int i = 0; i < num_frames; ++i) {
          buffer[i] =
              rt::lagrange3_fractional_delay(lane.buffer, lane.write_index, delay_q8_, buffer[i]);
        }
      } else {
        rt::DelayLine& lane = lanes_[static_cast<std::size_t>(ch)];
        for (int i = 0; i < num_frames; ++i) {
          buffer[i] = lane.process(buffer[i]);
        }
      }
    }
  }

 private:
  struct FractionalLane {
    std::vector<float> buffer{};
    std::size_t write_index = 0;
  };

  std::array<rt::DelayLine, MaxChannels> lanes_{};
  std::array<FractionalLane, MaxChannels> fractional_lanes_{};
  int delay_q8_ = 0;
  int prepared_channels_ = static_cast<int>(MaxChannels);
  bool fractional_ = false;
};

}  // namespace sonare::engine
