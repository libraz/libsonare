#pragma once

/// @file channel_delay.h
/// @brief Fixed-capacity multi-channel delay for plugin-delay compensation
///        (PDC), in Q8.8 fixed-point samples. Integer delays take an exact
///        rt::DelayLine fast path; fractional delays use a 3rd-order Lagrange
///        interpolator so sub-sample instrument latency is compensated too.
///
/// Threading / RT contract
/// -----------------------
///  - CONTROL thread: set_delay_q8() reallocates each lane's storage (same
///    contract as prepare()); call it only between audio blocks.
///  - AUDIO thread: process() and reset() are allocation-free and lock-free.
///    A delay of 0 is a pass-through (process() leaves the samples untouched).

#include <algorithm>
#include <array>
#include <cstddef>
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
  /// CONTROL thread: set the per-lane delay in Q8.8 samples (reallocates lane
  /// storage). Negative requests clamp to 0 (pass-through). When the fractional
  /// part is 0 the exact integer DelayLine path is used; otherwise a Lagrange3
  /// fractional buffer (sized to the integer part + interpolator headroom).
  void set_delay_q8(int delay_q8) {
    delay_q8_ = std::max(0, delay_q8);
    const int integer_delay = delay_q8_ >> 8;
    fractional_ = (delay_q8_ & 0xFF) != 0;
    for (rt::DelayLine& lane : lanes_) {
      lane.prepare(static_cast<std::size_t>(integer_delay));
    }
    const std::size_t frac_size = static_cast<std::size_t>(std::max(8, integer_delay + 8));
    for (FractionalLane& lane : fractional_lanes_) {
      lane.buffer.assign(frac_size, 0.0f);
      lane.write_index = 0;
    }
  }

  /// Convenience: set an integer-sample delay (delay_q8 = samples << 8).
  void set_delay(int delay_samples) { set_delay_q8(std::max(0, delay_samples) << 8); }

  /// AUDIO thread: zero every lane's history (flush stale audio on a transport
  /// discontinuity such as stop/seek/loop). Allocation-free.
  void reset() noexcept {
    for (rt::DelayLine& lane : lanes_) {
      lane.reset();
    }
    for (FractionalLane& lane : fractional_lanes_) {
      std::fill(lane.buffer.begin(), lane.buffer.end(), 0.0f);
      lane.write_index = 0;
    }
  }

  int delay_q8() const noexcept { return delay_q8_; }
  int delay_samples() const noexcept { return delay_q8_ >> 8; }

  /// AUDIO thread: delay @p num_channels planar buffers in place. A delay of 0
  /// returns immediately, leaving the buffers byte-identical. Channels beyond
  /// MaxChannels are left unprocessed (the engine never exceeds its scratch).
  void process(float* const* channels, int num_channels, int num_frames) noexcept {
    if (delay_q8_ == 0 || channels == nullptr || num_frames <= 0) {
      return;
    }
    const int n = std::min<int>(num_channels, static_cast<int>(MaxChannels));
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
    std::vector<float> buffer{0.0f};
    std::size_t write_index = 0;
  };
  std::array<rt::DelayLine, MaxChannels> lanes_{};
  std::array<FractionalLane, MaxChannels> fractional_lanes_{};
  int delay_q8_ = 0;
  bool fractional_ = false;
};

}  // namespace sonare::engine
