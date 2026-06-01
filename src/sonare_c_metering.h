#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Metering - offline basic / true-peak / clipping / dynamic range
// ============================================================================

/// @brief Sample-peak in dBFS over the whole buffer.
SonareError sonare_metering_peak_db(const float* samples, size_t length, int sample_rate,
                                    float* out_db);

/// @brief RMS level in dBFS over the whole buffer.
SonareError sonare_metering_rms_db(const float* samples, size_t length, int sample_rate,
                                   float* out_db);

/// @brief Crest factor in dB (peak_db - rms_db).
SonareError sonare_metering_crest_factor_db(const float* samples, size_t length, int sample_rate,
                                            float* out_db);

/// @brief DC offset (mean) of the buffer in linear amplitude.
SonareError sonare_metering_dc_offset(const float* samples, size_t length, int sample_rate,
                                      float* out_value);

/// @brief Inter-sample (true) peak in dBFS. @p oversample_factor must be a
///        power of two in [1, 16]; pass 0 for the library default (4).
SonareError sonare_metering_true_peak_db(const float* samples, size_t length, int sample_rate,
                                         int oversample_factor, float* out_db);

/// @brief A single contiguous run of samples that clipped (or exceeded
///        @p threshold). Mirrors @c sonare::metering::ClippingRegion.
typedef struct {
  size_t start_sample;
  size_t end_sample;
  size_t length;
  float peak;
} SonareClippingRegion;

/// @brief Aggregated clipping report for a buffer. @c regions is heap-allocated
///        by libsonare; free with @ref sonare_free_clipping_result.
typedef struct {
  size_t clipped_samples;
  float clipping_ratio;
  float max_clipped_peak;
  SonareClippingRegion* regions;
  size_t region_count;
} SonareClippingResult;

/// @brief Detects clipped sample runs.
/// @param threshold Linear absolute threshold (typical: 0.999f). Pass 0.0f for
///                  the library default.
/// @param min_region_samples Minimum run length to report. Pass 0 for the
///                  library default (1).
SonareError sonare_metering_detect_clipping(const float* samples, size_t length, int sample_rate,
                                            float threshold, size_t min_region_samples,
                                            SonareClippingResult* out);

void sonare_free_clipping_result(SonareClippingResult* result);

/// @brief Offline dynamic-range result. @c window_rms_db is heap-allocated;
///        free with @ref sonare_free_dynamic_range_result.
typedef struct {
  float dynamic_range_db;
  float low_percentile_db;
  float high_percentile_db;
  float* window_rms_db;
  size_t window_count;
} SonareDynamicRangeResult;

/// @brief Sliding-window dynamic range (high_percentile_db - low_percentile_db).
/// @param window_sec Analysis window length in seconds (0.0 = library default 3.0).
/// @param hop_sec Hop between windows in seconds (0.0 = library default 1.0).
/// @param low_percentile Lower percentile in [0,1] (0.0 = library default 0.10).
/// @param high_percentile Upper percentile in [0,1] (0.0 = library default 0.95).
SonareError sonare_metering_dynamic_range(const float* samples, size_t length, int sample_rate,
                                          float window_sec, float hop_sec, float low_percentile,
                                          float high_percentile, SonareDynamicRangeResult* out);

void sonare_free_dynamic_range_result(SonareDynamicRangeResult* result);

// ============================================================================
// Metering - stereo / phase-scope / spectrum (offline)
// ============================================================================

/// @brief Pearson correlation in [-1, 1] between two equal-length mono
///        channels. Returns 0.0 when both channels are silent.
SonareError sonare_metering_stereo_correlation(const float* left, const float* right, size_t length,
                                               int sample_rate, float* out_value);

/// @brief Side / mid energy ratio in [0, ~]. 0 = pure mono, ~1 = wide stereo.
SonareError sonare_metering_stereo_width(const float* left, const float* right, size_t length,
                                         int sample_rate, float* out_value);

/// @brief Single point of a mid/side vectorscope.
typedef struct {
  float mid;
  float side;
} SonareVectorscopePoint;

