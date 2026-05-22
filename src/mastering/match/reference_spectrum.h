#pragma once

/// @file reference_spectrum.h
/// @brief Long-term reference spectrum extraction helpers.

#include <vector>

#include "core/audio.h"

namespace sonare::mastering::match {

struct ReferenceSpectrumConfig {
  int n_fft = 2048;
  int hop_length = 512;
  bool apply_octave_smoothing = true;
  int octave_fraction = 3;
};

struct ReferenceSpectrum {
  std::vector<float> frequencies;
  std::vector<float> db;
  int sample_rate = 0;
};

ReferenceSpectrum reference_spectrum(const Audio& audio,
                                     const ReferenceSpectrumConfig& config = {});

}  // namespace sonare::mastering::match
