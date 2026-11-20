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

/// @brief The linear detection threshold for a caller's level in dBFS.
/// @details NaN means not given and resolves to @ref kDefaultClippingThreshold; every other value
///          must be finite and in [-120, 0] dBFS; a level below the numerical floor would read
///          every sample as clipped.
/// @throws SonareException (InvalidParameter) naming "threshold_db".
float clipping_threshold_from_db(float threshold_db);

ClippingResult detect_clipping(const Audio& audio, float threshold = kDefaultClippingThreshold,
                               size_t min_region_samples = kDefaultClippingMinRegionSamples);

}  // namespace sonare::metering
