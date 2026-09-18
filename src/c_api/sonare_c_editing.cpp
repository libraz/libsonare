/// @file sonare_c_editing.cpp
/// @brief C API entry points for ScaleQuantizer + offline metering (basic /
///        true peak / clipping / dynamic range / stereo / phase-scope /
///        spectrum). Implementations stay thin: they validate inputs, build
///        the C++ value type, and forward to the underlying module.
///        Heap-allocated result fields are released through dedicated
///        `sonare_free_*_result` entry points to mirror the rest of the C API.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "core/audio.h"
#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/pitch_editor/scale_quantizer.h"
#endif
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/phase_scope.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "metering/waveform.h"
#include "sonare_c_internal.h"
#include "util/zero_is_default.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

bool is_power_of_two(int v) { return v > 0 && (v & (v - 1)) == 0; }

SonareError fill_clipping_result(const metering::ClippingResult& result,
                                 SonareClippingResult* out) {
  out->clipped_samples = result.clipped_samples;
  out->clipping_ratio = result.clipping_ratio;
  out->max_clipped_peak = result.max_clipped_peak;
  out->region_count = result.regions.size();
  if (result.regions.empty()) return SONARE_OK;
  std::unique_ptr<SonareClippingRegion[]> tmp(new SonareClippingRegion[result.regions.size()]);
  for (size_t i = 0; i < result.regions.size(); ++i) {
    tmp[i].start_sample = result.regions[i].start_sample;
    tmp[i].end_sample = result.regions[i].end_sample;
    tmp[i].length = result.regions[i].length;
    tmp[i].peak = result.regions[i].peak;
  }
  out->regions = release_array(tmp);
  return SONARE_OK;
}

