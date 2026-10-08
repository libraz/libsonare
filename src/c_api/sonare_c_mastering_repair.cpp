#include <sonare/sonare_c.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"
#include "mastering/assistant/repair_session.h"
#include "mastering/common/noise_profile.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare_c_detail;
using sonare_c_mastering_detail::copy_stereo_channels;
using sonare_c_mastering_detail::validate_stereo_audio_params;

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
    case SONARE_DENOISE_NOISE_ESTIMATOR_SPP:
      return sonare::mastering::repair::DenoiseNoiseEstimator::Spp;
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

sonare::mastering::repair::DehumMode to_cpp_dehum_mode(int mode) {
  switch (mode) {
    case SONARE_DEHUM_MODE_SUBTRACT:
      return sonare::mastering::repair::DehumMode::Subtract;
    case SONARE_DEHUM_MODE_NOTCH:
      return sonare::mastering::repair::DehumMode::Notch;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown dehum mode");
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
  cpp.mode = to_cpp_dehum_mode(config->mode);
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

SonareClickDetection to_c_click_detection(const sonare::mastering::repair::ClickDetection& cpp) {
  SonareClickDetection c{};
  c.count = cpp.count;
  c.rejected = cpp.rejected;
  c.longest_run_samples = cpp.longest_run_samples;
  c.per_second = cpp.per_second;
  return c;
}

SonareDeclickReport to_c_declick_report(const sonare::mastering::repair::DeclickReport& cpp) {
  SonareDeclickReport c{};
  c.detected = to_c_click_detection(cpp.detected);
  c.repaired_runs = cpp.repaired_runs;
  c.repaired_samples = cpp.repaired_samples;
  c.linked_runs = cpp.linked_runs;
  c.lpc_model_used = cpp.lpc_model_used ? 1 : 0;
  return c;
}

SonareClipDetection to_c_clip_detection(const sonare::mastering::repair::ClipDetection& cpp) {
  SonareClipDetection c{};
  c.sample_count = cpp.sample_count;
  c.sample_fraction = cpp.sample_fraction;
  c.run_count = cpp.run_count;
  c.longest_run_samples = cpp.longest_run_samples;
  c.flat_run_count = cpp.flat_run_count;
  c.longest_flat_run_samples = cpp.longest_flat_run_samples;
  c.flat_sample_count = cpp.flat_sample_count;
  c.flat_level = cpp.flat_level;
  return c;
}

SonareDeclipReport to_c_declip_report(const sonare::mastering::repair::DeclipReport& cpp) {
  SonareDeclipReport c{};
  c.detected = to_c_clip_detection(cpp.detected);
  c.lpc_reconstructed_runs = cpp.lpc_reconstructed_runs;
  c.interpolated_runs = cpp.interpolated_runs;
  c.repaired_samples = cpp.repaired_samples;
  c.linked_runs = cpp.linked_runs;
  return c;
}

SonareCrackleDetection to_c_crackle_detection(
    const sonare::mastering::repair::CrackleDetection& cpp) {
  SonareCrackleDetection c{};
  c.sample_count = cpp.sample_count;
  c.sample_fraction = cpp.sample_fraction;
  c.per_second = cpp.per_second;
  return c;
}

SonareDecrackleReport to_c_decrackle_report(const sonare::mastering::repair::DecrackleReport& cpp) {
  SonareDecrackleReport c{};
  c.detected = to_c_crackle_detection(cpp.detected);
  c.replaced_samples = cpp.replaced_samples;
  c.detail_coefficients = cpp.detail_coefficients;
  c.shrunk_coefficients = cpp.shrunk_coefficients;
  c.noise_sigma = cpp.noise_sigma;
  return c;
}

SonareHumDetection to_c_hum_detection(const sonare::mastering::repair::HumDetection& cpp) {
  SonareHumDetection c{};
  c.fundamental_hz = cpp.fundamental_hz;
  c.fundamental_prominence = cpp.fundamental_prominence;
  c.harmonics = cpp.harmonics;
  static_assert(SONARE_DEHUM_MAX_HARMONICS == sonare::mastering::repair::kDehumMaxHarmonics,
                "C harmonic level array must match the core's harmonic bound");
  std::memcpy(c.harmonic_dbfs, cpp.harmonic_dbfs, sizeof(c.harmonic_dbfs));
  return c;
}

SonareDehumReport to_c_dehum_report(const sonare::mastering::repair::DehumReport& cpp) {
  SonareDehumReport c{};
  c.detected = to_c_hum_detection(cpp.detected);
  c.notched_harmonics = cpp.notched_harmonics;
  c.applied_fundamental_hz = cpp.applied_fundamental_hz;
  c.fundamental_drift_hz = cpp.fundamental_drift_hz;
  return c;
}

SonareNoiseDetection to_c_noise_detection(const sonare::mastering::repair::NoiseDetection& cpp) {
  SonareNoiseDetection c{};
  c.floor_dbfs = cpp.floor_dbfs;
  static_assert(static_cast<std::size_t>(SONARE_REPAIR_NOISE_BAND_COUNT) ==
                    sonare::mastering::repair::kRepairNoiseBandCount,
                "C band level array must match the core's band count");
  std::memcpy(c.band_floor_dbfs, cpp.band_floor_dbfs, sizeof(c.band_floor_dbfs));
  return c;
}

SonareDenoiseReport to_c_denoise_report(const sonare::mastering::repair::DenoiseReport& cpp) {
  SonareDenoiseReport c{};
  c.detected = to_c_noise_detection(cpp.detected);
  c.mean_reduction_db = cpp.mean_reduction_db;
  c.max_reduction_db = cpp.max_reduction_db;
  c.floor_limited_fraction = cpp.floor_limited_fraction;
  return c;
}

SonareReverbDetection to_c_reverb_detection(const sonare::mastering::repair::ReverbDetection& cpp) {
  SonareReverbDetection c{};
  c.late_decay_ratio_db = cpp.late_decay_ratio_db;
  c.late_predictability = cpp.late_predictability;
  return c;
}

SonareDereverbReport to_c_dereverb_report(const sonare::mastering::repair::DereverbReport& cpp) {
  SonareDereverbReport c{};
  c.detected = to_c_reverb_detection(cpp.detected);
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

/// Shared body of the detection entry points, which differ only in their config
/// type and the core call they wrap.
template <typename CDetection, typename Fn>
SonareError run_detection(const float* samples, size_t length, int sample_rate, CDetection* out,
                          Fn detect) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Cleared before any validation return, so a rejected call hands back a zero
  // detection rather than whatever the caller's stack slot held.
  *out = CDetection{};

  const SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  *out = detect();
  return SONARE_OK;
  SONARE_C_CATCH
}

/// Shared body of the channel-linked entry points, which differ only in their
/// report type and the core call they wrap.
template <typename CReport, typename Fn>
SonareError run_linked(const float* const* channels, size_t channel_count, size_t length,
                       int sample_rate, float* const* out_channels, CReport* out_report,
                       Fn process) {
  if (!out_report) return SONARE_ERROR_INVALID_PARAMETER;
  // Cleared before any validation return, so a rejected call hands back a zero
  // report rather than whatever the caller's stack slot held. The output planes
  // are the caller's and stay untouched until the core has produced samples.
  *out_report = CReport{};
  if (!channels || !out_channels || channel_count == 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  std::vector<Audio> inputs;
  // The API accepts arbitrary positive counts; reject only counts the containers
  // cannot represent before indexing caller pointers, preserving the C error
  // contract instead of exposing vector::length_error.
  if (channel_count > inputs.max_size()) return SONARE_ERROR_INVALID_PARAMETER;
  inputs.reserve(channel_count);
  for (size_t c = 0; c < channel_count; ++c) {
    if (!out_channels[c]) return SONARE_ERROR_INVALID_PARAMETER;
    const SonareError err = validate_audio_params(channels[c], length, sample_rate);
    if (err != SONARE_OK) return err;
    inputs.push_back(Audio::from_buffer(channels[c], length, sample_rate));
  }
  std::vector<const Audio*> pointers;
  if (channel_count > pointers.max_size()) return SONARE_ERROR_INVALID_PARAMETER;
  pointers.reserve(channel_count);
  for (const Audio& channel : inputs) pointers.push_back(&channel);

  std::vector<Audio> produced;
  *out_report = process(pointers.data(), &produced);
  if (produced.size() != channel_count) return SONARE_ERROR_INVALID_STATE;
  for (size_t c = 0; c < channel_count; ++c) {
    // The core resynthesizes every channel to the first one's input length, so
    // this agrees with `length` by construction; checking is what keeps the copy
    // from depending on that.
    if (produced[c].size() != length) return SONARE_ERROR_INVALID_STATE;
    std::memcpy(out_channels[c], produced[c].data(), length * sizeof(float));
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

/// Config pre-check shared by every denoise entry point (mono, stereo, linked).
bool denoise_config_valid(const SonareDenoiseClassicalConfig* config) {
  return !config || (is_power_of_two(config->n_fft) && config->hop_length > 0);
}

/// Config pre-check shared by the mono and stereo dereverb entry points.
bool dereverb_config_valid(const SonareDereverbClassicalConfig* config) {
  return !config || (is_power_of_two(config->n_fft) && config->hop_length > 0 &&
                     config->hop_length <= config->n_fft);
}

/// Shared body of the stereo repair entry points, which differ only in their
/// result type and the core call. @p process receives both channels and fills
/// @p out; @p params_ok is the caller's config pre-check, applied after the
/// result is cleared and before the audio is validated.
template <typename CResult, typename Fn>
SonareError run_stereo(bool params_ok, const float* left, const float* right, size_t length,
                       int sample_rate, CResult* out, Fn process) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined before any validation return, so a rejected call hands back an empty
  // result rather than whatever the caller's stack slot held.
  *out = CResult{};
  if (!params_ok) return SONARE_ERROR_INVALID_PARAMETER;

  const SonareError err = validate_stereo_audio_params(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  process(Audio::from_buffer(left, length, sample_rate),
          Audio::from_buffer(right, length, sample_rate));
  return SONARE_OK;
  SONARE_C_CATCH
}

template <typename CResult, typename Fn>
SonareError run_stereo(const float* left, const float* right, size_t length, int sample_rate,
                       CResult* out, Fn process) {
  return run_stereo(true, left, right, length, sample_rate, out, process);
}

/// Copy a stereo core result's channels into @p out and record the length.
template <typename StereoResult, typename CResult>
void fill_stereo_channels(const StereoResult& result, CResult* out) {
  out->length = result.left.size();
  copy_stereo_channels(result.left.data(), result.right.data(), out->length, &out->left,
                       &out->right);
}

}  // namespace

SonareError sonare_mastering_repair_declick(const float* samples, size_t length, int sample_rate,
                                            const SonareDeclickConfig* config, float** out,
                                            size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;

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
  return run_stereo(left, right, length, sample_rate, out, [&](const Audio& l, const Audio& r) {
    const auto result =
        sonare::mastering::repair::declick_stereo(l, r, to_cpp_declick_config(config));
    out->left_report = to_c_declick_report(result.left_report);
    out->right_report = to_c_declick_report(result.right_report);
    fill_stereo_channels(result, out);
  });
}

SonareError sonare_mastering_repair_detect_clicks(const float* samples, size_t length,
                                                  int sample_rate,
                                                  const SonareDeclickConfig* config,
                                                  SonareClickDetection* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_click_detection(sonare::mastering::repair::detect_clicks(
        samples, length, sample_rate, to_cpp_declick_config(config)));
  });
}

SonareError sonare_mastering_repair_denoise_classical(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareDenoiseClassicalConfig* config,
                                                      float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;
  if (!denoise_config_valid(config)) return SONARE_ERROR_INVALID_PARAMETER;

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
  // Mirrors the mono entry's pre-check so the two agree on which configs they
  // reject before the core ever sees them.
  return run_stereo(denoise_config_valid(config), left, right, length, sample_rate, out,
                    [&](const Audio& l, const Audio& r) {
                      const auto result = sonare::mastering::repair::denoise_classical_stereo(
                          l, r, to_cpp_denoise_config(config));
                      out->report = to_c_denoise_report(result.report);
                      fill_stereo_channels(result, out);
                    });
}

SonareError sonare_mastering_repair_denoise_classical_linked(
    const float* const* channels, size_t channel_count, size_t length, int sample_rate,
    const SonareDenoiseClassicalConfig* config, float* const* out_channels,
    SonareDenoiseReport* out_report) {
  SONARE_C_API_ENTRY;
  // Mirrors the mono and stereo entries' pre-check so the three agree on which
  // configs they reject before the core ever sees them.
  if (out_report) *out_report = {};
  if (!denoise_config_valid(config)) return SONARE_ERROR_INVALID_PARAMETER;

  return run_linked(
      channels, channel_count, length, sample_rate, out_channels, out_report,
      [&](const Audio* const* inputs, std::vector<Audio>* produced) {
        return to_c_denoise_report(sonare::mastering::repair::denoise_classical_linked(
            inputs, channel_count, produced, to_cpp_denoise_config(config)));
      });
}

SonareError sonare_mastering_repair_detect_noise_floor(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDenoiseClassicalConfig* config,
                                                       SonareNoiseDetection* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_noise_detection(sonare::mastering::repair::detect_noise_floor(
        samples, length, sample_rate, to_cpp_denoise_config(config)));
  });
}

