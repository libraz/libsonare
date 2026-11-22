/// @file sonare_c_editing.cpp
/// @brief C API entry points for ScaleQuantizer + offline metering (basic /
///        true peak / clipping / dynamic range / stereo / phase-scope /
///        spectrum). Implementations stay thin: they validate inputs, build
///        the C++ value type, and forward to the underlying module.
///        Heap-allocated result fields are released through dedicated
///        `sonare_free_*_result` entry points to mirror the rest of the C API.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "analysis/key_profiles.h"
#include "core/audio.h"
#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/pitch_editor/scale_quantizer.h"
#endif
#include "editing_input_domains.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/phase_scope.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "metering/waveform.h"
#include "rt/true_peak_fir.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"
#include "util/math_utils.h"
#include "util/parameter_checks.h"

using namespace sonare;
using namespace sonare_c_detail;
using sonare_c_mastering_detail::validate_stereo_audio_params;

namespace {

SonareError fill_clipping_result(const metering::ClippingResult& result,
                                 SonareClippingResult* out) {
  out->clipped_samples = result.clipped_samples;
  out->clipping_ratio = result.clipping_ratio;
  out->max_clipped_peak = result.max_clipped_peak;
  out->region_count = result.regions.size();
  if (result.regions.empty()) return SONARE_OK;
  std::unique_ptr<SonareClippingRegion[]> tmp(new SonareClippingRegion[result.regions.size()]);
  for (size_t i = 0; i < result.regions.size(); ++i) {
    tmp[i].start_sample = static_cast<int64_t>(result.regions[i].start_sample);
    tmp[i].end_sample = static_cast<int64_t>(result.regions[i].end_sample);
    tmp[i].length = result.regions[i].length;
    tmp[i].peak = result.regions[i].peak;
  }
  out->regions = release_array(tmp);
  return SONARE_OK;
}

SonareError fill_dynamic_range_result(const metering::DynamicRangeResult& result,
                                      SonareDynamicRangeResult* out) {
  sonare_c_detail::StagedOutput<SonareDynamicRangeResult, &sonare_free_dynamic_range_result> staged;
  staged->dynamic_range_db = result.dynamic_range_db;
  staged->low_percentile_db = result.low_percentile_db;
  staged->high_percentile_db = result.high_percentile_db;
  staged->window_count = result.window_rms_db.size();
  staged->window_rms_db = copy_vector(result.window_rms_db);
  staged.publish(out);
  return SONARE_OK;
}

// The oversample factor a true-peak entry runs, 0 selecting the meter's default; a factor the
// core has no polyphase design for is refused here, ahead of the audio checks.
int true_peak_factor(int oversample_factor) {
  if (oversample_factor == 0) return metering::kDefaultTruePeakOversample;
  if (!rt::is_supported_polyphase_oversample_factor(oversample_factor)) {
    util::check::one_of(oversample_factor, std::array<int, 5>{1, 2, 4, 8, 16}, "oversample_factor");
  }
  return oversample_factor;
}

}  // namespace

SonareError sonare_metering_peak_db(const float* samples, size_t length, int sample_rate,
                                    float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_REQUIRE_NONNULL(out_db);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       *out_db = metering::peak_db(audio);
                       return SONARE_OK;
                     });
}

SonareError sonare_metering_rms_db(const float* samples, size_t length, int sample_rate,
                                   float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_REQUIRE_NONNULL(out_db);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       *out_db = metering::rms_db(audio);
                       return SONARE_OK;
                     });
}

namespace {

SonareError validate_silence_ratio_params(float threshold_db, size_t frame_length,
                                          size_t hop_length) {
  return run_validation([&] {
    util::check::finite_float(threshold_db, "threshold_db");
    util::check::integer_within(frame_length, metering::kSilenceRatioFrameLengthDomain,
                                "frame_length");
    util::check::integer_within(hop_length, metering::kSilenceRatioHopLengthDomain, "hop_length");
  });
}

}  // namespace

SonareError sonare_metering_silence_ratio(const float* samples, size_t length, int sample_rate,
                                          float threshold_db, size_t frame_length,
                                          size_t hop_length, float* out_ratio) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_ratio);
  if (const SonareError err = validate_silence_ratio_params(threshold_db, frame_length, hop_length);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_ratio);
  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        *out_ratio = metering::silence_ratio(audio, threshold_db, static_cast<int>(frame_length),
                                             static_cast<int>(hop_length));
        return SONARE_OK;
      });
}