/// @brief Heap-allocated vectorscope result. Free with
///        @ref sonare_free_vectorscope_result.
typedef struct {
  SonareVectorscopePoint* points;
  size_t point_count;
} SonareVectorscopeResult;

/// @brief Per-sample mid/side point series for the (left, right) buffer.
SonareError sonare_metering_vectorscope(const float* left, const float* right, size_t length,
                                        int sample_rate, SonareVectorscopeResult* out);

void sonare_free_vectorscope_result(SonareVectorscopeResult* result);

/// @brief Single phase-scope point. @c angle_rad is atan2(side, mid).
typedef struct {
  float mid;
  float side;
  float radius;
  float angle_rad;
} SonarePhaseScopePoint;

/// @brief Heap-allocated phase-scope result. @c points is freed via
///        @ref sonare_free_phase_scope_result.
typedef struct {
  SonarePhaseScopePoint* points;
  size_t point_count;
  float correlation;
  float average_abs_angle_rad;
  float max_radius;
} SonarePhaseScopeResult;

/// @brief Phase-scope (Lissajous + summary stats) for a stereo pair.
SonareError sonare_metering_phase_scope(const float* left, const float* right, size_t length,
                                        int sample_rate, SonarePhaseScopeResult* out);

void sonare_free_phase_scope_result(SonarePhaseScopeResult* result);

/// @brief Spectrum-meter result. All four arrays have length @c bin_count
///        (@c n_fft / 2 + 1). Free with @ref sonare_free_spectrum_result.
typedef struct {
  float* frequencies;
  float* magnitude;
  float* power;
  float* db;
  size_t bin_count;
  int n_fft;
  int sample_rate;
} SonareSpectrumResult;

/// @brief Single-frame magnitude / power / dB spectrum for the first
///        @c n_fft samples of @p samples.
/// @param n_fft FFT size. Pass 0 for the library default (2048).
/// @param apply_octave_smoothing Non-zero applies fractional-octave smoothing
///        to magnitude (power and dB are rederived from the smoothed magnitude).
/// @param octave_fraction Smoothing fraction (e.g. 3 = 1/3-octave). Pass 0 for
///        the library default (3).
/// @param db_ref Linear reference for the dB conversion. Pass 0.0f for 1.0.
/// @param db_amin Linear floor used to avoid log(0). Pass 0.0f for the library
///        default (sonare::constants::kEpsilon).
SonareError sonare_metering_spectrum(const float* samples, size_t length, int sample_rate,
                                     int n_fft, int apply_octave_smoothing, int octave_fraction,
                                     float db_ref, float db_amin, SonareSpectrumResult* out);

void sonare_free_spectrum_result(SonareSpectrumResult* result);

// ============================================================================
// Metering - multi-channel / standards-compliant LUFS (offline)
// ============================================================================

/// @brief ITU-R BS.1770-4 multi-channel loudness over an interleaved buffer.
/// @details Mirrors @ref sonare_lufs but accepts an interleaved multi-channel
///          signal (channel-summed K-weighting per the standard). @p samples
///          holds @p frames * @p channels values in channel-interleaved order.
/// @param samples Interleaved input buffer (may be null only when @p frames is 0).
/// @param frames Number of sample frames (per-channel length).
/// @param channels Channel count (must be > 0).
/// @param sample_rate Sample rate in Hz (must be > 0).
/// @param out Receives the integrated / momentary / short-term / loudness-range
///            fields. Must not be null. No heap pointers (no free required).
SonareError sonare_lufs_interleaved(const float* samples, size_t frames, int channels,
                                    int sample_rate, SonareLufsResult* out);

/// @brief EBU R128 / Tech 3342 Loudness Range (LRA) in LU for a mono buffer.
/// @details Multi-channel signals are not accepted here; use
///          @ref sonare_lufs_interleaved for a multi-channel measurement.
/// @param samples Mono input buffer (may be null only when @p length is 0).
/// @param length Number of samples.
/// @param sample_rate Sample rate in Hz (must be > 0).
/// @param out_lra Receives the loudness range in LU. Must not be null.
SonareError sonare_ebur128_loudness_range(const float* samples, size_t length, int sample_rate,
                                          float* out_lra);

#ifdef __cplusplus
}
#endif