SonareError sonare_mastering_repair_noise_band_bins(int n_fft, int sample_rate, int* out_bins) {
  SONARE_C_API_ENTRY;
  static_assert(SONARE_REPAIR_NOISE_BAND_EDGE_COUNT ==
                    static_cast<int>(sonare::mastering::common::kRepairNoiseBandCount) + 1,
                "C band edge array must match the core's band count");
  if (!out_bins) return SONARE_ERROR_INVALID_PARAMETER;
  // Zeroed before any validation return, so a caller that ignores the error code
  // reads empty bands rather than whatever its stack slot held.
  std::memset(out_bins, 0, SONARE_REPAIR_NOISE_BAND_EDGE_COUNT * sizeof(int));
  // The same power-of-two rule the denoise config validator applies, so a grid
  // this hands back is always one the detector can report on.
  if (!is_power_of_two(n_fft) || sample_rate <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  sonare::mastering::common::repair_noise_band_bins(n_fft, sample_rate, out_bins);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_declip(const float* samples, size_t length, int sample_rate,
                                           const SonareDeclipConfig* config, float** out,
                                           size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;

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
  return run_stereo(left, right, length, sample_rate, out, [&](const Audio& l, const Audio& r) {
    const auto result =
        sonare::mastering::repair::declip_stereo(l, r, to_cpp_declip_config(config));
    out->left_report = to_c_declip_report(result.left_report);
    out->right_report = to_c_declip_report(result.right_report);
    fill_stereo_channels(result, out);
  });
}

SonareError sonare_mastering_repair_detect_clipping(const float* samples, size_t length,
                                                    int sample_rate,
                                                    const SonareDeclipConfig* config,
                                                    SonareClipDetection* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_clip_detection(sonare::mastering::repair::detect_clipping(
        samples, length, sample_rate, to_cpp_declip_config(config)));
  });
}

