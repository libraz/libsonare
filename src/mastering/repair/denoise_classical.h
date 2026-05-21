#pragma once

/// @file denoise_classical.h
/// @brief Classical (non-ML) noise reduction via STFT spectral subtraction.

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief Spectral-subtraction denoiser based on Berouti's over-subtraction.
///
/// The noise spectrum is estimated from the quietest frames of the input
/// (`noise_estimation_quantile` fraction). For each frame:
///   clean_psd[k] = max(input_psd[k] - alpha * noise_psd[k], beta * noise_psd[k])
/// The magnitude is replaced with sqrt(clean_psd) while the phase is preserved.
struct DenoiseClassicalConfig {
  int n_fft = 1024;
  int hop_length = 256;
  /// Over-subtraction factor (Berouti's alpha). 1.0 = textbook spectral subtraction;
  /// >1 (e.g. 2.0) reduces musical noise at the cost of slight signal attenuation.
  float over_subtraction = 2.0f;
  /// Spectral floor (Berouti's beta) preventing negative spectra; relative to noise PSD.
  float spectral_floor = 0.05f;
  /// Fraction of frames assumed to be noise-only when estimating the noise spectrum.
  /// 0.1 means the quietest 10% of frames contribute to the noise estimate.
  float noise_estimation_quantile = 0.1f;
};

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config = {});

}  // namespace sonare::mastering::repair
