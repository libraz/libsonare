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
/// @param low_percentile Lower percentile in [0,1]. Pass a NEGATIVE value (e.g.
///        -1.0f) for the library default (0.10). 0.0f is a real request for the
///        0th percentile (the minimum-RMS window), not the default.
/// @param high_percentile Upper percentile in [0,1]. Pass a NEGATIVE value (e.g.
///        -1.0f) for the library default (0.95). 0.0f is a real request for the
///        0th percentile, not the default.
/// @note low_percentile == high_percentile is accepted and yields a 0-LU range
///       (dynamic_range_db == 0); only an inverted pair (low > high) is rejected.
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
/// @warning This emits one point per input sample. For real-length audio that is
///          millions of points (hundreds of MB once marshalled across a binding).
///          Prefer @ref sonare_metering_vectorscope_decimated for a display-sized
///          point set.
SonareError sonare_metering_vectorscope(const float* left, const float* right, size_t length,
                                        int sample_rate, SonareVectorscopeResult* out);

/// @brief Display-sized mid/side vectorscope.
/// @param max_points Upper bound on the returned point count. Pass 0 (or a value
///        >= @p length) to get one point per sample, identical to
///        @ref sonare_metering_vectorscope. Otherwise the core deterministically
///        decimates the input to at most @p max_points points, keeping the
///        largest-radius sample of each contiguous bucket so transient stereo
///        peaks survive the down-sample.
SonareError sonare_metering_vectorscope_decimated(const float* left, const float* right,
                                                  size_t length, int sample_rate, size_t max_points,
                                                  SonareVectorscopeResult* out);

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
/// @warning Emits one point per input sample; see the warning on
///          @ref sonare_metering_vectorscope. Prefer
///          @ref sonare_metering_phase_scope_decimated for display use.
SonareError sonare_metering_phase_scope(const float* left, const float* right, size_t length,
                                        int sample_rate, SonarePhaseScopeResult* out);

/// @brief Display-sized phase-scope (Lissajous + summary stats).
/// @param max_points Upper bound on the returned point count. Pass 0 (or a value
///        >= @p length) for one point per sample. Otherwise the point cloud is
///        deterministically decimated to at most @p max_points points (keeping
///        the largest-radius sample per bucket). The summary stats
///        (@c correlation, @c average_abs_angle_rad, @c max_radius) are always
///        computed over the full-resolution signal and are unaffected by
///        @p max_points.
SonareError sonare_metering_phase_scope_decimated(const float* left, const float* right,
                                                  size_t length, int sample_rate, size_t max_points,
                                                  SonarePhaseScopeResult* out);

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

/// @brief Welch-averaged magnitude / power / dB spectrum over the WHOLE buffer.
/// @details This is NOT a single-frame snapshot. The signal is split into
///          Hann-windowed, 50%-overlapping @c n_fft-length frames whose power
///          spectra are averaged across the entire input (Welch's method), so the
///          result is a time-averaged spectrum rather than a single moment.
///          Transients are smeared by the averaging. For a true single-frame FFT
///          of one @c n_fft-length window, use @ref sonare_metering_spectrum_frame.
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

/// @brief True single-frame magnitude / power / dB spectrum (one Hann-windowed
///        @c n_fft-length FFT), for spectrum-analyzer "moment" snapshots that
///        must not be time-averaged like @ref sonare_metering_spectrum.
/// @param frame_offset Sample index where the analysis frame begins. The frame
///        spans [@p frame_offset, @p frame_offset + n_fft); samples past the end
///        of the buffer are zero-padded. Pass 0 for the first frame.
/// @param n_fft FFT size. Pass 0 for the library default (2048).
/// @param apply_octave_smoothing Non-zero applies fractional-octave smoothing
///        to magnitude (power and dB are rederived from the smoothed magnitude).
/// @param octave_fraction Smoothing fraction (e.g. 3 = 1/3-octave). Pass 0 for
///        the library default (3).
/// @param db_ref Linear reference for the dB conversion. Pass 0.0f for 1.0.
/// @param db_amin Linear floor used to avoid log(0). Pass 0.0f for the library
///        default (sonare::constants::kEpsilon).
SonareError sonare_metering_spectrum_frame(const float* samples, size_t length, int sample_rate,
                                           size_t frame_offset, int n_fft,
                                           int apply_octave_smoothing, int octave_fraction,
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
