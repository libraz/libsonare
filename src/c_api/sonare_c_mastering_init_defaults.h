#pragma once

#include <sonare/sonare_c.h>

#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "util/db.h"

namespace sonare_c_mastering_defaults {

inline int detector_mode_to_c(sonare::mastering::dynamics::DetectorMode mode) noexcept {
  switch (mode) {
    case sonare::mastering::dynamics::DetectorMode::Peak:
      return SONARE_COMPRESSOR_DETECTOR_PEAK;
    case sonare::mastering::dynamics::DetectorMode::Rms:
      return SONARE_COMPRESSOR_DETECTOR_RMS;
    case sonare::mastering::dynamics::DetectorMode::LogRms:
      return SONARE_COMPRESSOR_DETECTOR_LOG_RMS;
  }
  return SONARE_COMPRESSOR_DETECTOR_RMS;
}

inline int denoise_mode_to_c(sonare::mastering::repair::DenoiseMode mode) noexcept {
  switch (mode) {
    case sonare::mastering::repair::DenoiseMode::MmseStsa:
      return SONARE_DENOISE_MODE_MMSE_STSA;
    case sonare::mastering::repair::DenoiseMode::SpectralSubtraction:
      return SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION;
    case sonare::mastering::repair::DenoiseMode::LogMmse:
      return SONARE_DENOISE_MODE_LOG_MMSE;
  }
  return SONARE_DENOISE_MODE_LOG_MMSE;
}

inline int denoise_noise_estimator_to_c(
    sonare::mastering::repair::DenoiseNoiseEstimator estimator) noexcept {
  switch (estimator) {
    case sonare::mastering::repair::DenoiseNoiseEstimator::Mcra:
      return SONARE_DENOISE_NOISE_ESTIMATOR_MCRA;
    case sonare::mastering::repair::DenoiseNoiseEstimator::Imcra:
      return SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA;
    case sonare::mastering::repair::DenoiseNoiseEstimator::Quantile:
      return SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    case sonare::mastering::repair::DenoiseNoiseEstimator::Spp:
      return SONARE_DENOISE_NOISE_ESTIMATOR_SPP;
  }
  return SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
}

inline int decrackle_mode_to_c(sonare::mastering::repair::DecrackleMode mode) noexcept {
  return mode == sonare::mastering::repair::DecrackleMode::WaveletShrinkage
             ? SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE
             : SONARE_DECRACKLE_MODE_MEDIAN;
}

inline int dehum_mode_to_c(sonare::mastering::repair::DehumMode mode) noexcept {
  return mode == sonare::mastering::repair::DehumMode::Notch ? SONARE_DEHUM_MODE_NOTCH
                                                             : SONARE_DEHUM_MODE_SUBTRACT;
}

inline int trim_silence_mode_to_c(sonare::mastering::repair::TrimSilenceMode mode) noexcept {
  return mode == sonare::mastering::repair::TrimSilenceMode::LufsGated
             ? SONARE_TRIM_SILENCE_MODE_LUFS_GATED
             : SONARE_TRIM_SILENCE_MODE_PEAK;
}

inline void initialize(SonareMasteringConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::maximizer::LoudnessOptimizeConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->target_lufs = defaults.target_lufs;
  config->ceiling_dbtp = defaults.ceiling_dbtp;
  config->true_peak_oversample = defaults.true_peak_oversample;
  config->release_ms = defaults.release_ms;
  config->apply_gain_at_input_rate = defaults.apply_gain_at_input_rate ? 1 : 0;
}

inline void initialize(SonareCompressorConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::dynamics::CompressorConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold_db = defaults.threshold_db;
  config->ratio = defaults.ratio;
  config->attack_ms = defaults.attack_ms;
  config->release_ms = defaults.release_ms;
  config->knee_db = defaults.knee_db;
  config->makeup_gain_db = defaults.makeup_gain_db;
  config->auto_makeup = defaults.auto_makeup ? 1 : 0;
  config->detector = detector_mode_to_c(defaults.detector);
  config->sidechain_high_pass_hz = defaults.sidechain_high_pass_hz;
  config->pdr_time_ms = defaults.pdr_time_ms;
  config->pdr_release_scale = defaults.pdr_release_scale;
}

inline void initialize(SonareGateConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::dynamics::GateConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold_db = defaults.threshold_db;
  config->attack_ms = defaults.attack_ms;
  config->release_ms = defaults.release_ms;
  config->range_db = defaults.range_db;
  config->hold_ms = defaults.hold_ms;
  config->close_threshold_db = defaults.close_threshold_db;
  config->key_high_pass_hz = defaults.key_high_pass_hz;
}

inline void initialize(SonareTransientShaperConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::dynamics::TransientShaperConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->attack_gain_db = defaults.attack_gain_db;
  config->sustain_gain_db = defaults.sustain_gain_db;
  config->fast_attack_ms = defaults.fast_attack_ms;
  config->fast_release_ms = defaults.fast_release_ms;
  config->slow_attack_ms = defaults.slow_attack_ms;
  config->slow_release_ms = defaults.slow_release_ms;
  config->sensitivity = defaults.sensitivity;
  config->max_gain_db = defaults.max_gain_db;
  config->gain_smoothing_ms = defaults.gain_smoothing_ms;
  config->lookahead_ms = defaults.lookahead_ms;
}

inline void initialize(SonareDeclickConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::DeclickConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold = defaults.threshold;
  config->neighbor_ratio = defaults.neighbor_ratio;
  config->max_click_samples = defaults.max_click_samples;
  config->lpc_order = defaults.lpc_order;
  config->residual_ratio = defaults.residual_ratio;
}

inline void initialize(SonareDenoiseClassicalConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::DenoiseClassicalConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->mode = denoise_mode_to_c(defaults.mode);
  config->noise_estimator = denoise_noise_estimator_to_c(defaults.noise_estimator);
  config->n_fft = defaults.n_fft;
  config->hop_length = static_cast<size_t>(defaults.hop_length);
  config->dd_alpha = defaults.dd_alpha;
  config->reduction_db = defaults.reduction_db;
  config->over_subtraction = defaults.over_subtraction;
  config->spectral_floor = defaults.spectral_floor;
  config->noise_estimation_quantile = defaults.noise_estimation_quantile;
  config->speech_presence_gain = defaults.speech_presence_gain ? 1 : 0;
  config->gain_smoothing = defaults.gain_smoothing ? 1 : 0;
}

inline void initialize(SonareDeclipConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::DeclipConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->clip_threshold_db = sonare::linear_to_db(defaults.clip_threshold);
  config->lpc_order = defaults.lpc_order;
  config->iterations = defaults.iterations;
}

inline void initialize(SonareDecrackleConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::DecrackleConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold = defaults.threshold;
  config->mode = decrackle_mode_to_c(defaults.mode);
  config->levels = defaults.levels;
}

inline void initialize(SonareDehumConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::DehumConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->fundamental_hz = defaults.fundamental_hz;
  config->harmonics = defaults.harmonics;
  config->q = defaults.q;
  config->adaptive = defaults.adaptive ? 1 : 0;
  config->search_range_hz = defaults.search_range_hz;
  config->adaptation = defaults.adaptation;
  config->frame_size = defaults.frame_size;
  config->pll_bandwidth = defaults.pll_bandwidth;
  config->mode = dehum_mode_to_c(defaults.mode);
}

inline void initialize(SonareDereverbClassicalConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::DereverbClassicalConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold = defaults.threshold;
  config->attenuation = defaults.attenuation;
  config->n_fft = defaults.n_fft;
  config->hop_length = static_cast<size_t>(defaults.hop_length);
  config->t60_sec = defaults.t60_sec;
  config->late_delay_ms = defaults.late_delay_ms;
  config->over_subtraction = defaults.over_subtraction;
  config->spectral_floor = defaults.spectral_floor;
  config->wpe_enabled = defaults.wpe_enabled ? 1 : 0;
  config->wpe_iterations = defaults.wpe_iterations;
  config->wpe_taps = defaults.wpe_taps;
  config->wpe_strength = defaults.wpe_strength;
}

inline void initialize(SonareTrimSilenceConfig* config) noexcept {
  if (config == nullptr) return;
  const sonare::mastering::repair::TrimSilenceConfig defaults{};
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold_db = sonare::linear_to_db(defaults.threshold);
  config->padding_samples = defaults.padding_samples;
  config->mode = trim_silence_mode_to_c(defaults.mode);
  config->gate_lufs = defaults.gate_lufs;
  config->window_ms = defaults.window_ms;
}

}  // namespace sonare_c_mastering_defaults