SonareError sonare_metering_crest_factor_db(const float* samples, size_t length, int sample_rate,
                                            float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_REQUIRE_NONNULL(out_db);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       *out_db = metering::crest_factor_db(audio);
                       return SONARE_OK;
                     });
}

SonareError sonare_metering_crest_factor_db_stereo(const float* left, const float* right,
                                                   size_t length, int sample_rate, float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SonareError err = validate_stereo_audio_params("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_db);

  SONARE_C_TRY
  std::vector<float> interleaved(length * 2);
  for (size_t index = 0; index < length; ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
  }
  *out_db = metering::crest_factor_db_interleaved(interleaved.data(), length, 2);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_metering_dc_offset(const float* samples, size_t length, int sample_rate,
                                      float* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_value);
  SONARE_C_REQUIRE_NONNULL(out_value);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       *out_value = metering::dc_offset(audio);
                       return SONARE_OK;
                     });
}

SonareError sonare_metering_true_peak_dbtp(const float* samples, size_t length, int sample_rate,
                                           int oversample_factor, float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_TRY
  const int factor = true_peak_factor(oversample_factor);
  SONARE_C_REQUIRE_NONNULL(out_db);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       *out_db = metering::true_peak_db(audio, factor);
                       return SONARE_OK;
                     });
  SONARE_C_CATCH
}

SonareError sonare_metering_detect_clipping(const float* samples, size_t length, int sample_rate,
                                            float threshold_db, size_t min_region_samples,
                                            SonareClippingResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(out);
  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        return fill_clipping_result(
            metering::detect_clipping(audio, metering::clipping_threshold_from_db(threshold_db),
                                      min_region_samples),
            out);
      });
}

void sonare_free_clipping_result(SonareClippingResult* result) {
  if (!result) return;
  delete[] result->regions;
  *result = {};
}

SonareError sonare_metering_dynamic_range(const float* samples, size_t length, int sample_rate,
                                          float window_sec, float hop_sec, float low_percentile,
                                          float high_percentile, SonareDynamicRangeResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  // window_sec / hop_sec keep the "0 = library default" convention (a 0-second
  // window is meaningless). The percentiles instead use a NEGATIVE sentinel for
  // "default" so that 0.0 is a real request for the 0th percentile (the true
  // minimum-RMS window); 0.0 previously meant "default" and made the 0th
  // percentile unreachable.
  SONARE_C_TRY
  const metering::DynamicRangeConfig cfg = metering::dynamic_range_config_from_public(
      window_sec, hop_sec, low_percentile, high_percentile);
  SONARE_C_REQUIRE_NONNULL(out);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       return fill_dynamic_range_result(metering::dynamic_range(audio, cfg), out);
                     });
  SONARE_C_CATCH
}

void sonare_free_dynamic_range_result(SonareDynamicRangeResult* result) {
  if (!result) return;
  delete[] result->window_rms_db;
  *result = {};
}

namespace {

SonareError validate_stereo_pair(const char* left_name, const char* right_name, const float* left,
                                 const float* right, size_t length, int sample_rate) {
  if (!left) return refuse_null(left_name);
  if (!right) return refuse_null(right_name);
  // Validate both channels symmetrically: validate_audio_params checks the
  // pointer, length bounds, sample rate, and NaN/Inf samples. Checking only the
  // left channel would let bad right-channel data reach the metering callers.
  SonareError err = validate_audio_params(left_name, left, length, sample_rate);
  if (err != SONARE_OK) return err;
  return validate_audio_params(right_name, right, length, sample_rate);
}

}  // namespace