SonareError fill_dynamic_range_result(const metering::DynamicRangeResult& result,
                                      SonareDynamicRangeResult* out) {
  out->dynamic_range_db = result.dynamic_range_db;
  out->low_percentile_db = result.low_percentile_db;
  out->high_percentile_db = result.high_percentile_db;
  out->window_count = result.window_rms_db.size();
  out->window_rms_db = copy_vector(result.window_rms_db);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_metering_peak_db(const float* samples, size_t length, int sample_rate,
                                    float* out_db) {
  SONARE_C_API_ENTRY;
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::peak_db(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_rms_db(const float* samples, size_t length, int sample_rate,
                                   float* out_db) {
  SONARE_C_API_ENTRY;
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::rms_db(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_silence_ratio(const float* samples, size_t length, int sample_rate,
                                          float threshold_db, int frame_length, int hop_length,
                                          float* out_ratio) {
  SONARE_C_API_ENTRY;
  if (!out_ratio || !std::isfinite(threshold_db) || frame_length <= 0 || hop_length <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_ratio = metering::silence_ratio(audio, threshold_db, frame_length, hop_length);
    return SONARE_OK;
  });
}

SonareError sonare_metering_crest_factor_db(const float* samples, size_t length, int sample_rate,
                                            float* out_db) {
  SONARE_C_API_ENTRY;
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::crest_factor_db(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_crest_factor_db_stereo(const float* left, const float* right,
                                                   size_t length, int sample_rate, float* out_db) {
  SONARE_C_API_ENTRY;
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  *out_db = 0.0f;
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;

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
  if (!out_value) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_value = metering::dc_offset(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_true_peak_db(const float* samples, size_t length, int sample_rate,
                                         int oversample_factor, float* out_db) {
  SONARE_C_API_ENTRY;
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  *out_db = 0.0f;
  int factor = oversample_factor == 0 ? 4 : oversample_factor;
  if (factor < 1 || factor > 16 || !is_power_of_two(factor)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::true_peak_db(audio, factor);
    return SONARE_OK;
  });
}

SonareError sonare_metering_detect_clipping(const float* samples, size_t length, int sample_rate,
                                            float threshold, size_t min_region_samples,
                                            SonareClippingResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    // Only the documented sentinel (0) selects the library default; any other
    // out-of-domain threshold throws here and run_offline maps it to
    // SONARE_ERROR_INVALID_PARAMETER, so a negative request is refused exactly
    // like the equally out-of-domain 1.5 always was.
    const metering::ClippingParams params =
        metering::clipping_params_from_public(threshold, min_region_samples);
    return fill_clipping_result(
        metering::detect_clipping(audio, params.threshold, params.min_region_samples), out);
  });
}

void sonare_free_clipping_result(SonareClippingResult* result) {
  if (!result) return;
  delete[] result->regions;
  result->regions = nullptr;
  result->region_count = 0;
}

SonareError sonare_metering_dynamic_range(const float* samples, size_t length, int sample_rate,
                                          float window_sec, float hop_sec, float low_percentile,
                                          float high_percentile, SonareDynamicRangeResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  // window_sec / hop_sec keep the "0 = library default" convention (a 0-second
  // window is meaningless). The percentiles instead use a NEGATIVE sentinel for
  // "default" so that 0.0 is a real request for the 0th percentile (the true
  // minimum-RMS window); 0.0 previously meant "default" and made the 0th
  // percentile unreachable.
  SONARE_C_TRY
  const metering::DynamicRangeConfig cfg = metering::dynamic_range_config_from_public(
      window_sec, hop_sec, low_percentile, high_percentile);
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    return fill_dynamic_range_result(metering::dynamic_range(audio, cfg), out);
  });
  SONARE_C_CATCH
}

void sonare_free_dynamic_range_result(SonareDynamicRangeResult* result) {
  if (!result) return;
  delete[] result->window_rms_db;
  result->window_rms_db = nullptr;
  result->window_count = 0;
}

namespace {

SonareError validate_stereo_pair(const float* left, const float* right, size_t length,
                                 int sample_rate) {
  if (!left || !right) return SONARE_ERROR_INVALID_PARAMETER;
  // Validate both channels symmetrically: validate_audio_params checks the
  // pointer, length bounds, sample rate, and NaN/Inf samples. Checking only the
  // left channel would let bad right-channel data reach the metering callers.
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  return validate_audio_params(right, length, sample_rate);
}

}  // namespace

SonareError sonare_metering_stereo_correlation(const float* left, const float* right, size_t length,
                                               int sample_rate, float* out_value) {
  SONARE_C_API_ENTRY;
  if (!out_value) return SONARE_ERROR_INVALID_PARAMETER;
  *out_value = 0.0f;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  *out_value = metering::correlation(left, right, length);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_metering_stereo_width(const float* left, const float* right, size_t length,
                                         int sample_rate, float* out_value) {
  SONARE_C_API_ENTRY;
  if (!out_value) return SONARE_ERROR_INVALID_PARAMETER;
  *out_value = 0.0f;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
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
                                        int sample_rate, SonareVectorscopeResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  std::memset(out, 0, sizeof(*out));
  SONARE_C_TRY
  return fill_vectorscope_result(metering::vectorscope(left, right, length), out);
  SONARE_C_CATCH
}

SonareError sonare_metering_vectorscope_decimated(const float* left, const float* right,
                                                  size_t length, int sample_rate, size_t max_points,
                                                  SonareVectorscopeResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  std::memset(out, 0, sizeof(*out));
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
                                        int sample_rate, SonarePhaseScopeResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  std::memset(out, 0, sizeof(*out));
  SONARE_C_TRY
  return fill_phase_scope_result(metering::phase_scope(left, right, length), out);
  SONARE_C_CATCH
}

SonareError sonare_metering_phase_scope_decimated(const float* left, const float* right,
                                                  size_t length, int sample_rate, size_t max_points,
                                                  SonarePhaseScopeResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  std::memset(out, 0, sizeof(*out));
  SONARE_C_TRY
  return fill_phase_scope_result(metering::phase_scope(left, right, length, max_points), out);
  SONARE_C_CATCH
}

void sonare_free_phase_scope_result(SonarePhaseScopeResult* result) {
  if (!result) return;
  delete[] result->points;
  result->points = nullptr;
  result->point_count = 0;
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
  out->n_fft = result.n_fft;
  out->sample_rate = result.sample_rate;
  out->bin_count = result.frequencies.size();
  if (out->bin_count == 0) return SONARE_OK;
  out->frequencies = copy_vector(result.frequencies);
  out->magnitude = copy_vector(result.magnitude);
  out->power = copy_vector(result.power);
  out->db = copy_vector(result.db);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_metering_spectrum(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int apply_octave_smoothing, int octave_fraction,
                                     float db_ref, float db_amin, SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    return fill_spectrum_result(metering::spectrum(audio, cfg), out);
  });
}

SonareError sonare_metering_spectrum_frame(const float* samples, size_t length, int sample_rate,
                                           size_t frame_offset, int n_fft,
                                           int apply_octave_smoothing, int octave_fraction,
                                           float db_ref, float db_amin, SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
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
  err = validate_audio_params_window(samples, length, sample_rate, start, count);
  if (err != SONARE_OK) return err;

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
}

void sonare_free_spectrum_result(SonareSpectrumResult* result) {
  if (!result) return;
  delete[] result->frequencies;
  delete[] result->magnitude;
  delete[] result->power;
  delete[] result->db;
  result->frequencies = nullptr;
  result->magnitude = nullptr;
  result->power = nullptr;
  result->db = nullptr;
  result->bin_count = 0;
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
  if (!audio || !out_db) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_db = metering::peak_db(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_rms_db(const SonareAudio* audio, float* out_db) {
  SONARE_C_API_ENTRY;
  if (!audio || !out_db) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_db = metering::rms_db(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_dc_offset(const SonareAudio* audio, float* out_value) {
  SONARE_C_API_ENTRY;
  if (!audio || !out_value) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_value = metering::dc_offset(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_crest_factor_db(const SonareAudio* audio, float* out_db) {
  SONARE_C_API_ENTRY;
  if (!audio || !out_db) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_db = metering::crest_factor_db(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_silence_ratio(const SonareAudio* audio, float threshold_db,
                                       int frame_length, int hop_length, float* out_ratio) {
  SONARE_C_API_ENTRY;
  if (!audio || !out_ratio || !std::isfinite(threshold_db) || frame_length <= 0 || hop_length <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_ratio = metering::silence_ratio(audio->audio, threshold_db, frame_length, hop_length);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_true_peak_db(const SonareAudio* audio, int oversample_factor,
                                      float* out_db) {
  SONARE_C_API_ENTRY;
  if (!audio || !out_db) return SONARE_ERROR_INVALID_PARAMETER;
  *out_db = 0.0f;
  const int factor = oversample_factor == 0 ? 4 : oversample_factor;
  if (factor < 1 || factor > 16 || !is_power_of_two(factor)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  *out_db = metering::true_peak_db(audio->audio, factor);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_audio_detect_clipping(const SonareAudio* audio, float threshold,
                                         size_t min_region_samples, SonareClippingResult* out) {
  SONARE_C_API_ENTRY;
  if (!audio || !out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  SONARE_C_TRY
  const metering::ClippingParams params =
      metering::clipping_params_from_public(threshold, min_region_samples);
  return fill_clipping_result(
      metering::detect_clipping(audio->audio, params.threshold, params.min_region_samples), out);
  SONARE_C_CATCH
}

SonareError sonare_audio_dynamic_range(const SonareAudio* audio, float window_sec, float hop_sec,
                                       float low_percentile, float high_percentile,
                                       SonareDynamicRangeResult* out) {
  SONARE_C_API_ENTRY;
  if (!audio || !out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  SONARE_C_TRY
  const metering::DynamicRangeConfig cfg = metering::dynamic_range_config_from_public(
      window_sec, hop_sec, low_percentile, high_percentile);
  return fill_dynamic_range_result(metering::dynamic_range(audio->audio, cfg), out);
  SONARE_C_CATCH
}

SonareError sonare_audio_spectrum(const SonareAudio* audio, int n_fft, int apply_octave_smoothing,
                                  int octave_fraction, float db_ref, float db_amin,
                                  SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  if (!audio || !out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  SONARE_C_TRY
  return fill_spectrum_result(metering::spectrum(audio->audio, cfg), out);
  SONARE_C_CATCH
}

SonareError sonare_audio_spectrum_frame(const SonareAudio* audio, size_t frame_offset, int n_fft,
                                        int apply_octave_smoothing, int octave_fraction,
                                        float db_ref, float db_amin, SonareSpectrumResult* out) {
  SONARE_C_API_ENTRY;
  if (!audio || !out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  // The core reads the frame at the offset and zero-pads past the end, so unlike
  // the buffer form there is no window to copy and no out-of-range offset to
  // special-case.
  SONARE_C_TRY
  return fill_spectrum_result(metering::spectrum_frame(audio->audio, frame_offset, cfg), out);
  SONARE_C_CATCH
}

namespace {

SonareError validate_interleaved_audio(const float* samples, size_t frames, int channels) {
  if (channels <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (!samples && frames > 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  if (frames > std::numeric_limits<size_t>::max() / static_cast<size_t>(channels)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError fill_waveform_peaks_result(const metering::WaveformPeaksResult& result,
                                       SonareWaveformPeaksResult* out) {
  out->channels = result.channels;
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

SonareError sonare_waveform_peaks(const float* samples, size_t frames, int channels,
                                  size_t samples_per_bucket, SonareWaveformPeaksResult* out) {
  SONARE_C_API_ENTRY;
  if (!out || samples_per_bucket == 0) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  SonareError err = validate_interleaved_audio(samples, frames, channels);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  return fill_waveform_peaks_result(
      metering::waveform_peaks(samples, frames, channels, samples_per_bucket), out);
  SONARE_C_CATCH
}

SonareError sonare_waveform_peak_pyramid(const float* samples, size_t frames, int channels,
                                         const size_t* samples_per_bucket_levels,
                                         size_t level_count, SonareWaveformPeakPyramidResult* out) {
  SONARE_C_API_ENTRY;
  if (!out || !samples_per_bucket_levels || level_count == 0) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  SonareError err = validate_interleaved_audio(samples, frames, channels);
  if (err != SONARE_OK) return err;
  std::vector<size_t> levels(samples_per_bucket_levels, samples_per_bucket_levels + level_count);
  for (size_t level : levels) {
    if (level == 0) return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const auto pyramid = metering::waveform_peak_pyramid(samples, frames, channels, levels);
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
  result->channels = 0;
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
  cfg.reference_midi = ZeroIsDefault(reference_midi)
                           .checked(cfg.reference_midi, 0.0f,
                                    editing::pitch_editor::kMaxReferenceMidi, "reference_midi");
  return cfg;
}
#endif

}  // namespace

SonareError sonare_scale_quantize_midi(int root, uint16_t mode_mask, float reference_midi,
                                       float midi, float* out_quantized_midi) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate: the stub below returns without reaching
  // the real body, and a caller that frees or reads the slot on a failure code
  // would otherwise see whatever was there.
  if (!out_quantized_midi) return SONARE_ERROR_INVALID_PARAMETER;
  *out_quantized_midi = 0.0f;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!editing::pitch_editor::valid_scale_args(root, mode_mask) || !std::isfinite(midi)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, reference_midi));
  *out_quantized_midi = q.quantize_midi(midi);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(root, mode_mask, reference_midi, midi, out_quantized_midi);
#endif
}

SonareError sonare_scale_correction_semitones(int root, uint16_t mode_mask, float reference_midi,
                                              float midi, float* out_semitones) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate: the stub below returns without reaching
  // the real body, and a caller that frees or reads the slot on a failure code
  // would otherwise see whatever was there.
  if (!out_semitones) return SONARE_ERROR_INVALID_PARAMETER;
  *out_semitones = 0.0f;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!editing::pitch_editor::valid_scale_args(root, mode_mask) || !std::isfinite(midi)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, reference_midi));
  *out_semitones = q.correction_semitones(midi);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(root, mode_mask, reference_midi, midi, out_semitones);
#endif
}

SonareError sonare_scale_pitch_class_enabled(int root, uint16_t mode_mask, int pitch_class,
                                             int* out_enabled) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate: the stub below returns without reaching
  // the real body, and a caller that frees or reads the slot on a failure code
  // would otherwise see whatever was there.
  if (!out_enabled) return SONARE_ERROR_INVALID_PARAMETER;
  *out_enabled = 0;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (pitch_class < 0 || pitch_class > 11) return SONARE_ERROR_INVALID_PARAMETER;
  if (!editing::pitch_editor::valid_scale_args(root, mode_mask)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, 0.0f));
  *out_enabled = q.pitch_class_enabled(pitch_class) ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(root, mode_mask, pitch_class, out_enabled);
#endif
}