SonareError sonare_mastering_repair_decrackle(const float* samples, size_t length, int sample_rate,
                                              const SonareDecrackleConfig* config, float** out,
                                              size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;

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
  return run_stereo(left, right, length, sample_rate, out, [&](const Audio& l, const Audio& r) {
    const auto result =
        sonare::mastering::repair::decrackle_stereo(l, r, to_cpp_decrackle_config(config));
    out->left_report = to_c_decrackle_report(result.left_report);
    out->right_report = to_c_decrackle_report(result.right_report);
    fill_stereo_channels(result, out);
  });
}

SonareError sonare_mastering_repair_detect_crackle(const float* samples, size_t length,
                                                   int sample_rate,
                                                   const SonareDecrackleConfig* config,
                                                   SonareCrackleDetection* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_crackle_detection(sonare::mastering::repair::detect_crackle(
        samples, length, sample_rate, to_cpp_decrackle_config(config)));
  });
}

SonareError sonare_mastering_repair_dehum(const float* samples, size_t length, int sample_rate,
                                          const SonareDehumConfig* config, float** out,
                                          size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;

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
  return run_stereo(left, right, length, sample_rate, out, [&](const Audio& l, const Audio& r) {
    const auto result = sonare::mastering::repair::dehum_stereo(l, r, to_cpp_dehum_config(config));
    out->left_report = to_c_dehum_report(result.left_report);
    out->right_report = to_c_dehum_report(result.right_report);
    fill_stereo_channels(result, out);
  });
}

