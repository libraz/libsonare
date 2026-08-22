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

/// @brief One log-spaced band's stereo readout.
///
/// `correlation` and `side_rms` are measured OVER the interval
/// [low_hz, high_hz), not at a single representative frequency inside it, so two
/// components sharing a band both contribute: an in-phase and an anti-phase
/// partial of equal energy in one band read as an uncorrelated band, which is
/// what they sum to in mono.
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
