#pragma once

/// @file clipping.h
/// @brief Sample clipping detection utilities.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::analysis::meter {

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

ClippingResult detect_clipping(const Audio& audio, float threshold = 0.999f,
                               size_t min_region_samples = 1);

}  // namespace sonare::analysis::meter
