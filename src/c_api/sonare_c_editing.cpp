/// @file sonare_c_editing.cpp
/// @brief C API entry points for ScaleQuantizer + offline metering (basic /
///        true peak / clipping / dynamic range / stereo / phase-scope /
///        spectrum). Implementations stay thin: they validate inputs, build
///        the C++ value type, and forward to the underlying module.
///        Heap-allocated result fields are released through dedicated
///        `sonare_free_*_result` entry points to mirror the rest of the C API.

#include <sonare/sonare_c.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "core/audio.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/phase_scope.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "metering/waveform.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

bool is_power_of_two(int v) { return v > 0 && (v & (v - 1)) == 0; }

}  // namespace

SonareError sonare_metering_peak_db(const float* samples, size_t length, int sample_rate,
                                    float* out_db) {
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::peak_db(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_rms_db(const float* samples, size_t length, int sample_rate,
                                   float* out_db) {
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::rms_db(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_crest_factor_db(const float* samples, size_t length, int sample_rate,
                                            float* out_db) {
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_db = metering::crest_factor_db(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_dc_offset(const float* samples, size_t length, int sample_rate,
                                      float* out_value) {
  if (!out_value) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_value = metering::dc_offset(audio);
    return SONARE_OK;
  });
}

SonareError sonare_metering_true_peak_db(const float* samples, size_t length, int sample_rate,
                                         int oversample_factor, float* out_db) {
  if (!out_db) return SONARE_ERROR_INVALID_PARAMETER;
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
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  float effective_threshold = threshold <= 0.0f ? 0.999f : threshold;
  size_t effective_min = min_region_samples == 0 ? 1u : min_region_samples;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    metering::ClippingResult result =
        metering::detect_clipping(audio, effective_threshold, effective_min);
    out->clipped_samples = result.clipped_samples;
    out->clipping_ratio = result.clipping_ratio;
    out->max_clipped_peak = result.max_clipped_peak;
    out->region_count = result.regions.size();
    if (!result.regions.empty()) {
      std::unique_ptr<SonareClippingRegion[]> tmp(new SonareClippingRegion[result.regions.size()]);
      for (size_t i = 0; i < result.regions.size(); ++i) {
        tmp[i].start_sample = result.regions[i].start_sample;
        tmp[i].end_sample = result.regions[i].end_sample;
        tmp[i].length = result.regions[i].length;
        tmp[i].peak = result.regions[i].peak;
      }
      out->regions = release_array(tmp);
    }
    return SONARE_OK;
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
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  // window_sec / hop_sec keep the "0 = library default" convention (a 0-second
  // window is meaningless). The percentiles instead use a NEGATIVE sentinel for
  // "default" so that 0.0 is a real request for the 0th percentile (the true
  // minimum-RMS window); 0.0 previously meant "default" and made the 0th
  // percentile unreachable.
  if (window_sec < 0.0f || hop_sec < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  metering::DynamicRangeConfig cfg;
  if (window_sec > 0.0f) cfg.window_sec = window_sec;
  if (hop_sec > 0.0f) cfg.hop_sec = hop_sec;
  // A negative value (e.g. -1) selects the library default; 0.0 .. 1.0 are taken
  // verbatim. Values > 1.0 are out of range. The core additionally validates the
  // [0,1] range, but we range-check here so the sentinel decode stays explicit.
  if (low_percentile >= 0.0f) {
    if (low_percentile > 1.0f) return SONARE_ERROR_INVALID_PARAMETER;
    cfg.low_percentile = low_percentile;
  }
  if (high_percentile >= 0.0f) {
    if (high_percentile > 1.0f) return SONARE_ERROR_INVALID_PARAMETER;
    cfg.high_percentile = high_percentile;
  }
  // The core accepts low == high (a valid 0-LU range), so only reject a strictly
  // inverted pair (low > high) here instead of the previous low >= high guard.
  if (cfg.low_percentile > cfg.high_percentile) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    metering::DynamicRangeResult result = metering::dynamic_range(audio, cfg);
    out->dynamic_range_db = result.dynamic_range_db;
    out->low_percentile_db = result.low_percentile_db;
    out->high_percentile_db = result.high_percentile_db;
    out->window_count = result.window_rms_db.size();
    if (!result.window_rms_db.empty()) {
      std::unique_ptr<float[]> tmp(new float[result.window_rms_db.size()]);
      std::memcpy(tmp.get(), result.window_rms_db.data(),
                  result.window_rms_db.size() * sizeof(float));
      out->window_rms_db = release_array(tmp);
    }
    return SONARE_OK;
  });
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
  if (!out_value) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_stereo_pair(left, right, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  *out_value = metering::correlation(left, right, length);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_metering_stereo_width(const float* left, const float* right, size_t length,
                                         int sample_rate, float* out_value) {
  if (!out_value) return SONARE_ERROR_INVALID_PARAMETER;
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
// args, applying the "0 = default, negative = error" convention. Returns false
// (and the error code via *err) on an invalid value.
bool decode_spectrum_config(int n_fft, int apply_octave_smoothing, int octave_fraction,
                            float db_ref, float db_amin, metering::SpectrumConfig* cfg,
                            SonareError* err) {
  if (n_fft < 0 || octave_fraction < 0 || db_ref < 0.0f || db_amin < 0.0f) {
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
  const size_t bytes = out->bin_count * sizeof(float);
  std::unique_ptr<float[]> freq(new float[out->bin_count]);
  std::unique_ptr<float[]> mag(new float[out->bin_count]);
  std::unique_ptr<float[]> pwr(new float[out->bin_count]);
  std::unique_ptr<float[]> db(new float[out->bin_count]);
  std::memcpy(freq.get(), result.frequencies.data(), bytes);
  std::memcpy(mag.get(), result.magnitude.data(), bytes);
  std::memcpy(pwr.get(), result.power.data(), bytes);
  std::memcpy(db.get(), result.db.data(), bytes);
  out->frequencies = release_array(freq);
  out->magnitude = release_array(mag);
  out->power = release_array(pwr);
  out->db = release_array(db);
  return SONARE_OK;
}

}  // namespace

SonareError sonare_metering_spectrum(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int apply_octave_smoothing, int octave_fraction,
                                     float db_ref, float db_amin, SonareSpectrumResult* out) {
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
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  std::memset(out, 0, sizeof(*out));
  metering::SpectrumConfig cfg;
  SonareError err = SONARE_OK;
  if (!decode_spectrum_config(n_fft, apply_octave_smoothing, octave_fraction, db_ref, db_amin, &cfg,
                              &err)) {
    return err;
  }
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    return fill_spectrum_result(metering::spectrum_frame(audio, frame_offset, cfg), out);
  });
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
  std::unique_ptr<float[]> min_values(new float[total]);
  std::unique_ptr<float[]> max_values(new float[total]);
  std::memcpy(min_values.get(), result.min.data(), total * sizeof(float));
  std::memcpy(max_values.get(), result.max.data(), total * sizeof(float));
  out->min = release_array(min_values);
  out->max = release_array(max_values);
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

editing::pitch_editor::ScaleQuantizerConfig make_scale_config(int root, uint16_t mode_mask,
                                                              float reference_midi) {
  editing::pitch_editor::ScaleQuantizerConfig cfg;
  cfg.root = root;
  cfg.mode_mask = mode_mask;
  if (reference_midi > 0.0f) cfg.reference_midi = reference_midi;
  return cfg;
}

bool valid_scale_args(int root, uint16_t mode_mask) noexcept {
  return root >= 0 && root <= 11 && mode_mask != 0 && (mode_mask & ~uint16_t{0x0FFF}) == 0;
}

}  // namespace

SonareError sonare_scale_quantize_midi(int root, uint16_t mode_mask, float reference_midi,
                                       float midi, float* out_quantized_midi) {
  if (!out_quantized_midi) return SONARE_ERROR_INVALID_PARAMETER;
  if (!valid_scale_args(root, mode_mask)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, reference_midi));
  *out_quantized_midi = q.quantize_midi(midi);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_scale_correction_semitones(int root, uint16_t mode_mask, float reference_midi,
                                              float midi, float* out_semitones) {
  if (!out_semitones) return SONARE_ERROR_INVALID_PARAMETER;
  if (!valid_scale_args(root, mode_mask)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, reference_midi));
  *out_semitones = q.correction_semitones(midi);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_scale_pitch_class_enabled(int root, uint16_t mode_mask, int pitch_class,
                                             int* out_enabled) {
  if (!out_enabled) return SONARE_ERROR_INVALID_PARAMETER;
  if (pitch_class < 0 || pitch_class > 11) return SONARE_ERROR_INVALID_PARAMETER;
  if (!valid_scale_args(root, mode_mask)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  editing::pitch_editor::ScaleQuantizer q(make_scale_config(root, mode_mask, 0.0f));
  *out_enabled = q.pitch_class_enabled(pitch_class) ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
}
