#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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
#include "effects_config_domains.h"
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

// The core spells the STFT framing bare; at a folded entry they are members of `options`.
const std::array<CName, 2> kStftOptionNames{
    {{"n_fft", "options.n_fft"}, {"hop_length", "options.hop_length"}}};
const std::array<CName, 3> kTimeStretchNames{
    {{"rate", "speed_ratio"}, {"n_fft", "options.n_fft"}, {"hop_length", "options.hop_length"}}};

/// Checks a decompose-stems config and copies it onto the core one. Throws the refusal.
void fill_decompose_stems_config(const SonareDecomposeStemsConfig& config,
                                 DecomposeStemsConfig* core_config) {
  namespace check = sonare::util::check;
  require_struct_size(config.struct_size, sizeof(config), "config",
                      "sonare_decompose_stems_config_init");
  check::integer_within(config.n_components, kDecomposeNComponentsDomain, "config.n_components");
  check::even_within(config.n_fft, kColaNFftDomain, "config.n_fft");
  check_stft_hop(config.hop_length, "config.hop_length");
  check::integer_within(config.n_iter, kDecomposeStemsIterDomain, "config.n_iter");
  check::finite_float(config.beta, "config.beta");
  check::non_null(config.init, "config.init");
  check::at_least(config.mask_power, 1.0, "config.mask_power");
  core_config->n_components = config.n_components;
  core_config->n_fft = config.n_fft;
  core_config->hop_length = core_stft_hop(config.hop_length);
  core_config->n_iter = config.n_iter;
  core_config->beta = config.beta;
  core_config->init = config.init;
  core_config->mask_power = config.mask_power;
}

/// Refuses an optional factor buffer given without its length, or a length without its buffer.
SonareError check_factor_output_pairs(const void* out_w, const void* out_w_length,
                                      const void* out_h, const void* out_h_length) {
  return run_validation([&] {
    if ((out_w != nullptr) != (out_w_length != nullptr)) {
      sonare::util::check::refuse_relation({"out_w", "out_w_length"},
                                           "{0} and {1} must be given together");
    }
    if ((out_h != nullptr) != (out_h_length != nullptr)) {
      sonare::util::check::refuse_relation({"out_h", "out_h_length"},
                                           "{0} and {1} must be given together");
    }
  });
}

