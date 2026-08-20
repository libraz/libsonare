#include <sonare/sonare_c.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "effects/decompose.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
#include "effects/remix.h"
#include "effects/spectral_edit.h"
#include "effects/time_stretch.h"
#include "sonare_c_internal.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare_c_detail;

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out) {
  SONARE_C_API_ENTRY;
  return sonare_hpss_ex(samples, length, sample_rate, kernel_harmonic, kernel_percussive,
                        constants::kDefaultNFft, constants::kDefaultHopLength, 1, 0, out, nullptr);
}

SonareError sonare_hpss_ex(const float* samples, size_t length, int sample_rate,
                           int kernel_harmonic, int kernel_percussive, int n_fft, int hop_length,
                           int use_soft_mask, int with_residual, SonareHpssResult* out,
                           float** out_residual) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) {
    out->harmonic = nullptr;
    out->percussive = nullptr;
    out->length = 0;
    out->sample_rate = 0;
  }
  if (out_residual != nullptr) *out_residual = nullptr;
  if (out == nullptr || (with_residual != 0 && out_residual == nullptr)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Validate the transform shape at the boundary rather than relying on the
  // core's precondition throw, so every _ex entry point rejects it identically
  // and before any input is decoded.
  if (n_fft <= 0 || hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    HpssConfig config;
    config.kernel_size_harmonic = kernel_harmonic;
    config.kernel_size_percussive = kernel_percussive;
    config.use_soft_mask = use_soft_mask != 0;

    StftConfig stft_config;
    stft_config.n_fft = n_fft;
    stft_config.hop_length = hop_length;

    if (with_residual == 0) {
      HpssAudioResult result = hpss(audio, config, stft_config);
      const size_t n = result.harmonic.size();
      std::unique_ptr<float[]> harmonic(new float[n]);
      std::unique_ptr<float[]> percussive(new float[n]);
      std::memcpy(harmonic.get(), result.harmonic.data(), n * sizeof(float));
      std::memcpy(percussive.get(), result.percussive.data(), n * sizeof(float));

      out->length = n;
      out->sample_rate = result.harmonic.sample_rate();
      out->harmonic = release_array(harmonic);
      out->percussive = release_array(percussive);
      return SONARE_OK;
    }

    HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);
    const size_t n = result.harmonic.size();
    std::unique_ptr<float[]> harmonic(new float[n]);
    std::unique_ptr<float[]> percussive(new float[n]);
    std::unique_ptr<float[]> residual(new float[n]);
    std::memcpy(harmonic.get(), result.harmonic.data(), n * sizeof(float));
    std::memcpy(percussive.get(), result.percussive.data(), n * sizeof(float));
    std::memcpy(residual.get(), result.residual.data(), n * sizeof(float));

    out->length = n;
    out->sample_rate = result.harmonic.sample_rate();
    out->harmonic = release_array(harmonic);
    out->percussive = release_array(percussive);
    *out_residual = release_array(residual);
    return SONARE_OK;
  });
}

SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length) {
  SONARE_C_API_ENTRY;
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [](const Audio& a) { return harmonic(a); });
}

SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length) {
  SONARE_C_API_ENTRY;
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [](const Audio& a) { return percussive(a); });
}

SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate, float rate,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_time_stretch_ex(samples, length, sample_rate, rate, constants::kDefaultNFft,
                                constants::kDefaultHopLength, out, out_length);
}

SonareError sonare_time_stretch_ex(const float* samples, size_t length, int sample_rate, float rate,
                                   int n_fft, int hop_length, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = nullptr;
  if (out_length != nullptr) *out_length = 0;
  if (out == nullptr || out_length == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  TimeStretchConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = StretchBackend::NativeSpectral;
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [rate, config](const Audio& a) { return time_stretch(a, rate, config); });
}

SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float semitones, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_pitch_shift_ex(samples, length, sample_rate, semitones, constants::kDefaultNFft,
                               constants::kDefaultHopLength, out, out_length);
}

