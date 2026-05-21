#pragma once

/// @file mono_compat_check.h
/// @brief Stereo mono compatibility analysis helper.

#include <cstddef>

namespace sonare::mastering::stereo {

struct MonoCompatResult {
  float correlation = 0.0f;
  float width = 0.0f;
  float mono_peak = 0.0f;
  float side_rms = 0.0f;
  bool likely_mono_compatible = true;
};

MonoCompatResult mono_compat_check(const float* left, const float* right, size_t length,
                                   float correlation_threshold = 0.0f);

}  // namespace sonare::mastering::stereo