SonareError sonare_mastering_repair_detect_hum(const float* samples, size_t length, int sample_rate,
                                               const SonareDehumConfig* config,
                                               SonareHumDetection* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_hum_detection(sonare::mastering::repair::detect_hum(samples, length, sample_rate,
                                                                    to_cpp_dehum_config(config)));
  });
}

SonareError sonare_mastering_repair_dereverb_classical(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDereverbClassicalConfig* config,
                                                       float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;
  if (!dereverb_config_valid(config)) return SONARE_ERROR_INVALID_PARAMETER;

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
  // Mirrors the mono entry's pre-check, which bounds hop_length by n_fft where
  // the denoise pair only requires it positive.
  return run_stereo(dereverb_config_valid(config), left, right, length, sample_rate, out,
                    [&](const Audio& l, const Audio& r) {
                      const auto result = sonare::mastering::repair::dereverb_classical_stereo(
                          l, r, to_cpp_dereverb_config(config));
                      out->report = to_c_dereverb_report(result.report);
                      fill_stereo_channels(result, out);
                    });
}

SonareError sonare_mastering_repair_dereverb_classical_linked(
    const float* const* channels, size_t channel_count, size_t length, int sample_rate,
    const SonareDereverbClassicalConfig* config, float* const* out_channels,
    SonareDereverbReport* out_report) {
  SONARE_C_API_ENTRY;
  // Mirrors the mono and stereo entries' pre-check so the three agree on which
  // configs they reject before the core ever sees them.
  if (out_report) *out_report = {};
  if (!dereverb_config_valid(config)) return SONARE_ERROR_INVALID_PARAMETER;

  return run_linked(
      channels, channel_count, length, sample_rate, out_channels, out_report,
      [&](const Audio* const* inputs, std::vector<Audio>* produced) {
        return to_c_dereverb_report(sonare::mastering::repair::dereverb_classical_linked(
            inputs, channel_count, produced, to_cpp_dereverb_config(config)));
      });
}