SonareError sonare_pitch_shift_ex(const float* samples, size_t length, int sample_rate,
                                  float semitones, int n_fft, int hop_length, float** out,
                                  size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = nullptr;
  if (out_length != nullptr) *out_length = 0;
  if (out == nullptr || out_length == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  PitchShiftPlan plan;
  if (!make_pitch_shift_plan(length, sample_rate, semitones, &plan)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  PitchShiftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = StretchBackend::NativeSpectral;
  return run_mono_offline(
      samples, length, sample_rate, out, out_length,
      [semitones, config](const Audio& a) { return pitch_shift(a, semitones, config); });
}

SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [target_db](const Audio& a) { return normalize(a, target_db); });
}

SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_trim_ex(samples, length, sample_rate, threshold_db, constants::kDefaultNFft,
                        constants::kDefaultHopLength, out, out_length);
}

SonareError sonare_normalize_rms(const float* samples, size_t length, int sample_rate,
                                 float target_db, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = nullptr;
  if (out_length != nullptr) *out_length = 0;
  if (out == nullptr || out_length == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return run_mono_offline(
      samples, length, sample_rate, out, out_length,
      [target_db](const Audio& a) { return normalize_rms(a, target_db, true); });
}

SonareError sonare_trim_ex(const float* samples, size_t length, int sample_rate, float threshold_db,
                           int frame_length, int hop_length, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = nullptr;
  if (out_length != nullptr) *out_length = 0;
  if (out == nullptr || out_length == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (frame_length <= 0 || hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [threshold_db, frame_length, hop_length](const Audio& a) {
                            return trim_absolute(a, threshold_db, frame_length, hop_length);
                          });
}

SonareError sonare_decompose_with_init(const float* s, int n_features, int n_frames,
                                       int n_components, int n_iter, float beta, const char* init,
                                       float** out_w, size_t* out_w_length, float** out_h,
                                       size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  if (!out_w || !out_w_length || !out_h || !out_h_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out_w = nullptr;
  *out_w_length = 0;
  *out_h = nullptr;
  *out_h_length = 0;
  if (!s || n_features <= 0 || n_frames <= 0 || n_components <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // n_iter==0 would skip all multiplicative-update iterations and return the
  // raw (random or NNDSVD) init matrices: a plausible-shaped but meaningless
  // factorisation. Reject it at the boundary so callers cannot get garbage
  // that reports SONARE_OK.
  if (n_iter <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Non-finite elements of `s` are rejected by the core (decompose throws
  // InvalidParameter, which SONARE_C_CATCH maps to SONARE_ERROR_INVALID_PARAMETER).

  SONARE_C_TRY
  std::string init_str = init ? init : "random";
  DecomposeResult result =
      decompose(s, n_features, n_frames, n_components, n_iter, "mu", beta, init_str);

  SonareError werr = copy_vector(result.W, out_w, out_w_length);
  if (werr != SONARE_OK) return werr;
  return copy_vector(result.H, out_h, out_h_length);
  SONARE_C_CATCH
}

SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, float** out_w, size_t* out_w_length,
                             float** out_h, size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  // Backward-compatible delegation: original ABI always used random init.
  return sonare_decompose_with_init(s, n_features, n_frames, n_components, n_iter, beta, "random",
                                    out_w, out_w_length, out_h, out_h_length);
}

SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  if (!s || n_features <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Non-finite elements of `s` are rejected by the core (nn_filter throws
  // InvalidParameter, which SONARE_C_CATCH maps to SONARE_ERROR_INVALID_PARAMETER).

  SONARE_C_TRY
  std::string agg = aggregate ? aggregate : "mean";
  std::vector<float> result = nn_filter(s, n_features, n_frames, agg, k, width);

  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_remix(const float* samples, size_t length, int sample_rate, const int* intervals,
                         size_t interval_count, int align_zeros, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  if (interval_count > 0 && !intervals) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(interval_count);
    for (size_t i = 0; i < interval_count; ++i) {
      pairs.emplace_back(intervals[2 * i], intervals[2 * i + 1]);
    }
    std::vector<float> result = remix(audio.data(), audio.size(), pairs, align_zeros != 0);

    return copy_vector(result, out, out_length);
  });
}

SonareError sonare_decompose_stems(const float* samples, size_t length, int sample_rate,
                                   const SonareDecomposeStemsConfig* config, float** out,
                                   size_t* out_component_count, size_t* out_component_length,
                                   float** out_w, size_t* out_w_length, float** out_h,
                                   size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_component_count || !out_component_length) return SONARE_ERROR_INVALID_PARAMETER;
  if ((out_w != nullptr) != (out_w_length != nullptr)) return SONARE_ERROR_INVALID_PARAMETER;
  if ((out_h != nullptr) != (out_h_length != nullptr)) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_component_count = 0;
  *out_component_length = 0;
  if (out_w != nullptr) {
    *out_w = nullptr;
    *out_w_length = 0;
  }
  if (out_h != nullptr) {
    *out_h = nullptr;
    *out_h_length = 0;
  }
  if (config != nullptr && (config->struct_version < 0 || config->struct_version > 1)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  DecomposeStemsConfig core_config;
  if (config != nullptr) {
    if (config->n_components < 0 || config->n_fft < 0 || config->hop_length < 0 ||
        config->n_iter < 0 || !std::isfinite(config->beta) || !std::isfinite(config->mask_power) ||
        config->mask_power < 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (config->n_components > 0) core_config.n_components = config->n_components;
    if (config->n_fft > 0) core_config.n_fft = config->n_fft;
    if (config->hop_length > 0) core_config.hop_length = config->hop_length;
    if (config->n_iter > 0) core_config.n_iter = config->n_iter;
    if (config->beta != 0.0f) core_config.beta = config->beta;
    if (config->init != nullptr && config->init[0] != '\0') core_config.init = config->init;
    if (config->mask_power > 0.0f) core_config.mask_power = config->mask_power;
  }
  if (core_config.mask_power < 1.0f) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    DecomposeStemsResult result =
        decompose_stems(audio.data(), audio.size(), sample_rate, core_config);
    if (result.components.empty()) return SONARE_OK;
    const size_t component_length = result.components.front().size();
    if (component_length == 0) return SONARE_OK;
    if (result.components.size() > SIZE_MAX / component_length) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    auto flat = std::make_unique<float[]>(result.components.size() * component_length);
    for (size_t component = 0; component < result.components.size(); ++component) {
      std::memcpy(flat.get() + component * component_length, result.components[component].data(),
                  component_length * sizeof(float));
    }
    if (out_w != nullptr) {
      SonareError werr = copy_vector(result.W, out_w, out_w_length);
      if (werr != SONARE_OK) return werr;
    }
    if (out_h != nullptr) {
      SonareError herr = copy_vector(result.H, out_h, out_h_length);
      if (herr != SONARE_OK) {
        if (out_w != nullptr) {
          sonare_free_floats(*out_w);
          *out_w = nullptr;
          *out_w_length = 0;
        }
        return herr;
      }
    }
    *out = flat.release();
    *out_component_count = result.components.size();
    *out_component_length = component_length;
    return SONARE_OK;
  });
}

SonareError sonare_remix_aligned_intervals(const float* samples, size_t length, int sample_rate,
                                           const int* intervals, size_t interval_count,
                                           int align_zeros, int** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!out || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_count = 0;
  if (interval_count > 0 && !intervals) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(interval_count);
    for (size_t i = 0; i < interval_count; ++i) {
      pairs.emplace_back(intervals[2 * i], intervals[2 * i + 1]);
    }
    const std::vector<std::pair<int, int>> resolved =
        align_remix_intervals(audio.data(), audio.size(), pairs, align_zeros != 0);
    if (resolved.empty()) return SONARE_OK;
    auto flat = std::make_unique<int[]>(resolved.size() * 2);
    for (size_t i = 0; i < resolved.size(); ++i) {
      flat[2 * i] = resolved[i].first;
      flat[2 * i + 1] = resolved[i].second;
    }
    *out = flat.release();
    *out_count = resolved.size();
    return SONARE_OK;
  });
}

SonareError sonare_hpss_with_residual(const float* samples, size_t length, int sample_rate,
                                      int kernel_harmonic, int kernel_percussive,
                                      float** out_harmonic, float** out_percussive,
                                      float** out_residual, size_t* out_length,
                                      int* out_sample_rate) {
  SONARE_C_API_ENTRY;
  if (!out_harmonic || !out_percussive || !out_residual || !out_length || !out_sample_rate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_harmonic = nullptr;
  *out_percussive = nullptr;
  *out_residual = nullptr;
  *out_length = 0;
  *out_sample_rate = 0;

  SonareHpssResult result{};
  float* residual = nullptr;
  SonareError err = sonare_hpss_ex(samples, length, sample_rate, kernel_harmonic, kernel_percussive,
                                   constants::kDefaultNFft, constants::kDefaultHopLength, 1, 1,
                                   &result, &residual);
  if (err != SONARE_OK) return err;

  *out_harmonic = result.harmonic;
  *out_percussive = result.percussive;
  *out_residual = residual;
  *out_length = result.length;
  *out_sample_rate = result.sample_rate;
  return SONARE_OK;
}

SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate, float rate,
                                 int n_fft, int hop_length, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  if (rate <= 0.0f) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    StftConfig stft_config;
    stft_config.n_fft = n_fft;
    stft_config.hop_length = hop_length;
    Spectrogram spec = Spectrogram::compute(audio, stft_config);

    PhaseVocoderConfig pv_config;
    pv_config.hop_length = hop_length;
    Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

    int expected_length = static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate));
    Audio result = stretched.to_audio(expected_length);

    return copy_audio_result(result, out, out_length);
  });
}

