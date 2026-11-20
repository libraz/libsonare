#include <sonare/sonare_c.h>

#include <array>
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
#include "filters/iir.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"
#include "util/constants.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

using namespace sonare;
using namespace sonare_c_detail;
using sonare_c_mastering_detail::copy_stereo_channels;
using sonare_c_mastering_detail::validate_stereo_audio_params;

namespace {

bool fill_decompose_stems_config(const SonareDecomposeStemsConfig* config,
                                 DecomposeStemsConfig* core_config) {
  if (config != nullptr) {
    if (config->n_components < 0 || config->n_fft < 0 || config->hop_length < 0 ||
        config->n_iter < 0 || !std::isfinite(config->beta) || !std::isfinite(config->mask_power) ||
        config->mask_power < 0.0f) {
      return false;
    }
    if (config->n_components > 0) core_config->n_components = config->n_components;
    if (config->n_fft > 0) core_config->n_fft = config->n_fft;
    if (config->hop_length > 0) core_config->hop_length = config->hop_length;
    if (config->n_iter > 0) core_config->n_iter = config->n_iter;
    if (config->beta != 0.0f) core_config->beta = config->beta;
    if (config->init != nullptr && config->init[0] != '\0') core_config->init = config->init;
    if (config->mask_power > 0.0f) core_config->mask_power = config->mask_power;
  }
  if (core_config->mask_power < 1.0f) return false;
  return true;
}

template <typename DecomposeResult>
SonareError copy_decompose_factors(const DecomposeResult& result, float** out_w,
                                   size_t* out_w_length, float** out_h, size_t* out_h_length) {
  if ((out_w != nullptr) != (out_w_length != nullptr) ||
      (out_h != nullptr) != (out_h_length != nullptr)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  // Stage both factor buffers before publishing either output.
  std::unique_ptr<float[]> staged_w;
  std::unique_ptr<float[]> staged_h;
  if (out_w != nullptr) staged_w.reset(copy_vector(result.W));
  if (out_h != nullptr) staged_h.reset(copy_vector(result.H));

  if (out_w != nullptr) {
    *out_w = staged_w.release();
    *out_w_length = result.W.size();
  }
  if (out_h != nullptr) {
    *out_h = staged_h.release();
    *out_h_length = result.H.size();
  }
  return SONARE_OK;
}

/// @brief run_mono_offline for a caller that already refused the buffer and the outputs.
template <typename Fn>
SonareError run_mono_prevalidated(const float* samples, size_t length, int sample_rate, float** out,
                                  size_t* out_length, Fn process) {
  return run_prevalidated_offline(samples, length, sample_rate,
                                  [&](const Audio& audio) -> SonareError {
                                    Audio result = process(audio);
                                    return copy_audio_result(result, out, out_length);
                                  });
}

}  // namespace

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return sonare_hpss_ex(samples, length, sample_rate, kernel_harmonic, kernel_percussive,
                        constants::kDefaultNFft, constants::kDefaultHopLength, 1, 0, out, nullptr);
}

SonareError sonare_hpss_ex(const float* samples, size_t length, int sample_rate,
                           int kernel_harmonic, int kernel_percussive, int n_fft, int hop_length,
                           int use_soft_mask, int with_residual, SonareHpssResult* out,
                           float** out_residual) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_residual);
  if (const SonareError err =
          validate_audio_params("hpss_ex", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  // The FFT size is refused before any input is decoded. A hop of 0 is absent and
  // the core runs a quarter of n_fft; it names a negative one.
  if (n_fft <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_REQUIRE_NONNULL(out);
  if (with_residual != 0) SONARE_C_REQUIRE_NONNULL(out_residual);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("harmonic", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [](const Audio& a) { return harmonic(a); });
}

SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("percussive", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [](const Audio& a) { return percussive(a); });
}

SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate,
                                float speed_ratio, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  return sonare_time_stretch_ex(samples, length, sample_rate, speed_ratio, constants::kDefaultNFft,
                                constants::kDefaultHopLength, out, out_length);
}

