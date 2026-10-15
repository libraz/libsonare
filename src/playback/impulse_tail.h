#pragma once

/// @file impulse_tail.h
/// @brief Decay horizons measured on a prepared filter's unit-impulse response.
/// @details A playback stage reports how long its output keeps sounding once the input
///   stops. Where the stage's memory is a filter whose ringing has no closed form, the
///   horizon is read off a simulated unit-impulse response and ends where that response
///   stays below @ref rt::kTailFloor, the floor every tail_samples() uses.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "rt/tail_budget.h"

namespace sonare::playback {

/// Longest unit-impulse response simulated when measuring a decay, in seconds.
inline constexpr double kImpulseProbeSeconds = 2.0;

/// Frames after which @p response stays at or below rt::kTailFloor (0 if it never exceeds it).
inline int impulse_tail_frames(const std::vector<float>& response) noexcept {
  for (size_t i = response.size(); i > 0; --i) {
    if (std::abs(static_cast<double>(response[i - 1])) > rt::kTailFloor) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

/// Lowest band of a crossover built from @p config, driven by a unit impulse for
/// kImpulseProbeSeconds in blocks of @p max_block.
inline std::vector<float> crossover_low_band_impulse(
    const mastering::multiband::CrossoverConfig& config, double sample_rate, int max_block) {
  mastering::multiband::Crossover probe(config);
  probe.prepare(sample_rate, max_block, 1);
  mastering::multiband::CrossoverScratch scratch;
  probe.prepare_scratch(scratch, 1, max_block);
  const auto length = static_cast<size_t>(kImpulseProbeSeconds * sample_rate);
  const auto block = static_cast<size_t>(std::max(1, max_block));
  std::vector<float> input(length, 0.0f);
  if (!input.empty()) input[0] = 1.0f;
  std::vector<float> response(length, 0.0f);
  for (size_t start = 0; start < length; start += block) {
    const int count = static_cast<int>(std::min(block, length - start));
    float* ptr = input.data() + start;
    probe.split_into(&ptr, 1, count, scratch);
    std::copy(scratch.band_channels[0][0], scratch.band_channels[0][0] + count,
              response.begin() + static_cast<std::ptrdiff_t>(start));
  }
  return response;
}

}  // namespace sonare::playback
