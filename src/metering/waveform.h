#pragma once

/// @file waveform.h
/// @brief Display-oriented waveform peak buckets.

#include <cstddef>
#include <vector>

namespace sonare::metering {

struct WaveformPeaksResult {
  std::vector<float> min;
  std::vector<float> max;
  int channels = 0;
  size_t bucket_count = 0;
  size_t samples_per_bucket = 0;
};

/// @brief Compute per-channel min/max buckets from an interleaved buffer.
/// @details `samples` holds `frames * channels` values in channel-interleaved
///          order. The result arrays are channel-major:
///          `channel * bucket_count + bucket`.
WaveformPeaksResult waveform_peaks(const float* samples, size_t frames, int channels,
                                   size_t samples_per_bucket);

/// @brief Build multiple waveform peak levels for UI zoom levels.
/// @details Each entry in `samples_per_bucket_levels` is computed independently
///          using @ref waveform_peaks and returned in the same order.
std::vector<WaveformPeaksResult> waveform_peak_pyramid(
    const float* samples, size_t frames, int channels,
    const std::vector<size_t>& samples_per_bucket_levels);

}  // namespace sonare::metering
