#pragma once

/// @file spectrum.h
/// @brief Meter-oriented spectrum views.

#include <vector>

#include "core/audio.h"

namespace sonare::metering {

struct SpectrumConfig {
  int n_fft = 2048;
  bool apply_octave_smoothing = false;
  int octave_fraction = 3;
  float db_ref = 1.0f;
  float db_amin = 1e-10f;
};

struct SpectrumResult {
  std::vector<float> frequencies;
  std::vector<float> magnitude;
  std::vector<float> power;
  std::vector<float> db;
  int n_fft = 0;
  int sample_rate = 0;
};

SpectrumResult spectrum(const Audio& audio, const SpectrumConfig& config = {});
std::vector<float> smooth_fractional_octave(const std::vector<float>& values,
                                            const std::vector<float>& frequencies,
                                            int octave_fraction = 3);

}  // namespace sonare::metering
