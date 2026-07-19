#pragma once

/// @file dynamic_range.h
/// @brief Offline dynamic range meter.

#include <vector>

#include "core/audio.h"
#include "util/constants.h"

namespace sonare::metering {

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
  float floor_db = sonare::constants::kFloorDb;
};

/// Decodes the public C/JS sentinel convention into a validated core config.
/// Zero window/hop and negative percentiles retain library defaults. All
/// supplied scalar values must be finite; percentiles must resolve into [0,1]
/// and remain ordered.
DynamicRangeConfig dynamic_range_config_from_public(float window_sec, float hop_sec,
                                                    float low_percentile, float high_percentile);

void validate_dynamic_range_config(const DynamicRangeConfig& config);

DynamicRangeResult dynamic_range(const Audio& audio, const DynamicRangeConfig& config = {});

}  // namespace sonare::metering
