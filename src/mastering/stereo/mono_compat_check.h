#pragma once

/// @file mono_compat_check.h
/// @brief Stereo mono compatibility analysis helper.

#include <cstddef>
#include <vector>

namespace sonare::mastering::stereo {

struct MonoCompatResult {
  float correlation = 0.0f;
  float width = 0.0f;
  float mono_peak = 0.0f;
  float side_rms = 0.0f;
  bool likely_mono_compatible = true;
};

struct MonoCompatBandResult {
  float low_hz = 0.0f;
  float high_hz = 0.0f;
  float correlation = 0.0f;
  float side_rms = 0.0f;
};

MonoCompatResult mono_compat_check(const float* left, const float* right, size_t length,
                                   float correlation_threshold = 0.0f);
std::vector<MonoCompatBandResult> mono_compat_check_log_bands(const float* left, const float* right,
                                                              size_t length, double sample_rate,
                                                              int bands_per_octave = 3,
                                                              float low_hz = 20.0f,
                                                              float high_hz = 20000.0f);

}  // namespace sonare::mastering::stereo
