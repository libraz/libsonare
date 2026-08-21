#pragma once

/// @file clipping.h
/// @brief Sample clipping detection utilities.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::metering {

struct ClippingRegion {
  size_t start_sample = 0;
  size_t end_sample = 0;
  size_t length = 0;
  float peak = 0.0f;
};

struct ClippingResult {
  size_t clipped_samples = 0;
  float clipping_ratio = 0.0f;
  float max_clipped_peak = 0.0f;
  std::vector<ClippingRegion> regions;
};

/// Library default detection threshold: just below full scale, so the flattened
/// runs a converter produces are reported while ordinary peaks are not.
inline constexpr float kDefaultClippingThreshold = 0.999f;
/// Library default minimum run length: report every clipped run.
inline constexpr size_t kDefaultClippingMinRegionSamples = 1;

/// @brief Clipping detection parameters resolved against the library defaults.
struct ClippingParams {
  float threshold = kDefaultClippingThreshold;
  size_t min_region_samples = kDefaultClippingMinRegionSamples;
};

/// Decodes the public C/JS sentinel convention into validated parameters.
///
/// Exactly 0 selects the library default, for both fields. Every other value is
/// the caller's request, so a threshold outside the accepted [0, 1] domain is
/// rejected with InvalidParameter instead of being promoted to the default:
/// promoting it made a negative threshold indistinguishable from "use the
/// default" while an equally out-of-domain 1.5 was correctly refused.
ClippingParams clipping_params_from_public(float threshold, size_t min_region_samples);

ClippingResult detect_clipping(const Audio& audio, float threshold = kDefaultClippingThreshold,
                               size_t min_region_samples = kDefaultClippingMinRegionSamples);

}  // namespace sonare::metering
