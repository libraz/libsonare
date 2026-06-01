#include <cstring>
#include <memory>

#include "core/audio.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_c.h"
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
  cpp.gain_floor = config->gain_floor;
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

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

void clear_float_output(float** out, size_t* out_length) {
  *out = nullptr;
  *out_length = 0;
}

}  // namespace

SonareError sonare_mastering_repair_declick(const float* samples, size_t length, int sample_rate,
                                            const SonareDeclickConfig* config, float** out,
                                            size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = sonare::mastering::repair::declick(audio, to_cpp_declick_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_denoise_classical(const float* samples, size_t length,
                                                      int sample_rate,
                                                      const SonareDenoiseClassicalConfig* config,
                                                      float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (config) {
    if (!is_power_of_two(config->n_fft)) return SONARE_ERROR_INVALID_PARAMETER;
    if (config->hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = sonare::mastering::repair::denoise_classical(audio, to_cpp_denoise_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_declip(const float* samples, size_t length, int sample_rate,
                                           const SonareDeclipConfig* config, float** out,
                                           size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = sonare::mastering::repair::declip(audio, to_cpp_declip_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_decrackle(const float* samples, size_t length, int sample_rate,
                                              const SonareDecrackleConfig* config, float** out,
                                              size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = sonare::mastering::repair::decrackle(audio, to_cpp_decrackle_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_dehum(const float* samples, size_t length, int sample_rate,
                                          const SonareDehumConfig* config, float** out,
                                          size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = sonare::mastering::repair::dehum(audio, to_cpp_dehum_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_dereverb_classical(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareDereverbClassicalConfig* config,
                                                       float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (config) {
    if (!is_power_of_two(config->n_fft)) return SONARE_ERROR_INVALID_PARAMETER;
    if (config->hop_length <= 0 || config->hop_length > config->n_fft) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result =
      sonare::mastering::repair::dereverb_classical(audio, to_cpp_dereverb_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_repair_trim_silence(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareTrimSilenceConfig* config, float** out,
                                                 size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  Audio result = sonare::mastering::repair::trim_silence(audio, to_cpp_trim_silence_config(config));
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}