SonareError sonare_time_stretch_ex(const float* samples, size_t length, int sample_rate,
                                   float speed_ratio, int n_fft, int hop_length, float** out,
                                   size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("time_stretch_ex", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  TimeStretchConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = StretchBackend::NativeSpectral;
  return run_mono_prevalidated(
      samples, length, sample_rate, out, out_length,
      [speed_ratio, config](const Audio& a) { return time_stretch(a, speed_ratio, config); });
}

SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float pitch_semitones, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  return sonare_pitch_shift_ex(samples, length, sample_rate, pitch_semitones,
                               constants::kDefaultNFft, constants::kDefaultHopLength, out,
                               out_length);
}

SonareError sonare_pitch_shift_ex(const float* samples, size_t length, int sample_rate,
                                  float pitch_semitones, int n_fft, int hop_length, float** out,
                                  size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("pitch_shift_ex", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }

  PitchShiftPlan plan;
  if (!make_pitch_shift_plan(length, sample_rate, pitch_semitones, &plan)) {
    set_last_error_static(SONARE_ERROR_INVALID_PARAMETER, kPitchShiftPlanError);
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  PitchShiftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = StretchBackend::NativeSpectral;
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [pitch_semitones, config](const Audio& a) {
                                 return pitch_shift(a, pitch_semitones, config);
                               });
}

SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("normalize", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [target_db](const Audio& a) { return normalize(a, target_db); });
}

SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  return sonare_trim_ex(samples, length, sample_rate, threshold_db, kDefaultSilenceFrameLength,
                        kDefaultSilenceHopLength, out, out_length);
}

SonareError sonare_normalize_rms(const float* samples, size_t length, int sample_rate,
                                 float target_db, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("normalize_rms", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(
      samples, length, sample_rate, out, out_length,
      [target_db](const Audio& a) { return normalize_rms(a, target_db, true); });
}

SonareError sonare_fade(const float* samples, size_t length, int sample_rate, float fade_in_sec,
                        float fade_out_sec, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("fade", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        sonare::util::check::at_least(fade_in_sec, 0.0, "fade_in_sec");
        sonare::util::check::at_least(fade_out_sec, 0.0, "fade_out_sec");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [fade_in_sec, fade_out_sec](const Audio& a) {
                                 return fade_out(fade_in(a, fade_in_sec), fade_out_sec);
                               });
}

void sonare_biquad_filter_config_init(SonareBiquadFilterConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  config->type = SONARE_BIQUAD_FILTER_HIGH_PASS;
  config->order = 2;
}

namespace {

/// @brief Refuses the biquad filter configuration in member order.
void check_biquad_filter_config(const SonareBiquadFilterConfig& config, int sample_rate) {
  namespace check = sonare::util::check;
  check::integer_at_least(config.struct_size, static_cast<long long>(sizeof(config)),
                          "config.struct_size");
  check::integer_in_range(config.type, SONARE_BIQUAD_FILTER_HIGH_PASS, SONARE_BIQUAD_FILTER_NOTCH,
                          "config.type");
  check::one_of(config.order, std::array<int, 2>{2, 4}, "config.order");
  const bool corner =
      config.type == SONARE_BIQUAD_FILTER_HIGH_PASS || config.type == SONARE_BIQUAD_FILTER_LOW_PASS;
  const double nyquist = static_cast<double>(sample_rate) / 2.0;
  const auto unread = [](const char* subject, const char* types) {
    throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter, {subject, "config.type"},
                                           std::string("{0} must be 0 when {1} is ") + types);
  };
  if (corner) {
    check::in_interval(config.cutoff_hz, 0.0, false, nyquist, false, "config.cutoff_hz");
    if (config.center_hz != 0.0f) unread("config.center_hz", "high-pass or low-pass");
    if (config.bandwidth_hz != 0.0f) unread("config.bandwidth_hz", "high-pass or low-pass");
  } else {
    if (config.cutoff_hz != 0.0f) unread("config.cutoff_hz", "band-pass or notch");
    check::in_interval(config.center_hz, 0.0, false, nyquist, false, "config.center_hz");
    check::greater_than(config.bandwidth_hz, 0.0, "config.bandwidth_hz");
    if (config.order != 2) {
      throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter,
                                             {"config.order", "config.type"},
                                             "{0} must be 2 when {1} is band-pass or notch");
    }
  }
}