SonareError sonare_metering_stereo_correlation(const float* left, const float* right, size_t length,
                                               int sample_rate, float* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_value);
  SonareError err = validate_stereo_pair("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_value);
  SONARE_C_TRY
  *out_value = metering::correlation(left, right, length);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_metering_stereo_width(const float* left, const float* right, size_t length,
                                         int sample_rate, float* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_value);
  SonareError err = validate_stereo_pair("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_value);
  SONARE_C_TRY
  *out_value = metering::stereo_width(left, right, length);
  return SONARE_OK;
  SONARE_C_CATCH
}

namespace {

SonareError fill_vectorscope_result(const std::vector<metering::VectorscopePoint>& points,
                                    SonareVectorscopeResult* out) {
  out->point_count = points.size();
  if (!points.empty()) {
    std::unique_ptr<SonareVectorscopePoint[]> tmp(new SonareVectorscopePoint[points.size()]);
    for (size_t i = 0; i < points.size(); ++i) {
      tmp[i].mid = points[i].mid;
      tmp[i].side = points[i].side;
    }
    out->points = release_array(tmp);
  }
  return SONARE_OK;
}

}  // namespace

SonareError sonare_metering_vectorscope(const float* left, const float* right, size_t length,
                                        int sample_rate, size_t max_points,
                                        SonareVectorscopeResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareError err = validate_stereo_pair("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_vectorscope_result(metering::vectorscope(left, right, length, max_points), out);
  SONARE_C_CATCH
}

void sonare_free_vectorscope_result(SonareVectorscopeResult* result) {
  if (!result) return;
  delete[] result->points;
  result->points = nullptr;
  result->point_count = 0;
}

namespace {

SonareError fill_phase_scope_result(const metering::PhaseScopeResult& result,
                                    SonarePhaseScopeResult* out) {
  out->correlation = result.correlation;
  out->average_abs_angle_rad = result.average_abs_angle_rad;
  out->max_radius = result.max_radius;
  out->point_count = result.points.size();
  if (!result.points.empty()) {
    std::unique_ptr<SonarePhaseScopePoint[]> tmp(new SonarePhaseScopePoint[result.points.size()]);
    for (size_t i = 0; i < result.points.size(); ++i) {
      tmp[i].mid = result.points[i].mid;
      tmp[i].side = result.points[i].side;
      tmp[i].radius = result.points[i].radius;
      tmp[i].angle_rad = result.points[i].angle_rad;
    }
    out->points = release_array(tmp);
  }
  return SONARE_OK;
}

}  // namespace

SonareError sonare_metering_phase_scope(const float* left, const float* right, size_t length,
                                        int sample_rate, size_t max_points,
                                        SonarePhaseScopeResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareError err = validate_stereo_pair("left", "right", left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_phase_scope_result(metering::phase_scope(left, right, length, max_points), out);
  SONARE_C_CATCH
}

void sonare_free_phase_scope_result(SonarePhaseScopeResult* result) {
  if (!result) return;
  delete[] result->points;
  *result = {};
}

namespace {

// Decode the shared spectrum config knobs (n_fft / smoothing / dB) from the C
// args, applying the "0 = default, anything else outside the domain = error"
// convention. Returns false (and the error code via *err) on an invalid value.
bool decode_spectrum_config(int n_fft, int apply_octave_smoothing, int octave_fraction,
                            float db_ref, float db_amin, metering::SpectrumConfig* cfg,
                            SonareError* err) {
  // A `< 0` test would leave NaN and +Inf to the `> 0` substitution below.
  if (n_fft < 0 || octave_fraction < 0 || !numeric::finite_non_negative(db_ref) ||
      !numeric::finite_non_negative(db_amin)) {
    *err = SONARE_ERROR_INVALID_PARAMETER;
    return false;
  }
  if (n_fft > 0) cfg->n_fft = n_fft;
  if (!is_power_of_two(cfg->n_fft)) {
    *err = SONARE_ERROR_INVALID_PARAMETER;
    return false;
  }
  cfg->apply_octave_smoothing = apply_octave_smoothing != 0;
  if (octave_fraction > 0) cfg->octave_fraction = octave_fraction;
  if (db_ref > 0.0f) cfg->db_ref = db_ref;
  if (db_amin > 0.0f) cfg->db_amin = db_amin;
  return true;
}

SonareError fill_spectrum_result(const metering::SpectrumResult& result,
                                 SonareSpectrumResult* out) {
  sonare_c_detail::StagedOutput<SonareSpectrumResult, &sonare_free_spectrum_result> staged;
  staged->n_fft = result.n_fft;
  staged->sample_rate = result.sample_rate;
  staged->bin_count = result.frequencies.size();
  if (staged->bin_count != 0) {
    staged->frequencies = copy_vector(result.frequencies);
    staged->magnitude = copy_vector(result.magnitude);
    staged->power = copy_vector(result.power);
    staged->db = copy_vector(result.db);
  }
  staged.publish(out);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_metering_spectrum(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int apply_octave_smoothing, int octave_fraction,
                                     float db_ref, float db_amin, SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       return fill_spectrum_result(metering::spectrum(audio, cfg), out);
                     });
}

SonareError sonare_metering_spectrum_frame(const float* samples, size_t length, int sample_rate,
                                           size_t frame_offset, int n_fft,
                                           int apply_octave_smoothing, int octave_fraction,
                                           float db_ref, float db_amin, SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_TRY
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  // Only [frame_offset, frame_offset + n_fft) ever reaches the FFT, so that window
  // is both the only span copied into an Audio and the only span scanned for
  // non-finite samples. Never form frame_offset + n_fft directly -- it overflows
  // size_t when frame_offset is caller-supplied garbage.
  const size_t start = std::min(frame_offset, length);
  const size_t count = std::min(static_cast<size_t>(cfg.n_fft), length - start);
  err = validate_audio_params_window("samples", samples, length, sample_rate, start, count);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);

  const auto run_frame = [&](const Audio& window_audio) -> SonareError {
    return fill_spectrum_result(metering::spectrum_frame(window_audio, 0, cfg), out);
  };
  if (count == 0) {
    // frame_offset is at or past the buffer's end. The full-length path would
    // have windowed an all-zero frame here too; a genuinely empty Audio would
    // instead take spectrum_frame's own empty-input shortcut, which returns a
    // fixed dB floor regardless of db_amin/db_ref. A single zero sample keeps
    // this on the same finalize_spectrum() path the in-bounds case takes.
    static constexpr float kZeroSample = 0.0f;
    return run_prevalidated_offline(&kZeroSample, 1, sample_rate, run_frame);
  }
  return run_prevalidated_offline(samples + start, count, sample_rate, run_frame);
  SONARE_C_CATCH
}

void sonare_free_spectrum_result(SonareSpectrumResult* result) {
  if (!result) return;
  delete[] result->frequencies;
  delete[] result->magnitude;
  delete[] result->power;
  delete[] result->db;
  *result = {};
}

// ============================================================================
// Handle-form metering
// ============================================================================
// A SonareAudio already owns samples that passed validate_audio_params at
// construction, so these skip the scan and the copy run_offline performs and
// measure audio->audio in place. Parameter decoding is shared with the buffer
// forms rather than restated, so a sentinel can only change in one place.

SonareError sonare_audio_peak_db(const SonareAudio* audio, float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_db);
  SONARE_C_TRY
  *out_db = metering::peak_db(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_rms_db(const SonareAudio* audio, float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_db);
  SONARE_C_TRY
  *out_db = metering::rms_db(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_dc_offset(const SonareAudio* audio, float* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_value);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_value);
  SONARE_C_TRY
  *out_value = metering::dc_offset(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_crest_factor_db(const SonareAudio* audio, float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_db);
  SONARE_C_TRY
  *out_db = metering::crest_factor_db(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_silence_ratio(const SonareAudio* audio, float threshold_db,
                                       size_t frame_length, size_t hop_length, float* out_ratio) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_ratio);
  SONARE_C_REQUIRE_NONNULL(audio);
  if (const SonareError err = validate_silence_ratio_params(threshold_db, frame_length, hop_length);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_ratio);
  SONARE_C_TRY
  *out_ratio = metering::silence_ratio(audio->audio, threshold_db, static_cast<int>(frame_length),
                                       static_cast<int>(hop_length));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_true_peak_dbtp(const SonareAudio* audio, int oversample_factor,
                                        float* out_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_db);
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(audio);
  const int factor = true_peak_factor(oversample_factor);
  SONARE_C_REQUIRE_NONNULL(out_db);
  *out_db = metering::true_peak_db(audio->audio, factor);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_clipping(const SonareAudio* audio, float threshold_db,
                                         size_t min_region_samples, SonareClippingResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_TRY
  const float threshold = metering::clipping_threshold_from_db(threshold_db);
  SONARE_C_REQUIRE_NONNULL(out);
  return fill_clipping_result(
      metering::detect_clipping(audio->audio, threshold, min_region_samples), out);
  SONARE_C_CATCH
}

SonareError sonare_audio_dynamic_range(const SonareAudio* audio, float window_sec, float hop_sec,
                                       float low_percentile, float high_percentile,
                                       SonareDynamicRangeResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_TRY
  const metering::DynamicRangeConfig cfg = metering::dynamic_range_config_from_public(
      window_sec, hop_sec, low_percentile, high_percentile);
  SONARE_C_REQUIRE_NONNULL(out);
  return fill_dynamic_range_result(metering::dynamic_range(audio->audio, cfg), out);
  SONARE_C_CATCH
}

SonareError sonare_audio_spectrum(const SonareAudio* audio, int n_fft, int apply_octave_smoothing,
                                  int octave_fraction, float db_ref, float db_amin,
                                  SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(audio);
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_spectrum_result(metering::spectrum(audio->audio, cfg), out);
  SONARE_C_CATCH
}

SonareError sonare_audio_spectrum_frame(const SonareAudio* audio, size_t frame_offset, int n_fft,
                                        int apply_octave_smoothing, int octave_fraction,
                                        float db_ref, float db_amin, SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(audio);
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  // The core reads the frame at the offset and zero-pads past the end, so unlike
  // the buffer form there is no window to copy and no out-of-range offset to
  // special-case.
  SONARE_C_TRY
  return fill_spectrum_result(metering::spectrum_frame(audio->audio, frame_offset, cfg), out);
  SONARE_C_CATCH
}

namespace {

SonareError validate_interleaved_audio(const float* samples, size_t frames, int num_channels) {
  return run_validation([&] {
    util::check::integer_within(num_channels, kInterleavedChannelsDomain, "num_channels");
    util::check::non_null_when_count(samples, frames, "samples", "frames");
    if (frames > std::numeric_limits<size_t>::max() / static_cast<size_t>(num_channels)) {
      util::check::refuse_relation({"frames", "num_channels"},
                                   "{0} * {1} must not exceed the addressable sample count");
    }
  });
}

SonareError fill_waveform_peaks_result(const metering::WaveformPeaksResult& result,
                                       SonareWaveformPeaksResult* out) {
  out->num_channels = result.channels;
  out->bucket_count = result.bucket_count;
  out->samples_per_bucket = result.samples_per_bucket;
  const size_t total = static_cast<size_t>(result.channels) * result.bucket_count;
  if (total == 0) return SONARE_OK;
  out->min = copy_vector(result.min);
  out->max = copy_vector(result.max);
  return SONARE_OK;
}

struct WaveformPyramidLevels {
  std::unique_ptr<SonareWaveformPeaksResult[]> levels;
  size_t initialized = 0;

  explicit WaveformPyramidLevels(size_t count) : levels(new SonareWaveformPeaksResult[count]()) {}

  ~WaveformPyramidLevels() {
    if (!levels) return;
    for (size_t i = 0; i < initialized; ++i) {
      sonare_free_waveform_peaks_result(&levels[i]);
    }
  }

  SonareWaveformPeaksResult* get() noexcept { return levels.get(); }

  SonareWaveformPeaksResult* release() noexcept {
    initialized = 0;
    return release_array(levels);
  }
};

}  // namespace

SonareError sonare_waveform_peaks(const float* samples, size_t frames, int num_channels,
                                  size_t samples_per_bucket, SonareWaveformPeaksResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareError err = validate_interleaved_audio(samples, frames, num_channels);
  if (err != SONARE_OK) return err;
  if (const SonareError spb_err =
          run_validation([&] { metering::validate_samples_per_bucket(samples_per_bucket); });
      spb_err != SONARE_OK) {
    return spb_err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_waveform_peaks_result(
      metering::waveform_peaks(samples, frames, num_channels, samples_per_bucket), out);
  SONARE_C_CATCH
}

SonareError sonare_waveform_peak_pyramid(const float* samples, size_t frames, int num_channels,
                                         const size_t* samples_per_bucket_levels,
                                         size_t level_count, SonareWaveformPeakPyramidResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_TRY
  SonareError err = validate_interleaved_audio(samples, frames, num_channels);
  if (err != SONARE_OK) return err;
  util::check::integer_at_least(level_count, 1, "level_count");
  util::check::non_null(samples_per_bucket_levels, "samples_per_bucket_levels");
  std::vector<size_t> levels(samples_per_bucket_levels, samples_per_bucket_levels + level_count);
  for (size_t level : levels) {
    metering::validate_samples_per_bucket(level);
  }
  SONARE_C_REQUIRE_NONNULL(out);
  const auto pyramid = metering::waveform_peak_pyramid(samples, frames, num_channels, levels);
  WaveformPyramidLevels c_levels(pyramid.size());
  for (size_t i = 0; i < pyramid.size(); ++i) {
    err = fill_waveform_peaks_result(pyramid[i], &c_levels.get()[i]);
    if (err != SONARE_OK) {
      return err;
    }
    ++c_levels.initialized;
  }
  out->level_count = pyramid.size();
  out->levels = c_levels.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_waveform_peaks_result(SonareWaveformPeaksResult* result) {
  if (!result) return;
  delete[] result->min;
  delete[] result->max;
  result->min = nullptr;
  result->max = nullptr;
  result->num_channels = 0;
  result->bucket_count = 0;
  result->samples_per_bucket = 0;
}

void sonare_free_waveform_peak_pyramid_result(SonareWaveformPeakPyramidResult* result) {
  if (!result) return;
  for (size_t i = 0; i < result->level_count; ++i) {
    sonare_free_waveform_peaks_result(&result->levels[i]);
  }
  delete[] result->levels;
  result->levels = nullptr;
  result->level_count = 0;
}

namespace {

#if defined(SONARE_WITH_PITCH_EDITOR)
editing::pitch_editor::ScaleQuantizerConfig make_scale_config(int root, uint16_t mode_mask,
                                                              float reference_midi) {
  editing::pitch_editor::ScaleQuantizerConfig cfg;
  cfg.root = root;
  cfg.mode_mask = mode_mask;
  // NaN is "not given": the quantizer's own reference stands.
  if (!std::isnan(reference_midi)) {
    util::check::in_range(reference_midi, 0.0, editing::pitch_editor::kMaxReferenceMidi,
                          "scale_reference_midi");
    cfg.reference_midi = reference_midi;
  }
  return cfg;
}

SonareError validate_scale_args(int root, uint16_t mode_mask) {
  return run_validation([&] {
    util::check::integer_within(root, editing::pitch_editor::kScaleRootDomain, "root");
    util::check::integer_within(mode_mask, editing::pitch_editor::kScaleModeMaskDomain,
                                "mode_mask");
  });
}
#endif

}  // namespace

SonareError sonare_scale_quantize_midi(int root, uint16_t mode_mask, float scale_reference_midi,
                                       float midi, float* out_quantized_midi) {
  SONARE_C_API_ENTRY;
  // Zeroed before the gate: the stub below returns without reaching the real body.
  SONARE_C_ZERO_OUTS(out_quantized_midi);
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (const SonareError err = validate_scale_args(root, mode_mask); err != SONARE_OK) return err;
  if (const SonareError err = run_validation([&] { util::check::finite_float(midi, "midi"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_quantized_midi);
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, scale_reference_midi));
  *out_quantized_midi = q.quantize_midi(midi);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out_quantized_midi);
  return refuse_not_compiled_in(Feature::kPitchEditor, root, mode_mask, scale_reference_midi, midi,
                                out_quantized_midi);
#endif
}

SonareError sonare_scale_correction_semitones(int root, uint16_t mode_mask,
                                              float scale_reference_midi, float midi,
                                              float* out_semitones) {
  SONARE_C_API_ENTRY;
  // Zeroed before the gate: the stub below returns without reaching the real body.
  SONARE_C_ZERO_OUTS(out_semitones);
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (const SonareError err = validate_scale_args(root, mode_mask); err != SONARE_OK) return err;
  if (const SonareError err = run_validation([&] { util::check::finite_float(midi, "midi"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_semitones);
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, scale_reference_midi));
  *out_semitones = q.correction_semitones(midi);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out_semitones);
  return refuse_not_compiled_in(Feature::kPitchEditor, root, mode_mask, scale_reference_midi, midi,
                                out_semitones);
#endif
}

SonareError sonare_scale_pitch_class_enabled(int root, uint16_t mode_mask, int pitch_class,
                                             int* out_enabled) {
  SONARE_C_API_ENTRY;
  // Zeroed before the gate: the stub below returns without reaching the real body.
  SONARE_C_ZERO_OUTS(out_enabled);
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (const SonareError err = validate_scale_args(root, mode_mask); err != SONARE_OK) return err;
  if (const SonareError err = run_validation(
          [&] { util::check::integer_within(pitch_class, kScalePitchClassDomain, "pitch_class"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_enabled);
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, 0.0f));
  *out_enabled = q.pitch_class_enabled(pitch_class) ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out_enabled);
  return refuse_not_compiled_in(Feature::kPitchEditor, root, mode_mask, pitch_class, out_enabled);
#endif
}

SonareError sonare_scale_mask_for_mode(int root, SonareMode mode, uint16_t* out_mask) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_mask);
  SONARE_C_TRY
  util::check::integer_within(root, kScaleMaskRootDomain, "root");
  util::check::integer_in_range(mode, SONARE_MODE_MAJOR, SONARE_MODE_LOCRIAN, "mode");
  const Mode core_mode = admitted_ordinal_to_enum<Mode>(mode);
  SONARE_C_REQUIRE_NONNULL(out_mask);
  *out_mask = scale_mask_for_mode(core_mode);
  return SONARE_OK;
  SONARE_C_CATCH
}
