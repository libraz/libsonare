#pragma once

/// @file dynamic_range.h
/// @brief Offline dynamic range meter.

#include <vector>

#include "core/audio.h"

namespace sonare::analysis::meter {

struct DynamicRangeResult {
  float dynamic_range_db = 0.0f;
  float low_percentile_db = 0.0f;
  float high_percentile_db = 0.0f;
  std::vector<float> window_rms_db;
};

struct DynamicRangeConfig {
  float window_sec = 3.0f;
  float hop_sec = 1.0f;
  float low_percentile = 0.10f;
  float high_percentile = 0.95f;
  float floor_db = -120.0f;
};

DynamicRangeResult dynamic_range(const Audio& audio, const DynamicRangeConfig& config = {});

}  // namespace sonare::analysis::meter