/// @brief The filter the native CLI's `filter` command runs, for a checked configuration.
std::vector<float> run_biquad_filter(const Audio& audio, const SonareBiquadFilterConfig& config) {
  const int sr = audio.sample_rate();
  const bool zero_phase = config.zero_phase != 0;
  switch (config.type) {
    case SONARE_BIQUAD_FILTER_HIGH_PASS:
    case SONARE_BIQUAD_FILTER_LOW_PASS: {
      const bool high = config.type == SONARE_BIQUAD_FILTER_HIGH_PASS;
      if (config.order == 4) {
        const CascadedBiquad cascade = high ? highpass_coeffs_4th(config.cutoff_hz, sr)
                                            : lowpass_coeffs_4th(config.cutoff_hz, sr);
        return zero_phase ? apply_cascade_filtfilt(audio.data(), audio.size(), cascade)
                          : apply_cascade(audio.data(), audio.size(), cascade);
      }
      const BiquadCoeffs coeffs =
          high ? highpass_coeffs(config.cutoff_hz, sr) : lowpass_coeffs(config.cutoff_hz, sr);
      return zero_phase ? apply_biquad_filtfilt(audio.data(), audio.size(), coeffs)
                        : apply_biquad(audio.data(), audio.size(), coeffs);
    }
    default: {
      const BiquadCoeffs coeffs = config.type == SONARE_BIQUAD_FILTER_BAND_PASS
                                      ? bandpass_coeffs(config.center_hz, config.bandwidth_hz, sr)
                                      : notch_coeffs(config.center_hz, config.bandwidth_hz, sr);
      return zero_phase ? apply_biquad_filtfilt(audio.data(), audio.size(), coeffs)
                        : apply_biquad(audio.data(), audio.size(), coeffs);
    }
  }
}

}  // namespace

