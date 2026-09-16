#pragma once

/// @file denoise_classical.h
/// @brief Classical (non-ML) STFT-domain noise reduction.

#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "mastering/common/noise_profile.h"

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
  /// MMSE noise-power estimation weighted by speech-presence probability
  /// (Gerkmann-Hendriks 2012). Tracks no spectral minimum, so it carries neither
  /// a window length nor the bias compensation the two MCRA variants need.
  Spp,
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
  /// Deepest attenuation the gain mask may apply to any bin, in dB (>= 0). Acts
  /// as a residual-noise floor: at 26 dB the noise is left 26 dB down rather
  /// than removed, which is what keeps a denoised result from sounding gated.
  float reduction_db = 26.0f;
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

/// Validates every public DenoiseClassicalConfig field. Named mono/linked
/// dispatch, the detector and the direct DSP entrypoints share this oracle so
/// enum/range handling cannot drift between surfaces.
void validate_config(const DenoiseClassicalConfig& config);

/// @brief Bands the noise floor's shape is reported in.
/// @details The same 32-band geometric grid from 20 Hz to Nyquist the mastering
/// report's band_energy_delta_db uses, so a noise floor and a tonal-balance
/// change can be read on one axis.
using common::kRepairNoiseBandCount;

/// @brief What a denoise analysis found in the input.
struct NoiseDetection {
  float floor_dbfs = 0.0f;  ///< Broadband estimated noise floor.
  float band_floor_dbfs[kRepairNoiseBandCount] = {};
};

/// @brief What a denoise pass found and what it removed.
struct DenoiseReport {
  NoiseDetection detected;
  float mean_reduction_db = 0.0f;  ///< Mean attenuation the gain mask applied.
  /// Deepest attenuation any cell of the mask applied. Saturating at
  /// `reduction_db` says the floor, not the estimator, set the depth.
  float max_reduction_db = 0.0f;
  /// Fraction of mask cells sitting on the floor `reduction_db` sets. Separates
  /// a floor that never bound from one that bound everywhere, which
  /// mean_reduction_db cannot.
  /// Always 0 for SpectralSubtraction, whose floor is spectral_floor instead.
  float floor_limited_fraction = 0.0f;
};

/// @brief Measures the noise floor without denoising.
/// @details Runs the STFT and the configured noise estimator -- the same two
///   stages the repair runs -- and stops before the gain mask, which is why the
///   mean attenuation lives on @ref DenoiseReport rather than here.
///
///   The broadband and per-band levels are the estimator's own PSD smoothed
///   across bins and referred to the input's measured mean square; see
///   @ref common::noise_floor_dbfs for why neither step assumes a window
///   constant and why the smoothing is there.
/// @param samples Input samples; must hold at least `config.n_fft` of them.
/// @param size Number of samples.
/// @param sample_rate Sample rate in Hz; must be positive.
/// @param config Analysis configuration.
/// @throws SonareException(InvalidParameter) for a rejected config, a
///         non-positive sample rate, no samples, or fewer than `n_fft` samples.
NoiseDetection detect_noise_floor(const float* samples, std::size_t size, int sample_rate,
                                  const DenoiseClassicalConfig& config = {});

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config = {});

/// @brief Denoises @p audio and reports what the pass found and did.
/// @details @p report may be null, which skips the report and nothing else: the
///   samples are the two-argument overload's, bit for bit.
Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config,
                        DenoiseReport* report);

/// @brief Denoises a channel set with one channel-linked gain mask.
/// @details The mask is built from the channel-summed power and applied
///   unchanged to every channel, so the processing cannot move an interchannel
///   level or phase difference. Fed one channel it reproduces
///   @ref denoise_classical bit for bit.
/// @param channels Input channels; all the same length and sample rate.
/// @param channel_count Number of entries in @p channels; at least one.
/// @param out Receives one output per channel. Must not be null.
/// @param config Analysis configuration.
/// @throws SonareException(InvalidParameter) for a rejected config, a null
///         output, no channels, or channels that disagree on length or rate.
DenoiseReport denoise_classical_linked(const Audio* const* channels, std::size_t channel_count,
                                       std::vector<Audio>* out,
                                       const DenoiseClassicalConfig& config = {});

/// @brief A denoised channel pair and the mask that produced it.
struct DenoiseStereoResult {
  Audio left;
  Audio right;
  /// One report rather than one per channel: the linked mask is a single array
  /// applied to both, so a per-channel pair would be two copies of one
  /// measurement and would read as though the two could differ.
  DenoiseReport report;
};

/// @brief Two-channel @ref denoise_classical_linked.
DenoiseStereoResult denoise_classical_stereo(const Audio& left, const Audio& right,
                                             const DenoiseClassicalConfig& config = {});

}  // namespace sonare::mastering::repair