SonareError sonare_mastering_repair_detect_reverb(const float* samples, size_t length,
                                                  int sample_rate,
                                                  const SonareDereverbClassicalConfig* config,
                                                  SonareReverbDetection* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_reverb_detection(sonare::mastering::repair::detect_reverb(
        samples, length, sample_rate, to_cpp_dereverb_config(config)));
  });
}

SonareError sonare_mastering_repair_trim_silence(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareTrimSilenceConfig* config, float** out,
                                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!begin_vector_output(out, out_length)) return SONARE_ERROR_INVALID_PARAMETER;

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
  return run_stereo(left, right, length, sample_rate, out, [&](const Audio& l, const Audio& r) {
    const auto result =
        sonare::mastering::repair::trim_silence_stereo(l, r, to_cpp_trim_silence_config(config));
    out->report = to_c_trim_report(result.report);
    out->left_range = to_c_trim_range(result.left_range);
    out->right_range = to_c_trim_range(result.right_range);
    // A trimmed pair can come back empty, which no other repair stereo entry can
    // produce. Hand back (NULL, 0) rather than a zero-length allocation, matching
    // the empty-result policy the mono entries take through copy_audio_result.
    if (!result.left.empty()) fill_stereo_channels(result, out);
  });
}

SonareError sonare_mastering_repair_detect_trim_range(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareTrimSilenceConfig* config,
                                                      SonareTrimRange* out) {
  SONARE_C_API_ENTRY;
  return run_detection(samples, length, sample_rate, out, [&] {
    return to_c_trim_range(sonare::mastering::repair::detect_trim_range(
        samples, length, sample_rate, to_cpp_trim_silence_config(config)));
  });
}