template <typename DecomposeResult>
SonareError copy_decompose_factors(const DecomposeResult& result, float** out_w,
                                   size_t* out_w_length, float** out_h, size_t* out_h_length) {
  if (const SonareError err = check_factor_output_pairs(out_w, out_w_length, out_h, out_h_length);
      err != SONARE_OK) {
    return err;
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

void sonare_hpss_options_init(SonareHpssOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = constants::kDefaultNFft;
  options->use_soft_mask = 1;
}

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, const SonareHpssOptions* options,
                        SonareHpssResult* out, float** out_residual) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_residual);
  if (const SonareError err =
          validate_audio_params("hpss", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonareHpssOptions resolved;
  if (const SonareError err =
          resolve_options(options, sonare_hpss_options_init, "sonare_hpss_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  // The FFT size and hop are refused before any input is decoded.
  if (const SonareError err = run_validation([&] {
        sonare::util::check::even_within(resolved.n_fft, sonare::kColaNFftDomain, "options.n_fft");
        check_stft_hop(resolved.hop_length, "options.hop_length");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  const bool with_residual = out_residual != nullptr;

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        HpssConfig config;
        config.kernel_size_harmonic = kernel_harmonic;
        config.kernel_size_percussive = kernel_percussive;
        config.use_soft_mask = resolved.use_soft_mask != 0;

        StftConfig stft_config;
        stft_config.n_fft = resolved.n_fft;
        stft_config.hop_length = core_stft_hop(resolved.hop_length);

        if (!with_residual) {
          HpssAudioResult result =
              with_c_names(kStftOptionNames, [&] { return hpss(audio, config, stft_config); });
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

        HpssAudioResultWithResidual result = with_c_names(
            kStftOptionNames, [&] { return hpss_with_residual(audio, config, stft_config); });
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

void sonare_time_stretch_options_init(SonareTimeStretchOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = constants::kDefaultNFft;
}

SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate,
                                float speed_ratio, const SonareTimeStretchOptions* options,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("time_stretch", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonareTimeStretchOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_time_stretch_options_init,
                                              "sonare_time_stretch_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err =
          run_validation([&] { check_stft_hop(resolved.hop_length, "options.hop_length"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  TimeStretchConfig config;
  config.n_fft = resolved.n_fft;
  config.hop_length = core_stft_hop(resolved.hop_length);
  config.backend = StretchBackend::NativeSpectral;
  return run_mono_prevalidated(
      samples, length, sample_rate, out, out_length, [speed_ratio, config](const Audio& a) {
        return with_c_names(kTimeStretchNames,
                            [&] { return time_stretch(a, speed_ratio, config); });
      });
}

void sonare_pitch_shift_options_init(SonarePitchShiftOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = constants::kDefaultNFft;
}

SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float pitch_semitones, const SonarePitchShiftOptions* options,
                               float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("pitch_shift", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonarePitchShiftOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_pitch_shift_options_init,
                                              "sonare_pitch_shift_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err =
          run_validation([&] { check_stft_hop(resolved.hop_length, "options.hop_length"); });
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
  config.n_fft = resolved.n_fft;
  config.hop_length = core_stft_hop(resolved.hop_length);
  config.backend = StretchBackend::NativeSpectral;
  return run_mono_prevalidated(
      samples, length, sample_rate, out, out_length, [pitch_semitones, config](const Audio& a) {
        return with_c_names(kStftOptionNames,
                            [&] { return pitch_shift(a, pitch_semitones, config); });
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
  config->cutoff_hz = std::numeric_limits<float>::quiet_NaN();
  config->center_hz = std::numeric_limits<float>::quiet_NaN();
  config->bandwidth_hz = std::numeric_limits<float>::quiet_NaN();
}

namespace {

/// @brief Refuses the biquad filter configuration in member order.
void check_biquad_filter_config(const SonareBiquadFilterConfig& config, int sample_rate) {
  namespace check = sonare::util::check;
  sonare_c_detail::require_struct_size(config.struct_size, sizeof(config), "config",
                                       "sonare_biquad_filter_config_init");
  check::integer_within(config.type, kBiquadFilterTypeDomain, "config.type");
  check::one_of(config.order, std::array<int, 2>{2, 4}, "config.order");
  const bool corner =
      config.type == SONARE_BIQUAD_FILTER_HIGH_PASS || config.type == SONARE_BIQUAD_FILTER_LOW_PASS;
  const double nyquist = static_cast<double>(sample_rate) / 2.0;
  const auto unread = [](const char* subject, const char* types) {
    throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter, {subject, "config.type"},
                                           std::string("{0} does not apply when {1} is ") + types);
  };
  const auto required = [](float value, const char* subject, const char* types) {
    if (!std::isnan(value)) return;
    throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter, {subject, "config.type"},
                                           std::string("{0} must be given when {1} is ") + types);
  };
  if (corner) {
    required(config.cutoff_hz, "config.cutoff_hz", "highPass or lowPass");
    check::in_interval(config.cutoff_hz, 0.0, false, nyquist, false, "config.cutoff_hz");
    if (!std::isnan(config.center_hz)) unread("config.center_hz", "highPass or lowPass");
    if (!std::isnan(config.bandwidth_hz)) unread("config.bandwidth_hz", "highPass or lowPass");
  } else {
    if (!std::isnan(config.cutoff_hz)) unread("config.cutoff_hz", "bandPass or notch");
    required(config.center_hz, "config.center_hz", "bandPass or notch");
    required(config.bandwidth_hz, "config.bandwidth_hz", "bandPass or notch");
    check::in_interval(config.center_hz, 0.0, false, nyquist, false, "config.center_hz");
    check::greater_than(config.bandwidth_hz, 0.0, "config.bandwidth_hz");
    if (config.order != 2) {
      throw SonareParameterRefusal::relation(ErrorCode::InvalidParameter,
                                             {"config.order", "config.type"},
                                             "{0} must be 2 when {1} is bandPass or notch");
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
                                 int sample_rate, float** out_left, float** out_right,
                                 size_t* out_length, Fn&& normalize_pair) {
  SONARE_C_ZERO_OUTS(out_left, out_right, out_length);

  SonareError err =
      validate_stereo_audio_params(fn, left_name, right_name, left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_left);
  SONARE_C_REQUIRE_NONNULL(out_right);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  const auto result = normalize_pair(Audio::from_buffer(left, length, sample_rate),
                                     Audio::from_buffer(right, length, sample_rate));
  copy_stereo_channels(result.left.data(), result.right.data(), result.left.size(), out_left,
                       out_right, out_length);
  return SONARE_OK;
  SONARE_C_CATCH
}

}  // namespace

SonareError sonare_normalize_stereo(const float* left, const float* right, size_t length,
                                    int sample_rate, float target_db, float** out_left,
                                    float** out_right, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return run_normalize_stereo(
      "normalize_stereo", "left", "right", left, right, length, sample_rate, out_left, out_right,
      out_length,
      [target_db](const Audio& l, const Audio& r) { return normalize_stereo(l, r, target_db); });
}

SonareError sonare_normalize_rms_stereo(const float* left, const float* right, size_t length,
                                        int sample_rate, float target_db, float** out_left,
                                        float** out_right, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return run_normalize_stereo("normalize_rms_stereo", "left", "right", left, right, length,
                              sample_rate, out_left, out_right, out_length,
                              [target_db](const Audio& l, const Audio& r) {
                                return normalize_rms_stereo(l, r, target_db, true);
                              });
}

void sonare_trim_options_init(SonareTrimOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->frame_length = static_cast<size_t>(kDefaultSilenceFrameLength);
  options->hop_length = static_cast<size_t>(kDefaultSilenceHopLength);
}

SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        const SonareTrimOptions* options, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("trim", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonareTrimOptions resolved;
  if (const SonareError err =
          resolve_options(options, sonare_trim_options_init, "sonare_trim_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  const size_t frame_length = resolved.frame_length;
  const size_t hop_length = resolved.hop_length;
  if (const SonareError err = run_validation([&] {
        sonare::util::check::integer_within(frame_length, kSilenceFrameLengthDomain,
                                            "options.frame_length");
        sonare::util::check::integer_within(hop_length, kSilenceHopLengthDomain,
                                            "options.hop_length");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [threshold_db, frame_length, hop_length](const Audio& a) {
                                 return trim_absolute(a, threshold_db,
                                                      static_cast<int>(frame_length),
                                                      static_cast<int>(hop_length));
                               });
}

void sonare_decompose_options_init(SonareDecomposeOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, const SonareDecomposeOptions* options,
                             float** out_w, size_t* out_w_length, float** out_h,
                             size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_w, out_w_length, out_h, out_h_length);
  SONARE_C_REQUIRE_NONNULL(s);
  SonareDecomposeOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_decompose_options_init,
                                              "sonare_decompose_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  const std::string init = resolved.init != nullptr ? resolved.init : "random";
  if (const SonareError err = run_validation([&] {
        validate_decompose_shape(n_features, n_frames, n_components, n_iter);
        if (init != "random" && init != "nndsvd") {
          sonare::util::check::refuse("options.init", "must be \"random\" or \"nndsvd\"");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  // 0 selects the library default; the core would read it as "no iterations"
  // and return the raw init matrices, so it never reaches the core as 0.
  if (n_iter == 0) n_iter = kDecomposeDefaultIter;
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return run_validation([&] {
      sonare::util::check::refuse_relation({"n_features", "n_frames"},
                                           "the product of {0} and {1} is too large to address");
    });
  }
  SONARE_C_REQUIRE_NONNULL(out_w);
  SONARE_C_REQUIRE_NONNULL(out_w_length);
  SONARE_C_REQUIRE_NONNULL(out_h);
  SONARE_C_REQUIRE_NONNULL(out_h_length);
  // Non-finite elements of `s` are rejected by the core (decompose throws
  // InvalidParameter, which SONARE_C_CATCH maps to SONARE_ERROR_INVALID_PARAMETER).

  SONARE_C_TRY
  DecomposeResult result =
      decompose(s, n_features, n_frames, n_components, n_iter, "mu", beta, init);

  return copy_decompose_factors(result, out_w, out_w_length, out_h, out_h_length);
  SONARE_C_CATCH
}

SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(s);
  if (const SonareError err = run_validation([&] {
        sonare::util::check::integer_within(n_features, kDecomposeNFeaturesDomain, "n_features");
        sonare::util::check::integer_within(n_frames, kDecomposeNFramesDomain, "n_frames");
      });
      err != SONARE_OK) {
    return err;
  }
  // Reject dims whose product would overflow size_t before the core indexes
  // n_features * n_frames elements of the caller-owned buffer.
  if (static_cast<size_t>(n_features) > SIZE_MAX / static_cast<size_t>(n_frames)) {
    return run_validation([&] {
      sonare::util::check::refuse_relation({"n_features", "n_frames"},
                                           "the product of {0} and {1} is too large to address");
    });
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

void sonare_decompose_stems_config_init(SonareDecomposeStemsConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  const DecomposeStemsConfig defaults;
  config->n_components = defaults.n_components;
  config->n_fft = defaults.n_fft;
  config->hop_length = 0;
  config->n_iter = defaults.n_iter;
  config->beta = defaults.beta;
  config->init = "random";
  config->mask_power = defaults.mask_power;
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
  DecomposeStemsConfig core_config;
  if (config != nullptr) {
    if (const SonareError err =
            run_validation([&] { fill_decompose_stems_config(*config, &core_config); });
        err != SONARE_OK) {
      return err;
    }
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_component_count);
  SONARE_C_REQUIRE_NONNULL(out_component_length);
  // An optional pointer given without its length has nowhere to write the length.
  if (const SonareError err = check_factor_output_pairs(out_w, out_w_length, out_h, out_h_length);
      err != SONARE_OK) {
    return err;
  }

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        DecomposeStemsResult result =
            decompose_stems(audio.data(), audio.size(), sample_rate, core_config);
        if (result.components.empty()) return SONARE_OK;
        const size_t component_length = result.components.front().size();
        if (component_length == 0) return SONARE_OK;
        if (result.components.size() > SIZE_MAX / component_length) {
          return run_validation([&] {
            sonare::util::check::refuse("length", "is too large to hold the decomposed components");
          });
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
                                          size_t* out_component_count, size_t* out_component_length,
                                          float** out_w, size_t* out_w_length, float** out_h,
                                          size_t* out_h_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_component_count, out_component_length, out_w, out_w_length, out_h,
                     out_h_length);
  if (const SonareError err = run_validation([&] {
        sonare::util::check::non_null(channels, "channels");
        sonare::util::check::integer_within(
            static_cast<long long>(
                std::min<size_t>(num_channels, kMaxDecomposeStemsLinkedChannels + 1)),
            kDecomposeStemsLinkedChannelsDomain, "num_channels");
      });
      err != SONARE_OK) {
    return err;
  }
  for (size_t c = 0; c < num_channels; ++c) {
    SonareError err = validate_audio_params("decompose_stems_linked", "channels", channels[c],
                                            length, sample_rate);
    if (err != SONARE_OK) return err;
  }

  SONARE_C_TRY
  DecomposeStemsConfig core_config;
  if (config != nullptr) fill_decompose_stems_config(*config, &core_config);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_component_count);
  SONARE_C_REQUIRE_NONNULL(out_component_length);
  // Same output-pair guard as sonare_decompose_stems.
  if (const SonareError err = check_factor_output_pairs(out_w, out_w_length, out_h, out_h_length);
      err != SONARE_OK) {
    return err;
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
  const char* const too_large = "{0} channels of {1} samples are too large to hold the components";
  if (num_channels > SIZE_MAX / component_length) {
    return run_validation(
        [&] { sonare::util::check::refuse_relation({"num_channels", "length"}, too_large); });
  }
  const size_t per_component = num_channels * component_length;
  if (result.components.size() > SIZE_MAX / per_component) {
    return run_validation(
        [&] { sonare::util::check::refuse_relation({"num_channels", "length"}, too_large); });
  }

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

SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate,
                                 float speed_ratio, int n_fft, size_t hop_length, float** out,
                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          validate_audio_params("phase_vocoder", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        validate_stretch_rate(speed_ratio);
        sonare::util::check::even_within(n_fft, kColaNFftDomain, "n_fft");
        check_stft_hop(hop_length, "hop_length");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        const int hop = resolve_resynthesis_hop(core_stft_hop(hop_length), n_fft);
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

void sonare_spectral_edit_config_init(SonareSpectralEditConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  const SpectralEditConfig defaults;
  config->n_fft = defaults.n_fft;
  config->hop_length = 0;
  config->window = SONARE_WINDOW_HANN;
  config->heal_radius_frames = defaults.heal_radius_frames;
}

void sonare_spectral_region_op_init(SonareSpectralRegionOp* op) {
  if (op == nullptr) return;
  *op = {};
  op->struct_size = sizeof(*op);
  op->high_hz = std::numeric_limits<float>::quiet_NaN();
  op->mode = SONARE_SPECTRAL_EDIT_MODE_GAIN;
}

namespace {

// The core spells the STFT framing bare; here they are members of `config`.
const std::array<CName, 2> kSpectralEditNames{
    {{"n_fft", "config.n_fft"}, {"hop_length", "config.hop_length"}}};

}  // namespace

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
    if (const SonareError err = run_validation([&] {
          namespace check = sonare::util::check;
          require_struct_size(config->struct_size, sizeof(*config), "config",
                              "sonare_spectral_edit_config_init");
          check::power_of_two_within(config->n_fft, sonare::kSpectralEditNFftDomain,
                                     "config.n_fft");
          check_stft_hop(config->hop_length, "config.hop_length");
          check::integer_within(config->heal_radius_frames, kSpectralEditHealRadiusDomain,
                                "config.heal_radius_frames");
          check::integer_within(config->window, kSpectralEditWindowDomain, "config.window");
        });
        err != SONARE_OK) {
      return err;
    }
    core_config.n_fft = config->n_fft;
    core_config.hop_length = core_stft_hop(config->hop_length);
    core_config.window = to_window_type(config->window);
    core_config.heal_radius_frames = config->heal_radius_frames;
  }

  // ops may be NULL iff there are no ops (identity transform).
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(ops, n_ops);

  std::vector<SpectralRegionOp> core_ops(n_ops);
  for (size_t i = 0; i < n_ops; ++i) {
    if (const SonareError err = run_validation([&] {
          const std::string subject = "ops[" + std::to_string(i) + "]";
          require_struct_size(ops[i].struct_size, sizeof(SonareSpectralRegionOp), subject,
                              "sonare_spectral_region_op_init");
          if (!std::isnan(ops[i].high_hz)) {
            sonare::util::check::greater_than(ops[i].high_hz, 0.0, (subject + ".high_hz").c_str());
          }
          sonare::util::check::integer_within(ops[i].mode, kSpectralEditModeDomain,
                                              (subject + ".mode").c_str());
        });
        err != SONARE_OK) {
      return err;
    }
    core_ops[i].mode = admitted_ordinal_to_enum<SpectralEditMode>(ops[i].mode);
    core_ops[i].start_sample = ops[i].start_sample;
    core_ops[i].end_sample = ops[i].end_sample;
    core_ops[i].low_hz = ops[i].low_hz;
    // The core reads a high edge of 0 or less as nyquist.
    core_ops[i].high_hz = std::isnan(ops[i].high_hz) ? 0.0f : ops[i].high_hz;
    core_ops[i].gain_db = ops[i].gain_db;
  }

  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_mono_prevalidated(samples, length, sample_rate, out, out_length,
                               [&](const Audio& audio) -> Audio {
                                 return with_c_names(kSpectralEditNames, [&] {
                                   return spectral_edit(audio, core_config, core_ops.data(), n_ops);
                                 });
                               });
  SONARE_C_CATCH
}