SonareError sonare_biquad_filter(const float* samples, size_t length, int sample_rate,
                                 const SonareBiquadFilterConfig* config, float** out,
                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("biquad_filter", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(config);
  if (const SonareError err =
          run_validation([&] { check_biquad_filter_config(*config, sample_rate); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  const SonareBiquadFilterConfig checked = *config;
  return run_mono_prevalidated(
      samples, length, sample_rate, out, out_length, [&checked](const Audio& a) {
        return Audio::from_vector(run_biquad_filter(a, checked), a.sample_rate());
      });
}

namespace {

/// @brief Shared body of the two stereo normalize entries.
/// @details Both differ only in which level they hand the core, so the
///          validation, the heap hand-off and the failure shape live here once.
template <typename Fn>
SonareError run_normalize_stereo(const char* fn, const char* left_name, const char* right_name,
                                 const float* left, const float* right, size_t length,
                                 int sample_rate, SonareNormalizeStereoResult* out,
                                 Fn&& normalize_pair) {
  SONARE_C_ZERO_OUTS(out);

  SonareError err =
      validate_stereo_audio_params(fn, left_name, right_name, left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  const auto result = normalize_pair(Audio::from_buffer(left, length, sample_rate),
                                     Audio::from_buffer(right, length, sample_rate));
  out->applied_gain_db = result.applied_gain_db;
  copy_stereo_channels(result.left.data(), result.right.data(), result.left.size(), &out->left,
                       &out->right, &out->length);
  return SONARE_OK;
  SONARE_C_CATCH
}

}  // namespace

SonareError sonare_normalize_stereo(const float* left, const float* right, size_t length,
                                    int sample_rate, float target_db,
                                    SonareNormalizeStereoResult* out) {
  SONARE_C_API_ENTRY;
  return run_normalize_stereo(
      "normalize_stereo", "left", "right", left, right, length, sample_rate, out,
      [target_db](const Audio& l, const Audio& r) { return normalize_stereo(l, r, target_db); });
}

SonareError sonare_normalize_rms_stereo(const float* left, const float* right, size_t length,
                                        int sample_rate, float target_db,
                                        SonareNormalizeStereoResult* out) {
  SONARE_C_API_ENTRY;
  return run_normalize_stereo("normalize_rms_stereo", "left", "right", left, right, length,
                              sample_rate, out, [target_db](const Audio& l, const Audio& r) {
                                return normalize_rms_stereo(l, r, target_db, true);
                              });
}

void sonare_free_normalize_stereo_result(SonareNormalizeStereoResult* result) {
  sonare_c_mastering_detail::free_stereo_channels(result);
}

SonareError sonare_trim_ex(const float* samples, size_t length, int sample_rate, float threshold_db,
                           int frame_length, int hop_length, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("trim_ex", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (frame_length <= 0 || hop_length <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [threshold_db, frame_length, hop_length](const Audio& a) {
                                 return trim_absolute(a, threshold_db, frame_length, hop_length);
                               });
}

SonareError sonare_decompose_with_init(const float* s, int n_features, int n_frames,
                                       int n_components, int n_iter, float beta, const char* init,
                                       float** out_w, size_t* out_w_length, float** out_h,
                                       size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_w, out_w_length, out_h, out_h_length);
  SONARE_C_REQUIRE_NONNULL(s);
  if (const SonareError err = run_validation(
          [&] { validate_decompose_shape(n_features, n_frames, n_components, n_iter); });
      err != SONARE_OK) {
    return err;
  }
  // 0 selects the library default; the core would read it as "no iterations"
  // and return the raw init matrices, so it never reaches the core as 0.
  if (n_iter == 0) n_iter = kDecomposeDefaultIter;
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out_w);
  SONARE_C_REQUIRE_NONNULL(out_w_length);
  SONARE_C_REQUIRE_NONNULL(out_h);
  SONARE_C_REQUIRE_NONNULL(out_h_length);
  // Non-finite elements of `s` are rejected by the core (decompose throws
  // InvalidParameter, which SONARE_C_CATCH maps to SONARE_ERROR_INVALID_PARAMETER).

  SONARE_C_TRY
  std::string init_str = init ? init : "random";
  DecomposeResult result =
      decompose(s, n_features, n_frames, n_components, n_iter, "mu", beta, init_str);

  return copy_decompose_factors(result, out_w, out_w_length, out_h, out_h_length);
  SONARE_C_CATCH
}

SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, float** out_w, size_t* out_w_length,
                             float** out_h, size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_w, out_w_length, out_h, out_h_length);
  // Backward-compatible delegation: original ABI always used random init.
  return sonare_decompose_with_init(s, n_features, n_frames, n_components, n_iter, beta, "random",
                                    out_w, out_w_length, out_h, out_h_length);
}

SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(s);
  if (n_features <= 0 || n_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  // Non-finite elements of `s` are rejected by the core (nn_filter throws
  // InvalidParameter, which SONARE_C_CATCH maps to SONARE_ERROR_INVALID_PARAMETER).

  if (k == -1) k = kNnFilterDefaultK;
  if (width == -1) width = kNnFilterDefaultWidth;

  SONARE_C_TRY
  std::string agg = aggregate ? aggregate : "mean";
  std::vector<float> result = nn_filter(s, n_features, n_frames, agg, k, width);

  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_remix(const float* samples, size_t length, int sample_rate, const int* intervals,
                         size_t interval_count, int align_zeros, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("remix", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(intervals, interval_count);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  SONARE_C_ZERO_OUTS(out, out_component_count, out_component_length, out_w, out_w_length, out_h,
                     out_h_length);
  if (const SonareError err =
          validate_audio_params("decompose_stems", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (config != nullptr && (config->struct_version < 0 || config->struct_version > 1)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  DecomposeStemsConfig core_config;
  if (!fill_decompose_stems_config(config, &core_config)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_component_count);
  SONARE_C_REQUIRE_NONNULL(out_component_length);
  // An optional pointer given without its length has nowhere to write the length.
  if (((out_w != nullptr) != (out_w_length != nullptr)) ||
      ((out_h != nullptr) != (out_h_length != nullptr))) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
          std::memcpy(flat.get() + component * component_length,
                      result.components[component].data(), component_length * sizeof(float));
        }
        SonareError factors_err =
            copy_decompose_factors(result, out_w, out_w_length, out_h, out_h_length);
        if (factors_err != SONARE_OK) return factors_err;
        *out = flat.release();
        *out_component_count = result.components.size();
        *out_component_length = component_length;
        return SONARE_OK;
      });
}

SonareError sonare_decompose_stems_linked(const float* const* channels, size_t num_channels,
                                          size_t length, int sample_rate,
                                          const SonareDecomposeStemsConfig* config, float** out,
                                          size_t* out_component_count, size_t* out_num_channels,
                                          size_t* out_component_length, float** out_w,
                                          size_t* out_w_length, float** out_h,
                                          size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_component_count, out_num_channels, out_component_length, out_w,
                     out_w_length, out_h, out_h_length);
  if (channels == nullptr || num_channels == 0 || num_channels > kMaxDecomposeStemsLinkedChannels) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t c = 0; c < num_channels; ++c) {
    SonareError err = validate_audio_params("decompose_stems_linked", "channels", channels[c],
                                            length, sample_rate);
    if (err != SONARE_OK) return err;
  }

  SONARE_C_TRY
  if (config != nullptr && (config->struct_version < 0 || config->struct_version > 1)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  DecomposeStemsConfig core_config;
  if (!fill_decompose_stems_config(config, &core_config)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_component_count);
  SONARE_C_REQUIRE_NONNULL(out_num_channels);
  SONARE_C_REQUIRE_NONNULL(out_component_length);
  // Same output-pair guard as sonare_decompose_stems.
  if (((out_w != nullptr) != (out_w_length != nullptr)) ||
      ((out_h != nullptr) != (out_h_length != nullptr))) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  std::vector<Audio> audio;
  audio.reserve(num_channels);
  for (size_t c = 0; c < num_channels; ++c) {
    audio.push_back(Audio::from_buffer(channels[c], length, sample_rate));
  }
  std::vector<const float*> pointers;
  pointers.reserve(num_channels);
  for (const Audio& one : audio) pointers.push_back(one.data());

  DecomposeStemsLinkedResult result =
      decompose_stems_linked(pointers.data(), num_channels, length, sample_rate, core_config);
  if (result.components.empty()) return SONARE_OK;
  const size_t component_length =
      result.components.front().empty() ? 0 : result.components.front().front().size();
  if (component_length == 0) return SONARE_OK;
  // Overflow-checked the same way as sonare_decompose_stems: reject rather than
  // wrap before the flat [component][channel][sample] allocation is sized.
  if (num_channels > SIZE_MAX / component_length) return SONARE_ERROR_INVALID_PARAMETER;
  const size_t per_component = num_channels * component_length;
  if (result.components.size() > SIZE_MAX / per_component) return SONARE_ERROR_INVALID_PARAMETER;

  auto flat = std::make_unique<float[]>(result.components.size() * per_component);
  for (size_t component = 0; component < result.components.size(); ++component) {
    for (size_t c = 0; c < num_channels; ++c) {
      std::memcpy(flat.get() + component * per_component + c * component_length,
                  result.components[component][c].data(), component_length * sizeof(float));
    }
  }
  SonareError factors_err =
      copy_decompose_factors(result, out_w, out_w_length, out_h, out_h_length);
  if (factors_err != SONARE_OK) return factors_err;
  *out = flat.release();
  *out_component_count = result.components.size();
  *out_num_channels = num_channels;
  *out_component_length = component_length;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_remix_aligned_intervals(const float* samples, size_t length, int sample_rate,
                                           const int* intervals, size_t interval_count,
                                           int align_zeros, int** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err =
          validate_audio_params("remix_aligned_intervals", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(intervals, interval_count);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
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
  SONARE_C_ZERO_OUTS(out_harmonic, out_percussive, out_residual, out_length, out_sample_rate);
  if (const SonareError err =
          validate_audio_params("hpss_ex", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_harmonic);
  SONARE_C_REQUIRE_NONNULL(out_percussive);
  SONARE_C_REQUIRE_NONNULL(out_residual);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_sample_rate);

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

SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate,
                                 float speed_ratio, int n_fft, int hop_length, float** out,
                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("phase_vocoder", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { validate_stretch_rate(speed_ratio); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        // A hop of 0 is absent and runs a quarter of n_fft; a negative one is
        // named by the core.
        const int hop = resolve_resynthesis_hop(hop_length, n_fft);
        StftConfig stft_config;
        stft_config.n_fft = n_fft;
        stft_config.hop_length = hop;
        Spectrogram spec = Spectrogram::compute(audio, stft_config);

        PhaseVocoderConfig pv_config;
        pv_config.hop_length = hop;
        Spectrogram stretched = phase_vocoder(spec, speed_ratio, pv_config);

        int expected_length =
            static_cast<int>(std::ceil(static_cast<float>(audio.size()) / speed_ratio));
        Audio result = stretched.to_audio(expected_length);

        return copy_audio_result(result, out, out_length);
      });
}

SonareError sonare_spectral_edit(const float* samples, size_t length, int sample_rate,
                                 const SonareSpectralEditConfig* config,
                                 const SonareSpectralRegionOp* ops, size_t n_ops, float** out,
                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_TRY
  if (const SonareError err =
          validate_audio_params("spectral_edit", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }

  SpectralEditConfig core_config;  // n_fft=2048, hop a quarter of n_fft, Hann, heal_radius=2
  if (config != nullptr) {
    if (config->n_fft != 0) core_config.n_fft = config->n_fft;
    if (config->hop_length != 0) core_config.hop_length = config->hop_length;
    if (!valid_window(config->window)) return SONARE_ERROR_INVALID_PARAMETER;
    core_config.window = to_window_type(config->window);
    if (config->heal_radius_frames != 0)
      core_config.heal_radius_frames = config->heal_radius_frames;
  }

  // ops may be NULL iff there are no ops (identity transform).
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(ops, n_ops);

  std::vector<SpectralRegionOp> core_ops(n_ops);
  for (size_t i = 0; i < n_ops; ++i) {
    if (!ordinal_to_enum(ops[i].mode, SONARE_SPECTRAL_EDIT_MODE_GAIN,
                         SONARE_SPECTRAL_EDIT_MODE_HEAL, &core_ops[i].mode)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    core_ops[i].start_sample = ops[i].start_sample;
    core_ops[i].end_sample = ops[i].end_sample;
    core_ops[i].low_hz = ops[i].low_hz;
    core_ops[i].high_hz = ops[i].high_hz;
    core_ops[i].gain_db = ops[i].gain_db;
  }

  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [&](const Audio& audio) -> Audio {
                                 return spectral_edit(audio, core_config, core_ops.data(), n_ops);
                               });
  SONARE_C_CATCH
}