SonareError sonare_mastering_repair_detect_trim_range_stereo(const float* left, const float* right,
                                                             size_t length, int sample_rate,
                                                             const SonareTrimSilenceConfig* config,
                                                             SonareTrimRange* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Cleared before any validation return, so a rejected call hands back a zero
  // range rather than whatever the caller's stack slot held.
  *out = SonareTrimRange{};

  SonareError err = validate_stereo_audio_params(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  *out = to_c_trim_range(sonare::mastering::repair::detect_trim_range_stereo(
      left, right, length, sample_rate, to_cpp_trim_silence_config(config)));
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

SonareError sonare_mastering_repair_analyze(const float* const* channels, size_t channel_count,
                                            size_t length, int sample_rate,
                                            const char* request_json, char** json_out) {
  SONARE_C_API_ENTRY;
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
  if (!channels || channel_count == 0) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t c = 0; c < channel_count; ++c) {
    const SonareError err = validate_audio_params(channels[c], length, sample_rate);
    if (err != SONARE_OK) return err;
  }

  SONARE_C_TRY
  *json_out = copy_string(sonare::mastering::assistant::repair_analyze_json(
      channels, channel_count, length, sample_rate, request_json ? request_json : ""));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_apply(const float* const* channels, size_t channel_count,
                                          size_t length, int sample_rate, const char* stages_json,
                                          SonareMasteringProgressCallback callback, void* user_data,
                                          float* const* out_channels, char** reports_json_out,
                                          SonareCancelCallback cancel_cb, void* cancel_user_data) {
  SONARE_C_API_ENTRY;
  if (!reports_json_out) return SONARE_ERROR_INVALID_PARAMETER;
  *reports_json_out = nullptr;
  if (!channels || !out_channels || channel_count == 0) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t c = 0; c < channel_count; ++c) {
    if (!out_channels[c]) return SONARE_ERROR_INVALID_PARAMETER;
    const SonareError err = validate_audio_params(channels[c], length, sample_rate);
    if (err != SONARE_OK) return err;
  }

  SONARE_C_TRY
  sonare::mastering::assistant::RepairProgressCallback progress;
  if (callback) {
    progress = [callback, user_data](float done, const char* stage) {
      callback(done, stage, user_data);
    };
  }
  sonare::mastering::assistant::RepairCancelCallback cancel;
  if (cancel_cb) {
    cancel = [cancel_cb, cancel_user_data] { return cancel_cb(cancel_user_data) != 0; };
  }
  std::string reports;
  if (!sonare::mastering::assistant::repair_apply_json(channels, channel_count, length, sample_rate,
                                                       stages_json ? stages_json : "", out_channels,
                                                       &reports, progress, cancel)) {
    return SONARE_ERROR_CANCELLED;
  }
  *reports_json_out = copy_string(reports);
  return SONARE_OK;
  SONARE_C_CATCH
}
