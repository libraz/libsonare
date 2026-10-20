#pragma once

/// @file dynamic_detector.h
/// @brief Shared detector primitives for the dynamic-EQ processors.

#include <algorithm>
#include <cmath>

#include "util/db.h"

namespace sonare::mastering::eq {

/// Maximum per-band detector delay the detector rings are preallocated for. A
/// delay past this bound saturates so the audio-thread path never resizes.
inline constexpr float kMaxDetectorDelayMs = 20.0f;

/// @brief Broadband RMS level of a block, in dB.
/// @details Root-mean-square over all channels and samples, converted to dB.
///          The accumulation is double-precision and the sample count is floored
///          at 1 to avoid a divide-by-zero on an empty block. Shared by the
///          static parametric EQ (dynamic bands) and the dedicated dynamic EQ so
///          their broadband detector reads identically. Only the first
///          @p readable_samples positions are read (negative = all); the rest of
///          the block counts as silence, so a short external key is zero-padded.
inline float broadband_detector_db(const float* const* channels, int num_channels, int num_samples,
                                   int excluded_channel, int readable_samples = -1) {
  const int readable =
      (readable_samples < 0 || readable_samples > num_samples) ? num_samples : readable_samples;
  double sum = 0.0;
  int counted = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (ch == excluded_channel) continue;
    ++counted;
    for (int i = 0; i < readable; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
    }
  }
  const double count = static_cast<double>(counted) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

/// @brief Downward-compression gain delta (dB) for a band above its threshold.
/// @details Applies the compression curve to the detector level once the caller
///          has confirmed the band is active and @p detector_db exceeds
///          @p threshold_db: the over-threshold excess is scaled by
///          @c (1 - 1/ratio), clamped to @c |range_db|, and signed by
///          @p range_db (a negative range cuts, a positive range boosts). The
///          caller supplies the band-specific enable/threshold guard.
inline float dynamic_compression_delta(float detector_db, float threshold_db, float ratio,
                                       float range_db) {
  const float over_db = detector_db - threshold_db;
  const float compressed_db = over_db * (1.0f - 1.0f / ratio);
  const float range = std::abs(range_db);
  const float amount = std::min(range, compressed_db);
  return range_db < 0.0f ? -amount : amount;
}

}  // namespace sonare::mastering::eq