namespace {

/// @brief Maps a SonareWindowType int to the core WindowType (defaults to Hann).
WindowType spectral_edit_window(int value) {
  switch (value) {
    case SONARE_WINDOW_HAMMING:
      return WindowType::Hamming;
    case SONARE_WINDOW_BLACKMAN:
      return WindowType::Blackman;
    case SONARE_WINDOW_RECTANGULAR:
      return WindowType::Rectangular;
    case SONARE_WINDOW_HANN:
    default:
      return WindowType::Hann;
  }
}

}  // namespace

SonareError sonare_spectral_edit(const float* samples, size_t length, int sample_rate,
                                 const SonareSpectralEditConfig* config,
                                 const SonareSpectralRegionOp* ops, size_t n_ops, float** out,
                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *out_length = 0;
  // ops may be NULL iff there are no ops (identity transform).
  if (ops == nullptr && n_ops != 0) return SONARE_ERROR_INVALID_PARAMETER;

  SpectralEditConfig core_config;  // n_fft=2048, hop=512, Hann, heal_radius=2
  if (config != nullptr) {
    if (config->n_fft != 0) core_config.n_fft = config->n_fft;
    if (config->hop_length != 0) core_config.hop_length = config->hop_length;
    core_config.window = spectral_edit_window(config->window);
    if (config->heal_radius_frames != 0)
      core_config.heal_radius_frames = config->heal_radius_frames;
  }

  std::vector<SpectralRegionOp> core_ops(n_ops);
  for (size_t i = 0; i < n_ops; ++i) {
    if (ops[i].mode < SONARE_SPECTRAL_EDIT_MODE_GAIN ||
        ops[i].mode > SONARE_SPECTRAL_EDIT_MODE_HEAL)
      return SONARE_ERROR_INVALID_PARAMETER;
    core_ops[i].start_sample = ops[i].start_sample;
    core_ops[i].end_sample = ops[i].end_sample;
    core_ops[i].low_hz = ops[i].low_hz;
    core_ops[i].high_hz = ops[i].high_hz;
    core_ops[i].gain_db = ops[i].gain_db;
    core_ops[i].mode = static_cast<SpectralEditMode>(ops[i].mode);
  }

  return run_mono_offline(samples, length, sample_rate, out, out_length,
                          [&](const Audio& audio) -> Audio {
                            return spectral_edit(audio, core_config, core_ops.data(), n_ops);
                          });
}
