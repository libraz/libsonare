#include <sonare/sonare_c.h>

#include <cstring>
#include <memory>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_c_internal.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

sonare::mastering::repair::DeclickConfig to_cpp_declick_config(const SonareDeclickConfig* config) {
  sonare::mastering::repair::DeclickConfig cpp;
  if (!config) return cpp;
  cpp.threshold = config->threshold;
  cpp.neighbor_ratio = config->neighbor_ratio;
  cpp.max_click_samples = config->max_click_samples;
  cpp.lpc_order = config->lpc_order;
  cpp.residual_ratio = config->residual_ratio;
  return cpp;
}

sonare::mastering::repair::DenoiseMode to_cpp_denoise_mode(int mode) {
  switch (mode) {
    case SONARE_DENOISE_MODE_MMSE_STSA:
      return sonare::mastering::repair::DenoiseMode::MmseStsa;
    case SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION:
      return sonare::mastering::repair::DenoiseMode::SpectralSubtraction;
    case SONARE_DENOISE_MODE_LOG_MMSE:
      return sonare::mastering::repair::DenoiseMode::LogMmse;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown denoise mode");
}

sonare::mastering::repair::DenoiseNoiseEstimator to_cpp_denoise_noise_estimator(int estimator) {
  switch (estimator) {
    case SONARE_DENOISE_NOISE_ESTIMATOR_MCRA:
      return sonare::mastering::repair::DenoiseNoiseEstimator::Mcra;
    case SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA:
      return sonare::mastering::repair::DenoiseNoiseEstimator::Imcra;
    case SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE:
      return sonare::mastering::repair::DenoiseNoiseEstimator::Quantile;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise noise estimator");
}

sonare::mastering::repair::DenoiseClassicalConfig to_cpp_denoise_config(
    const SonareDenoiseClassicalConfig* config) {
  sonare::mastering::repair::DenoiseClassicalConfig cpp;
  if (!config) return cpp;
  cpp.mode = to_cpp_denoise_mode(config->mode);
  cpp.noise_estimator = to_cpp_denoise_noise_estimator(config->noise_estimator);
  cpp.n_fft = config->n_fft;
  cpp.hop_length = config->hop_length;
  cpp.dd_alpha = config->dd_alpha;
  cpp.reduction_db = config->reduction_db;
  cpp.over_subtraction = config->over_subtraction;
  cpp.spectral_floor = config->spectral_floor;
  cpp.noise_estimation_quantile = config->noise_estimation_quantile;
  cpp.speech_presence_gain = config->speech_presence_gain != 0;
  cpp.gain_smoothing = config->gain_smoothing != 0;
  return cpp;
}

sonare::mastering::repair::DeclipConfig to_cpp_declip_config(const SonareDeclipConfig* config) {
  sonare::mastering::repair::DeclipConfig cpp;
  if (!config) return cpp;
  cpp.clip_threshold = config->clip_threshold;
  cpp.lpc_order = config->lpc_order;
  cpp.iterations = config->iterations;
  cpp.lpc_blend = config->lpc_blend;
  return cpp;
}

sonare::mastering::repair::DecrackleMode to_cpp_decrackle_mode(int mode) {
  switch (mode) {
    case SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE:
      return sonare::mastering::repair::DecrackleMode::WaveletShrinkage;
    case SONARE_DECRACKLE_MODE_MEDIAN:
      return sonare::mastering::repair::DecrackleMode::Median;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown decrackle mode");
}

sonare::mastering::repair::DecrackleConfig to_cpp_decrackle_config(
    const SonareDecrackleConfig* config) {
  sonare::mastering::repair::DecrackleConfig cpp;
  if (!config) return cpp;
  cpp.threshold = config->threshold;
  cpp.mode = to_cpp_decrackle_mode(config->mode);
  cpp.levels = config->levels;
  return cpp;
}

sonare::mastering::repair::DehumConfig to_cpp_dehum_config(const SonareDehumConfig* config) {
  sonare::mastering::repair::DehumConfig cpp;
  if (!config) return cpp;
  cpp.fundamental_hz = config->fundamental_hz;
  cpp.harmonics = config->harmonics;
  cpp.q = config->q;
  cpp.adaptive = config->adaptive != 0;
  cpp.search_range_hz = config->search_range_hz;
  cpp.adaptation = config->adaptation;
  cpp.frame_size = config->frame_size;
  cpp.pll_bandwidth = config->pll_bandwidth;
  return cpp;
}

sonare::mastering::repair::DereverbClassicalConfig to_cpp_dereverb_config(
    const SonareDereverbClassicalConfig* config) {
  sonare::mastering::repair::DereverbClassicalConfig cpp;
  if (!config) return cpp;
  cpp.threshold = config->threshold;
  cpp.attenuation = config->attenuation;
  cpp.n_fft = config->n_fft;
  cpp.hop_length = config->hop_length;
  cpp.t60_sec = config->t60_sec;
  cpp.late_delay_ms = config->late_delay_ms;
  cpp.over_subtraction = config->over_subtraction;
  cpp.spectral_floor = config->spectral_floor;
  cpp.wpe_enabled = config->wpe_enabled != 0;
  cpp.wpe_iterations = config->wpe_iterations;
  cpp.wpe_taps = config->wpe_taps;
  cpp.wpe_strength = config->wpe_strength;
  return cpp;
}

sonare::mastering::repair::TrimSilenceMode to_cpp_trim_silence_mode(int mode) {
  switch (mode) {
    case SONARE_TRIM_SILENCE_MODE_LUFS_GATED:
      return sonare::mastering::repair::TrimSilenceMode::LufsGated;
    case SONARE_TRIM_SILENCE_MODE_PEAK:
      return sonare::mastering::repair::TrimSilenceMode::Peak;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown trim silence mode");
}

sonare::mastering::repair::TrimSilenceConfig to_cpp_trim_silence_config(
    const SonareTrimSilenceConfig* config) {
  sonare::mastering::repair::TrimSilenceConfig cpp;
  if (!config) return cpp;
  cpp.threshold = config->threshold;
  cpp.padding_samples = config->padding_samples;
  cpp.mode = to_cpp_trim_silence_mode(config->mode);
  cpp.gate_lufs = config->gate_lufs;
  cpp.window_ms = config->window_ms;
  return cpp;
}

SonareDeclickReport to_c_declick_report(const sonare::mastering::repair::DeclickReport& cpp) {
  SonareDeclickReport c{};
  c.detected.count = cpp.detected.count;
  c.detected.rejected = cpp.detected.rejected;
  c.detected.longest_run_samples = cpp.detected.longest_run_samples;
  c.detected.per_second = cpp.detected.per_second;
  c.repaired_runs = cpp.repaired_runs;
  c.repaired_samples = cpp.repaired_samples;
  c.linked_runs = cpp.linked_runs;
  c.lpc_model_used = cpp.lpc_model_used ? 1 : 0;
  return c;
}

SonareDeclipReport to_c_declip_report(const sonare::mastering::repair::DeclipReport& cpp) {
  SonareDeclipReport c{};
  c.detected.sample_count = cpp.detected.sample_count;
  c.detected.sample_fraction = cpp.detected.sample_fraction;
  c.detected.run_count = cpp.detected.run_count;
  c.detected.longest_run_samples = cpp.detected.longest_run_samples;
  c.lpc_reconstructed_runs = cpp.lpc_reconstructed_runs;
  c.interpolated_runs = cpp.interpolated_runs;
  c.repaired_samples = cpp.repaired_samples;
  c.linked_runs = cpp.linked_runs;
  return c;
}

SonareDecrackleReport to_c_decrackle_report(const sonare::mastering::repair::DecrackleReport& cpp) {
  SonareDecrackleReport c{};
  c.detected.sample_count = cpp.detected.sample_count;
  c.detected.sample_fraction = cpp.detected.sample_fraction;
  c.detected.per_second = cpp.detected.per_second;
  c.replaced_samples = cpp.replaced_samples;
  c.detail_coefficients = cpp.detail_coefficients;
  c.shrunk_coefficients = cpp.shrunk_coefficients;
  c.noise_sigma = cpp.noise_sigma;
  return c;
}

SonareDehumReport to_c_dehum_report(const sonare::mastering::repair::DehumReport& cpp) {
  SonareDehumReport c{};
  c.detected.fundamental_hz = cpp.detected.fundamental_hz;
  c.detected.fundamental_prominence = cpp.detected.fundamental_prominence;
  c.detected.harmonics = cpp.detected.harmonics;
  static_assert(SONARE_DEHUM_MAX_HARMONICS == sonare::mastering::repair::kDehumMaxHarmonics,
                "C harmonic level array must match the core's harmonic bound");
  std::memcpy(c.detected.harmonic_dbfs, cpp.detected.harmonic_dbfs,
              sizeof(c.detected.harmonic_dbfs));
  c.notched_harmonics = cpp.notched_harmonics;
  c.applied_fundamental_hz = cpp.applied_fundamental_hz;
  c.fundamental_drift_hz = cpp.fundamental_drift_hz;
  return c;
}

SonareDenoiseReport to_c_denoise_report(const sonare::mastering::repair::DenoiseReport& cpp) {
  SonareDenoiseReport c{};
  c.detected.floor_dbfs = cpp.detected.floor_dbfs;
  static_assert(static_cast<std::size_t>(SONARE_REPAIR_NOISE_BAND_COUNT) ==
                    sonare::mastering::repair::kRepairNoiseBandCount,
                "C band level array must match the core's band count");
  std::memcpy(c.detected.band_floor_dbfs, cpp.detected.band_floor_dbfs,
              sizeof(c.detected.band_floor_dbfs));
  c.mean_reduction_db = cpp.mean_reduction_db;
  c.max_reduction_db = cpp.max_reduction_db;
  c.floor_limited_fraction = cpp.floor_limited_fraction;
  return c;
}

SonareDereverbReport to_c_dereverb_report(const sonare::mastering::repair::DereverbReport& cpp) {
  SonareDereverbReport c{};
  c.detected.late_decay_ratio_db = cpp.detected.late_decay_ratio_db;
  c.detected.late_predictability = cpp.detected.late_predictability;
  c.mean_reduction_db = cpp.mean_reduction_db;
  c.suppressed_fraction = cpp.suppressed_fraction;
  c.wpe_predictor_norm = cpp.wpe_predictor_norm;
  return c;
}

SonareTrimRange to_c_trim_range(const sonare::mastering::repair::TrimRange& cpp) {
  SonareTrimRange c{};
  c.first = cpp.first;
  c.last_exclusive = cpp.last_exclusive;
  return c;
}

SonareTrimReport to_c_trim_report(const sonare::mastering::repair::TrimReport& cpp) {
  SonareTrimReport c{};
  c.range = to_c_trim_range(cpp.range);
  c.removed_head_samples = cpp.removed_head_samples;
  c.removed_tail_samples = cpp.removed_tail_samples;
  return c;
}

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

void clear_float_output(float** out, size_t* out_length) {
  *out = nullptr;
  *out_length = 0;
}

}  // namespace

SonareError sonare_mastering_repair_declick(const float* samples, size_t length, int sample_rate,
                                            const SonareDeclickConfig* config, float** out,
                                            size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result = sonare::mastering::repair::declick(audio, to_cpp_declick_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_declick_stereo(const float* left, const float* right,
                                                   size_t length, int sample_rate,
                                                   const SonareDeclickConfig* config,
                                                   SonareDeclickStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareDeclickStereoResult{};

  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::declick_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_declick_config(config));
  out->length = result.left.size();
  out->left_report = to_c_declick_report(result.left_report);
  out->right_report = to_c_declick_report(result.right_report);
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_denoise_classical(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareDenoiseClassicalConfig* config,
                                                      float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  if (config) {
    if (!is_power_of_two(config->n_fft)) return SONARE_ERROR_INVALID_PARAMETER;
    if (config->hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result =
        sonare::mastering::repair::denoise_classical(audio, to_cpp_denoise_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_denoise_classical_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareDenoiseClassicalConfig* config, SonareDenoiseStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareDenoiseStereoResult{};

  // Mirrors the mono entry's pre-check so the two agree on which configs they
  // reject before the core ever sees them.
  if (config) {
    if (!is_power_of_two(config->n_fft)) return SONARE_ERROR_INVALID_PARAMETER;
    if (config->hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::denoise_classical_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_denoise_config(config));
  out->length = result.left.size();
  out->report = to_c_denoise_report(result.report);
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_declip(const float* samples, size_t length, int sample_rate,
                                           const SonareDeclipConfig* config, float** out,
                                           size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result = sonare::mastering::repair::declip(audio, to_cpp_declip_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_declip_stereo(const float* left, const float* right,
                                                  size_t length, int sample_rate,
                                                  const SonareDeclipConfig* config,
                                                  SonareDeclipStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareDeclipStereoResult{};

  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::declip_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_declip_config(config));
  out->length = result.left.size();
  out->left_report = to_c_declip_report(result.left_report);
  out->right_report = to_c_declip_report(result.right_report);
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_decrackle(const float* samples, size_t length, int sample_rate,
                                              const SonareDecrackleConfig* config, float** out,
                                              size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result = sonare::mastering::repair::decrackle(audio, to_cpp_decrackle_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_decrackle_stereo(const float* left, const float* right,
                                                     size_t length, int sample_rate,
                                                     const SonareDecrackleConfig* config,
                                                     SonareDecrackleStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareDecrackleStereoResult{};

  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::decrackle_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_decrackle_config(config));
  out->length = result.left.size();
  out->left_report = to_c_decrackle_report(result.left_report);
  out->right_report = to_c_decrackle_report(result.right_report);
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_dehum(const float* samples, size_t length, int sample_rate,
                                          const SonareDehumConfig* config, float** out,
                                          size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result = sonare::mastering::repair::dehum(audio, to_cpp_dehum_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_dehum_stereo(const float* left, const float* right,
                                                 size_t length, int sample_rate,
                                                 const SonareDehumConfig* config,
                                                 SonareDehumStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareDehumStereoResult{};

  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::dehum_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_dehum_config(config));
  out->length = result.left.size();
  out->left_report = to_c_dehum_report(result.left_report);
  out->right_report = to_c_dehum_report(result.right_report);
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_dereverb_classical(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDereverbClassicalConfig* config,
                                                       float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  if (config) {
    if (!is_power_of_two(config->n_fft)) return SONARE_ERROR_INVALID_PARAMETER;
    if (config->hop_length <= 0 || config->hop_length > config->n_fft) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result =
        sonare::mastering::repair::dereverb_classical(audio, to_cpp_dereverb_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_dereverb_classical_stereo(
    const float* left, const float* right, size_t length, int sample_rate,
    const SonareDereverbClassicalConfig* config, SonareDereverbStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareDereverbStereoResult{};

  // Mirrors the mono entry's pre-check, which bounds hop_length by n_fft where
  // the denoise pair only requires it positive.
  if (config) {
    if (!is_power_of_two(config->n_fft)) return SONARE_ERROR_INVALID_PARAMETER;
    if (config->hop_length <= 0 || config->hop_length > config->n_fft) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::dereverb_classical_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_dereverb_config(config));
  out->length = result.left.size();
  out->report = to_c_dereverb_report(result.report);
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_trim_silence(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareTrimSilenceConfig* config, float** out,
                                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    Audio result =
        sonare::mastering::repair::trim_silence(audio, to_cpp_trim_silence_config(config));
    return copy_audio_result(result, out, out_length);
  });
}

SonareError sonare_mastering_repair_trim_silence_stereo(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareTrimSilenceConfig* config,
                                                        SonareTrimSilenceStereoResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = SonareTrimSilenceStereoResult{};

  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto result = sonare::mastering::repair::trim_silence_stereo(
      Audio::from_buffer(left, length, sample_rate), Audio::from_buffer(right, length, sample_rate),
      to_cpp_trim_silence_config(config));
  out->report = to_c_trim_report(result.report);
  out->left_range = to_c_trim_range(result.left_range);
  out->right_range = to_c_trim_range(result.right_range);
  out->length = result.left.size();
  // A trimmed pair can come back empty, which no other repair stereo entry can
  // produce. Hand back (NULL, 0) rather than a zero-length allocation, matching
  // the empty-result policy the mono entries take through copy_audio_result.
  if (out->length == 0) return SONARE_OK;
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_dereverb_apply_room_estimate(
    const SonareRoomEstimate* estimate, SonareDereverbClassicalConfig* config) {
  SONARE_C_API_ENTRY;
  if (!estimate || !config) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<float> bands;
  if (estimate->rt60_bands != nullptr && estimate->band_count > 0) {
    bands.assign(estimate->rt60_bands, estimate->rt60_bands + estimate->band_count);
  }
  // Round-trips through the C++ config so the mapping lives in one place; only
  // the two fields a measurement determines are written back.
  sonare::mastering::repair::DereverbClassicalConfig cpp = to_cpp_dereverb_config(config);
  sonare::mastering::repair::apply_room_measurement(cpp, sonare::mid_frequency_rt60(bands),
                                                    estimate->volume);
  config->t60_sec = cpp.t60_sec;
  config->late_delay_ms = cpp.late_delay_ms;
  return SONARE_OK;
  SONARE_C_CATCH
}
