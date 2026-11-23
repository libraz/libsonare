#pragma once

/// @file reference_spectrum.h
/// @brief Long-term reference spectrum extraction helpers.

#include <type_traits>
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
ReferenceSpectrum reference_spectrum(const Audio& left, const Audio& right,
                                     const ReferenceSpectrumConfig& config);

// Deducing the right channel keeps reference_spectrum(audio, {}) a mono call.
template <typename Right, std::enable_if_t<std::is_same_v<Right, Audio>, int> = 0>
ReferenceSpectrum reference_spectrum(const Audio& left, const Right& right) {
  return reference_spectrum(left, right, {});
}

}  // namespace sonare::mastering::match
