#pragma once

/// @file denoise_classical.h
/// @brief Classical (non-ML) STFT-domain noise reduction.

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief Algorithm used by `denoise_classical`.
enum class DenoiseMode {
  /// Ephraim-Malah Log-Spectral Amplitude estimator (1985).
  /// Best perceptual quality; minimizes musical noise.
  LogMmse,
  /// Ephraim-Malah Short-Time Spectral Amplitude estimator (1984).
  /// Slightly cheaper than LogMmse, similar musical-noise resilience.
  MmseStsa,
  /// Berouti spectral subtraction with over-subtraction (1979).
  /// Cheapest option but prone to musical noise.
  SpectralSubtraction,
};

enum class DenoiseNoiseEstimator {
  /// Estimate one stationary noise spectrum from the quietest frames.
  Quantile,
  /// Minimum-controlled recursive averaging.
  Mcra,
  /// Improved MCRA with speech-presence probability gating.
  Imcra,
};

/// @brief STFT-based denoiser supporting three classical gain functions.
///
/// This module is intentionally limited to classical, non-ML noise reduction:
/// spectral subtraction, MMSE-STSA, and LogMMSE with explicit noise tracking.
/// It does not attempt source separation, spectral repair, or DNN restoration.
///
/// The noise PSD is estimated from the quietest `noise_estimation_quantile`
/// fraction of frames (typically 10%). The decision-directed a priori SNR is
/// then computed via the Ephraim-Malah recursion with smoothing factor
/// `dd_alpha` (0.98 is the literature standard).
struct DenoiseClassicalConfig {
  DenoiseMode mode = DenoiseMode::LogMmse;
  DenoiseNoiseEstimator noise_estimator = DenoiseNoiseEstimator::Quantile;
  int n_fft = 1024;
  int hop_length = 256;
  /// Decision-directed a priori SNR smoothing factor (Ephraim-Malah 1984).
  /// 0.98 is the literature default; higher values produce smoother gains but
  /// slower adaptation to changing noise conditions.
  float dd_alpha = 0.98f;
  /// Minimum gain (linear) applied to any bin. Acts as a residual-noise floor;
  /// e.g. 0.05 leaves -26 dB of residual noise, preventing complete silence.
  float gain_floor = 0.05f;
  /// Spectral-subtraction over-subtraction factor (Berouti's alpha).
  /// Only used when `mode == SpectralSubtraction`.
  float over_subtraction = 2.0f;
  /// Spectral-subtraction floor multiplier (Berouti's beta), relative to noise
  /// PSD. Only used when `mode == SpectralSubtraction`.
  float spectral_floor = 0.05f;
  /// Fraction of frames assumed to be noise-only when estimating the noise
  /// spectrum. 0.1 means the quietest 10% of frames contribute.
  float noise_estimation_quantile = 0.1f;
  bool speech_presence_gain = true;
  bool gain_smoothing = true;
};

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config = {});

}  // namespace sonare::mastering::repair
